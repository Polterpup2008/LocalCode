// ---------------------------------------------------------------------------
// Web tools: search, fetch, download.
// ---------------------------------------------------------------------------
#include "../platform/Paths.h"
#include "../util/Log.h"
#include "../web/WebService.h"
#include "ToolRegistry.h"
#include "ToolSupport.h"

namespace localcode {
namespace {

using namespace tool_support;

std::optional<ToolResult> require_web(ToolContext& context) {
    if (!context.web) {
        return ToolResult::failure(
            "web access unavailable",
            "Web access is not configured in this session. Answer from the project's own files "
            "and from what you know, and say plainly if you are unsure.");
    }
    if (!context.web->enabled()) {
        return ToolResult::failure(
            "web access is disabled",
            "The user has turned web access off in Settings. Do not treat this as an error; work "
            "from the repository instead.");
    }
    return std::nullopt;
}

// --- web_search ------------------------------------------------------------
class WebSearchTool final : public ITool {
public:
    std::string name() const override { return "web_search"; }
    bool requires_workspace() const override { return false; }

    std::string description() const override {
        return "Searches the web and returns titles, URLs and snippets. Use this for things that "
               "cannot be known from the project itself: the meaning of an unfamiliar compiler "
               "error, the current signature of a library API, whether a bug is a known issue. "
               "Follow a promising result with web_fetch to read the page. Do not use it for "
               "questions the workspace can answer - search the code instead.";
    }

    Json parameters_schema() const override {
        return schema::object(
            {{"query", schema::string_param(
                           "What to search for. Include the library, language or error code to "
                           "get useful results.")},
             {"max_results", schema::integer_param("How many results. Defaults to 6.", 1, 20)}},
            {"query"});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        if (auto refusal = require_web(context)) return *refusal;

        const auto query = arg_string(arguments, "query");
        const auto max_results = static_cast<std::size_t>(arg_int(arguments, "max_results", 6));

        auto results = context.web->search(query, max_results, context.cancel);
        if (!results) {
            auto result = ToolResult::failure("search failed", results.error().to_string());
            result.evidence.push_back(
                make_evidence(EvidenceKind::WebFetched, query, results.error().message, false));
            return result;
        }

        if (results->empty()) {
            return ToolResult::success(
                "No results for " + query,
                "The search returned no results for '" + query +
                    "'. Try different wording, or work from the project's own source.");
        }

        std::string report = std::to_string(results->size()) + " result(s) for '" + query + "':\n";
        Json structured = Json::array();
        int index = 1;
        for (const auto& item : *results) {
            report += "\n" + std::to_string(index++) + ". " + item.title + "\n   " + item.url +
                      "\n";
            if (!item.snippet.empty()) {
                report += "   " + strings::utf8_truncate(item.snippet, 400) + "\n";
            }
            structured.push_back(
                {{"title", item.title}, {"url", item.url}, {"snippet", item.snippet}});
        }
        report += "\nUse web_fetch on a URL above to read the page itself.";

        auto result = ToolResult::success(
            "Searched: " + strings::utf8_truncate(query, 60) + " (" +
                std::to_string(results->size()) + " results)",
            report);
        result.structured = std::move(structured);
        result.evidence.push_back(make_evidence(EvidenceKind::WebFetched, "search: " + query,
                                                std::to_string(results->size()) + " results",
                                                true));
        return result;
    }
};

// --- web_fetch -------------------------------------------------------------
class WebFetchTool final : public ITool {
public:
    std::string name() const override { return "web_fetch"; }
    bool requires_workspace() const override { return false; }

    std::string description() const override {
        return "Downloads a web page and returns its readable text with the markup removed. Use "
               "this to read documentation, a GitHub issue or a release note that a search "
               "turned up. Content that is not text is refused - use download_file for that.";
    }

    Json parameters_schema() const override {
        return schema::object(
            {{"url", schema::string_param("The http or https URL to read.")},
             {"max_bytes", schema::integer_param(
                               "Maximum characters of text to return. Defaults to 40000.", 1000,
                               200000)}},
            {"url"});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        if (auto refusal = require_web(context)) return *refusal;

        const auto url = arg_string(arguments, "url");
        const auto max_bytes = static_cast<std::size_t>(arg_int(arguments, "max_bytes", 40000));

        auto page = context.web->fetch(url, max_bytes, context.cancel);
        if (!page) {
            auto result = ToolResult::failure("fetch failed", page.error().to_string());
            result.evidence.push_back(
                make_evidence(EvidenceKind::WebFetched, url, page.error().message, false));
            return result;
        }

        std::string report;
        if (!page->title.empty()) report += page->title + "\n";
        report += page->final_url.empty() ? url : page->final_url;
        report += "\n" + std::string(40, '-') + "\n";
        report += page->text;
        if (page->truncated) report += "\n\n[the page was truncated]";

        auto result = ToolResult::success(
            "Fetched " + (page->title.empty() ? strings::utf8_truncate(url, 60) : page->title),
            report);
        result.truncated = page->truncated;
        result.evidence.push_back(make_evidence(
            EvidenceKind::WebFetched, url,
            std::to_string(page->status_code) + ", " + strings::human_bytes(page->original_bytes),
            true));
        return result;
    }
};

// --- download_file ---------------------------------------------------------
class DownloadFileTool final : public ITool {
public:
    std::string name() const override { return "download_file"; }
    bool mutates() const override { return true; }

