#include "SkillRegistry.h"
#include "../platform/PathEncoding.h"

#include <algorithm>
#include <cctype>
#include <system_error>

#include "../inference/ChatTypes.h"   // estimate_tokens
#include "../platform/FileSystem.h"
#include "../platform/Paths.h"
#include "../util/Log.h"
#include "../util/Strings.h"

namespace localcode {
namespace {

constexpr std::size_t kMaxSkillBytes = 256 * 1024;

// Words too common to carry signal when matching a skill against a request.
bool is_stop_word(std::string_view word) {
    static const char* kStopWords[] = {
        "the",    "and",      "for",   "with",  "this",  "that",    "from",  "into",    "your",
        "you",    "are",      "was",   "were",  "have",  "has",     "had",   "not",     "but",
        "can",    "will",     "how",   "what",  "which", "when",    "where", "before",  "after",
        "actual", "actually", "file",  "files", "code",  "project", "user",  "without", "work",
        "read",   "run",      "find",  "una",   "les",   "des",     "que",   "pour",    "dans",
        "avec",   "est",      "sur",   "par",   "der",   "die",     "das",   "und",     "ist",
        "ein",    "eine",     "nicht",
    };
    for (const char* stop : kStopWords) {
        if (strings::iequals(word, stop)) return true;
    }
    return word.size() < 3;
}

std::vector<std::string> tokenize_words(std::string_view text) {
    std::vector<std::string> words;
    std::string current;
    for (char c : text) {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '_' || c == '+' || c == '#') {
            current.push_back(static_cast<char>(std::tolower(uc)));
        } else {
            if (!current.empty() && !is_stop_word(current)) words.push_back(current);
            current.clear();
        }
    }
    if (!current.empty() && !is_stop_word(current)) words.push_back(current);
    std::sort(words.begin(), words.end());
    words.erase(std::unique(words.begin(), words.end()), words.end());
    return words;
}

bool matches_trigger(std::string_view text, std::string_view trigger) {
    const auto needle = strings::to_lower(trigger);
    if (needle.empty()) return false;
    const auto identifier = [](char value) {
        return std::isalnum(static_cast<unsigned char>(value)) || value == '_';
    };
    std::size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string_view::npos) {
        const auto end = offset + needle.size();
        const bool left = offset == 0 || !identifier(text[offset - 1]);
        const bool right = end == text.size() || !identifier(text[end]);
        // An explicit prohibition is not a request for the prohibited procedure.
        const auto prefix = text.substr(0, offset);
        const bool prohibited =
            prefix.ends_with("do not ") || prefix.ends_with("don't ") || prefix.ends_with("never ");
        if (left && right && !prohibited) return true;
        offset = end;
    }
    return false;
}

}   // namespace

void SkillRegistry::clear() {
    std::lock_guard lock(mutex_);
    skills_.clear();
    errors_.clear();
}

void SkillRegistry::insert(Skill skill) {
    auto it = std::find_if(skills_.begin(), skills_.end(),
                           [&](const Skill& existing) { return existing.id == skill.id; });
    if (it != skills_.end())
        *it = std::move(skill);
    else
        skills_.push_back(std::move(skill));
}

Status SkillRegistry::load_directory(const std::filesystem::path& directory) {
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        return make_error(ErrorCode::NotFound, "skills directory not found: " + path_to_utf8(directory));
    }

    std::vector<std::filesystem::path> candidates;
    const auto direct = directory / "SKILL.md";
    if (std::filesystem::is_regular_file(direct, ec)) candidates.push_back(direct);

    for (const auto& entry : std::filesystem::directory_iterator(
             directory, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (!entry.is_directory()) continue;
        const auto file = entry.path() / "SKILL.md";
        if (std::filesystem::is_regular_file(file, ec)) candidates.push_back(file);
    }

    std::sort(candidates.begin(), candidates.end());

    std::lock_guard lock(mutex_);
    for (const auto& file : candidates) {
        auto text = fs::read_text(file, kMaxSkillBytes);
        if (!text) {
            errors_.push_back(path_to_utf8(file) + ": " + text.error().message);
            continue;
        }
        auto skill = parse_skill(text->content, file);
        if (!skill) {
            errors_.push_back(path_to_utf8(file) + ": " + skill.error().message);
            continue;
        }
        const auto problems = skill->validate();
        if (!problems.empty()) {
            errors_.push_back(path_to_utf8(file) + ": " + strings::join(problems, "; "));
            continue;
        }
        insert(std::move(*skill));
    }

    log_info(LogChannel::App, "skills loaded",
             {{"directory", path_to_utf8(directory)},
              {"count", std::to_string(skills_.size())},
              {"errors", std::to_string(errors_.size())}});
    return ok_status();
}

Status SkillRegistry::load_default_locations() {
    Status result = ok_status();
    const auto builtin = app_paths().builtin_skills_dir();
    std::error_code ec;
    if (std::filesystem::is_directory(builtin, ec)) {
        result = load_directory(builtin);
    } else {
        std::lock_guard lock(mutex_);
        errors_.push_back("built-in skills directory not found: " + path_to_utf8(builtin));
    }

    const auto user = app_paths().user_skills_dir();
    if (std::filesystem::is_directory(user, ec)) load_directory(user);
    return result;
}

const Skill* SkillRegistry::find(std::string_view id) const {
    std::lock_guard lock(mutex_);
    for (const auto& skill : skills_) {
        if (skill.id == id) return &skill;
    }
    return nullptr;
}

