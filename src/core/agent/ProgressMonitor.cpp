#include "ProgressMonitor.h"

#include <algorithm>
#include <map>

#include "../util/Log.h"
#include "../util/Strings.h"

namespace localcode {
namespace {

constexpr std::size_t kHistoryDepth = 24;

// Recursively rebuilds the JSON with sorted keys and collapsed whitespace, so
// two calls that differ only cosmetically produce the same digest.
Json normalise(const Json& value) {
    if (value.is_object()) {
        std::map<std::string, Json> ordered;
        for (auto it = value.begin(); it != value.end(); ++it) {
            ordered.emplace(it.key(), normalise(it.value()));
        }
        Json out = Json::object();
        for (auto& [key, item] : ordered)
            out[key] = std::move(item);
        return out;
    }
    if (value.is_array()) {
        Json out = Json::array();
        for (const auto& item : value)
            out.push_back(normalise(item));
        return out;
    }
    if (value.is_string()) {
        // Collapse runs of whitespace: "cmake  --build ." and "cmake --build ."
        // are the same command.
        const auto text = value.get<std::string>();
        std::string collapsed;
        collapsed.reserve(text.size());
        bool in_space = false;
        for (char c : text) {
            const bool is_space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
            if (is_space) {
                if (!in_space) collapsed.push_back(' ');
                in_space = true;
            } else {
                collapsed.push_back(c);
                in_space = false;
            }
        }
        return strings::trim(collapsed);
    }
    return value;
}

}   // namespace

std::string_view to_string(ProgressVerdict verdict) noexcept {
    switch (verdict) {
        case ProgressVerdict::Fine: return "fine";
        case ProgressVerdict::RepeatedIdenticalCall: return "repeated_identical_call";
        case ProgressVerdict::RepeatedFailure: return "repeated_failure";
        case ProgressVerdict::Oscillating: return "oscillating";
        case ProgressVerdict::Stalled: return "stalled";
        case ProgressVerdict::Exhausted: return "exhausted";
    }
    return "fine";
}

void ProgressMonitor::reset() {
    history_.clear();
    failures_.clear();
    iterations_ = 0;
    warnings_ = 0;
    last_change_iteration_ = 0;
}

std::string ProgressMonitor::digest_arguments(const Json& arguments) {
    if (arguments.is_null()) return "{}";
    return normalise(arguments).dump();
}

std::string ProgressMonitor::digest_result(const std::string& summary) {
    // Strip the parts that vary run to run - timings, byte counts - so that two
    // identical failures compare equal.
    std::string out;
    out.reserve(summary.size());
    bool in_digits = false;
    for (char c : summary) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            if (!in_digits) out.push_back('#');
            in_digits = true;
            continue;
        }
        in_digits = false;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return strings::trim(out);
}