    std::string description() const override {
        return "Downloads a file from the web into the workspace. Use this for source archives, "
               "headers or dependencies that a project genuinely needs. Executables and archives "
               "may only come from the trusted-source list (official vendor sites, official "
               "repositories and package registries), and the user is asked before any download "
               "runs.";
    }

    Json parameters_schema() const override {
        return schema::object(
            {{"url", schema::string_param("The URL to download.")},
             {"destination", schema::string_param(
                                 "Where to save it, relative to the workspace root.")}},
            {"url", "destination"});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        if (auto refusal = require_web(context)) return *refusal;

        const auto url = arg_string(arguments, "url");
        const auto destination_input = arg_string(arguments, "destination");

        auto destination = resolve(context, destination_input, PathAccess::Create);
        if (!destination) {
            return ToolResult::failure("cannot write " + destination_input,
                                       destination.error().message);
        }

        const auto host = TrustPolicy::host_of(url);
        const bool executable = TrustPolicy::is_executable_payload(url);
        const bool trusted = context.web->trust().is_trusted(url);

        if (executable && !trusted) {
            auto result = ToolResult::failure(
                "refused: untrusted download from " + host,
                "This looks like an executable or archive and " + host +
                    " is not on the trusted-source list, so it will not be downloaded.\n"
                    "Ask the user to add the host in Settings if it really is an official source, "
                    "or find the dependency through a package manager instead.");
            result.evidence.push_back(
                make_evidence(EvidenceKind::WebFetched, url, "blocked: untrusted host", false));
            return result;
        }

        // Every download is confirmed, whatever the host: this writes a file the
        // user did not ask for from a source they have not seen.
        ConfirmationRequest confirmation;
        confirmation.title = "Download this file?";
        confirmation.detail = url + "\n  to: " + destination->string() + "\n  host: " + host +
                              (trusted ? "  (trusted source)" : "  (not on the trusted list)");
        confirmation.reason = "downloading a file from the internet into the workspace";
        confirmation.rule_id = "web.download";

        if (!context.ask(confirmation)) {
            auto result = ToolResult::failure(
                "download not approved",
                "The user did not approve downloading " + url +
                    ". Continue without it, or explain what it was needed for.");
            result.evidence.push_back(
                make_evidence(EvidenceKind::WebFetched, url, "refused by user", false));
            return result;
        }

        std::uint64_t last_reported = 0;
        auto progress = [&context, &last_reported](std::uint64_t received, std::uint64_t total) {
            if (!context.on_progress) return;
            if (received - last_reported < 1024 * 1024) return;   // report each megabyte
            last_reported = received;
            std::string message = "downloaded " + strings::human_bytes(received);
            if (total > 0) message += " of " + strings::human_bytes(total);
            context.on_progress(message);
        };

        auto status = context.web->download(url, *destination, progress, context.cancel);
        if (!status) {
            auto result = ToolResult::failure("download failed", status.error().to_string());
            result.evidence.push_back(
                make_evidence(EvidenceKind::WebFetched, url, status.error().message, false));
            return result;
        }

        std::error_code ec;
        const auto size = std::filesystem::file_size(*destination, ec);
        const auto relative = relative_to_workspace(context, *destination);
        context.workspace->record_agent_change(*destination, AgentChangeKind::Created);

        auto result = ToolResult::success(
            "Downloaded " + relative + " (" + strings::human_bytes(ec ? 0 : size) + ")",
            "Downloaded " + url + "\n  to " + relative + "\n  size: " +
                strings::human_bytes(ec ? 0 : size));
        result.evidence.push_back(make_evidence(EvidenceKind::WebFetched, url,
                                                "saved to " + relative, true));
        return result;
    }
};

}  // namespace

std::vector<ToolPtr> make_web_tools() {
    return {std::make_shared<WebSearchTool>(), std::make_shared<WebFetchTool>(),
            std::make_shared<DownloadFileTool>()};
}

}  // namespace localcode