void SkillRegistry::set_enabled(std::string_view id, bool enabled) {
    std::lock_guard lock(mutex_);
    for (auto& skill : skills_) {
        if (skill.id == id) skill.enabled = enabled;
    }
}

Status SkillRegistry::save_user_skill(const Skill& skill) {
    const auto problems = skill.validate();
    if (!problems.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "skill did not validate: " + strings::join(problems, "; "));
    }

    const auto directory = app_paths().user_skills_dir() / skill.id;
    const auto file = directory / "SKILL.md";
    auto written = fs::write_text_atomic(file, serialize_skill(skill));
    if (!written) return written;

    Skill stored = skill;
    stored.source_path = file;
    {
        std::lock_guard lock(mutex_);
        insert(std::move(stored));
    }
    log_info(LogChannel::App, "user skill saved", {{"id", skill.id}, {"path", path_to_utf8(file)}});
    return ok_status();
}

std::vector<SkillMatch> SkillRegistry::select(std::string_view objective,
                                              std::string_view recent_context,
                                              std::size_t max_results) const {
    std::lock_guard lock(mutex_);

    const std::string haystack =
        strings::to_lower(std::string{objective} + "\n" + std::string{recent_context});
    const auto words = tokenize_words(haystack);

    std::vector<SkillMatch> always_on;
    std::vector<SkillMatch> scored;

    for (const auto& skill : skills_) {
        if (!skill.enabled || !skill.validated) continue;

        if (skill.always) {
            always_on.push_back({&skill, 1000.0 + skill.priority, "always enabled"});
            continue;
        }

        double score = 0.0;
        std::vector<std::string> reasons;

        // A trigger phrase appearing verbatim is the strongest signal.
        for (const auto& trigger : skill.triggers) {
            if (matches_trigger(haystack, trigger)) {
                score += 10.0 + static_cast<double>(strings::utf8_length(trigger)) * 0.2;
                reasons.push_back("trigger '" + trigger + "'");
            }
        }
        for (const auto& tag : skill.tags) {
            if (std::find_if(words.begin(), words.end(), [&](const std::string& w) {
                    return strings::iequals(w, tag);
                }) != words.end()) {
                score += 4.0;
                reasons.push_back("tag '" + tag + "'");
            }
        }
        // Weak signal: words shared with the description.
        const auto description_words = tokenize_words(skill.description);
        std::size_t overlap = 0;
        for (const auto& word : description_words) {
            if (std::find(words.begin(), words.end(), word) != words.end()) ++overlap;
        }
        // One generic shared word previously selected unrelated procedures (a
        // linker guide for "source files", a Git guide for "inspect"). Keep
        // description-only retrieval, but require two distinct content words.
        if (overlap >= 2) {
            score += static_cast<double>(overlap) * 0.75;
            if (reasons.empty()) reasons.push_back("description overlap");
        }

        if (score <= 0.0) continue;
        score += static_cast<double>(skill.priority) * 0.05;   // gentle tie-break
        scored.push_back({&skill, score, strings::join(reasons, ", ")});
    }

    std::sort(scored.begin(), scored.end(), [](const SkillMatch& a, const SkillMatch& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.skill->id < b.skill->id;
    });

    // The cap governs how many *relevant* skills are added. Always-on skills
    // are additional - otherwise a handful of them would crowd out the very
    // skill the task called for, which is the opposite of the intent.
    std::vector<SkillMatch> result = std::move(always_on);
    std::size_t added = 0;
    for (auto& match : scored) {
        if (added >= max_results) break;
        result.push_back(match);
        ++added;
    }
    return result;
}

std::string SkillRegistry::render(const std::vector<SkillMatch>& matches, int max_tokens) {
    if (matches.empty()) return {};

    const std::string header =
        "## Applicable procedures\n\n"
        "These are established procedures for the kind of work at hand. Follow them.\n\n";

    if (max_tokens <= 0) {
        std::string out = header;
        for (const auto& match : matches) {
            if (!match.skill) continue;
            out += match.skill->to_prompt_section();
            out += "\n";
        }
        return out;
    }

    // Spend the budget on the highest-scoring skills first. `matches` is already
    // ordered: always-on skills lead, then by score.
    std::string out = header;
    int spent = estimate_tokens(header);
    std::size_t included = 0;

    for (const auto& match : matches) {
        if (!match.skill) continue;

        const auto section = match.skill->to_prompt_section();
        const int cost = estimate_tokens(section);

        if (spent + cost <= max_tokens) {
            out += section;
            out += "\n";
            spent += cost;
            ++included;
            continue;
        }

        // It does not fit whole. Include the opening of it if there is room for
        // something meaningful, so the model at least knows the procedure exists
        // and what it is for.
        const int remaining = max_tokens - spent;
        if (remaining > 120) {
            auto trimmed = strings::utf8_truncate(section, static_cast<std::size_t>(remaining) * 3);
            // Cut back to a line boundary rather than mid-sentence.
            if (const auto newline = trimmed.find_last_of('\n');
                newline != std::string::npos && newline > trimmed.size() / 2) {
                trimmed.resize(newline);
            }
            out += trimmed;
            out +=
                "\n\n[this procedure was abridged to fit the context window; call "
                "list_skills for the rest]\n\n";
            ++included;
        }
        break;
    }

    if (included == 0) return {};
    return out;
}

}   // namespace localcode