ProgressAssessment ProgressMonitor::observe(const std::string& tool_name, const Json& arguments,
                                            const std::string& result_summary, bool succeeded,
                                            bool changed_files, bool advanced_verification,
                                            std::optional<bool> source_novelty,
                                            std::optional<std::string> failure_identity) {
    ProgressObservation observation;
    observation.tool_name = tool_name;
    observation.arguments_digest = digest_arguments(arguments);
    // The trusted diagnostic identity must also distinguish actual outcomes
    // in the identical-call guard; a prose digest erases numeric exit codes.
    observation.result_digest =
        !succeeded && failure_identity ? *failure_identity : digest_result(result_summary);
    observation.succeeded = succeeded;
    observation.changed_files = changed_files;
    observation.at_unix_ms = now_unix_ms();

    // Successful new reads are progress in an investigation. Requiring an edit
    // here stopped a real read-only lifecycle audit after twelve varied calls.
    // Repeats and failures still go through the guards below, and the agent's
    // iteration/time budgets bound a long investigation.
    const bool discovery_tool = tool_name == "read_file" || tool_name == "read_file_range" ||
                                tool_name == "search_text" || tool_name == "search_files" ||
                                tool_name == "list_directory" || tool_name == "repo_overview" ||
                                tool_name == "repo_search" || tool_name == "repo_symbol" ||
                                tool_name == "repo_tests_for";
    const auto discovery_result = strings::to_lower(result_summary);
    const bool empty_result = discovery_result.empty() ||
                              discovery_result.find("no matches") != std::string::npos ||
                              discovery_result.find("0 match") != std::string::npos ||
                              discovery_result.find("no results") != std::string::npos ||
                              discovery_result.find("found 0 file") != std::string::npos;
    const bool new_discovery =
        discovery_tool && succeeded && !empty_result &&
        source_novelty.value_or(std::none_of(
            history_.begin(), history_.end(), [&](const ProgressObservation& previous) {
                return previous.tool_name == observation.tool_name &&
                       previous.arguments_digest == observation.arguments_digest &&
                       previous.result_digest == observation.result_digest;
            }));
    history_.push_back(observation);
    if (history_.size() > kHistoryDepth) history_.pop_front();

    // Settling a criterion is progress even though nothing on disk moved.
    if (changed_files || advanced_verification || new_discovery)
        last_change_iteration_ = iterations_;
    const auto failure_key = tool_name + "|" + failure_identity.value_or(observation.result_digest);
    if (!succeeded) {
        const auto& key = failure_key;
        failures_.push_back(key);
        if (failures_.size() > kHistoryDepth) failures_.erase(failures_.begin());
    }

    ProgressAssessment assessment;
    assessment.warning_count = warnings_;

    // --- Identical call, identical result ---------------------------------
    int identical = 0;
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        if (it->tool_name == observation.tool_name &&
            it->arguments_digest == observation.arguments_digest &&
            it->result_digest == observation.result_digest) {
            ++identical;
        } else if (identical > 0) {
            // Only count an unbroken run; an intervening different call means
            // the model is at least trying something.
            break;
        }
    }
    if (identical >= identical_threshold_ && !source_novelty.value_or(false)) {
        assessment.verdict = ProgressVerdict::RepeatedIdenticalCall;
        assessment.guidance =
            "You have now called `" + tool_name + "` with exactly the same arguments " +
            std::to_string(identical) +
            " times and received the same result each time. Repeating it again will not change "
            "anything.\n"
            "Stop and reconsider: what does that result actually tell you? Look at a different "
            "file, search for the symbol somewhere else, or check an assumption you have not "
            "checked yet. If you are stuck, say so and explain precisely what you tried.";
    }

    // --- The same failure over and over ------------------------------------
    if (assessment.verdict == ProgressVerdict::Fine && !succeeded) {
        const auto& key = failure_key;
        const auto count = std::count(failures_.begin(), failures_.end(), key);
        if (count >= failure_threshold_) {
            assessment.verdict = ProgressVerdict::RepeatedFailure;
            assessment.guidance =
                "The same failure has now happened " + std::to_string(count) +
                " times: " + strings::utf8_truncate(result_summary, 200) +
                "\nThe approach you are taking is not working. Do not try it again. Read the "
                "error text carefully and treat it as evidence: what is it actually saying is "
                "wrong? Investigate that specific thing before changing any more code.";
        }
    }

    // --- Oscillation: A, B, A, B ------------------------------------------
    if (assessment.verdict == ProgressVerdict::Fine && !source_novelty.has_value() &&
        history_.size() >= 4) {
        const auto& a = history_[history_.size() - 1];
        const auto& b = history_[history_.size() - 2];
        const auto& c = history_[history_.size() - 3];
        const auto& d = history_[history_.size() - 4];
        const bool oscillating = a.tool_name == c.tool_name && b.tool_name == d.tool_name &&
                                 a.arguments_digest == c.arguments_digest &&
                                 b.arguments_digest == d.arguments_digest &&
                                 a.arguments_digest != b.arguments_digest;
        if (oscillating) {
            assessment.verdict = ProgressVerdict::Oscillating;
            assessment.guidance =
                "You are alternating between two actions and undoing your own work. Neither "
                "version is fixing the problem, which means the cause is somewhere you have not "
                "looked yet.\n"
                "Stop editing. Gather evidence first: read the full error, inspect the "
                "surrounding code, or check how the failing symbol is declared elsewhere.";
        }
    }

    // --- Lots of turns, nothing to show for them --------------------------
    if (assessment.verdict == ProgressVerdict::Fine && iterations_ >= 12 &&
        iterations_ - last_change_iteration_ >= 10) {
        assessment.verdict = ProgressVerdict::Stalled;
        assessment.guidance =
            "You have taken " + std::to_string(iterations_) +
            " turns without new evidence, file changes, or a settled check. Review the "
            "evidence already gathered and answer the user's request if you can. If you "
            "are stuck, explain what you found and what is blocking you.";
    }

    if (assessment.verdict != ProgressVerdict::Fine) {
        ++warnings_;
        assessment.warning_count = warnings_;
        log_warn(LogChannel::Agent, "no progress detected",
                 {{"verdict", std::string{to_string(assessment.verdict)}},
                  {"tool", tool_name},
                  {"warnings", std::to_string(warnings_)}});

        if (warnings_ >= max_warnings_) {
            assessment.verdict = ProgressVerdict::Exhausted;
            assessment.should_stop = true;
            assessment.guidance =
                "The run has been stopped: after " + std::to_string(warnings_) +
                " warnings the same unproductive pattern is still repeating. Everything gathered "
                "so far is preserved - review it and tell the user what was learned and what is "
                "blocking progress.";
        }
    }
    return assessment;
}

}   // namespace localcode
