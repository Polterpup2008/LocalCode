#pragma once
// ---------------------------------------------------------------------------
// Web access: search, fetch, download.
//
// A coding agent needs the web for the things it cannot know: the exact spelling
// of an API, what a compiler error actually means, whether a library release
// fixed a bug. It does not need the web to run arbitrary binaries, so downloads
// are governed by a trust policy: an executable or archive may only be fetched
// from a domain on the trusted list, and even then the user is asked first.
//
// Search providers are pluggable. The default needs no API key so the feature
// works out of the box; users who have a key for something better can configure
// it without a rebuild.
// ---------------------------------------------------------------------------
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../inference/HttpClient.h"
#include "../util/Result.h"

namespace localcode {

enum class SearchProviderKind {
    DuckDuckGoHtml,   // no API key required
    SearxngJson,      // self-hosted or public SearXNG instance
    BraveJson,        // needs an API key
    Custom,
};

std::string_view to_string(SearchProviderKind kind) noexcept;

struct SearchProvider {
    std::string        id;
    std::string        display_name;
    SearchProviderKind kind = SearchProviderKind::DuckDuckGoHtml;
    // {query} is substituted with the URL-encoded query.
    std::string        endpoint;
    std::string        api_key;
    bool               enabled = true;
};

struct SearchResult {
    std::string title;
    std::string url;
    std::string snippet;
};

struct FetchedPage {
    std::string url;
    std::string final_url;
    std::string title;
    std::string text;          // readable text, tags removed
    std::string content_type;
    int         status_code = 0;
    std::size_t original_bytes = 0;
    bool        truncated = false;
};

// Domains from which binaries and archives may be downloaded.
class TrustPolicy {
public:
    TrustPolicy();

    [[nodiscard]] bool is_trusted(const std::string& url) const;
    [[nodiscard]] static std::string host_of(const std::string& url);
    // True when the URL looks like something executable or an archive that
    // could contain one.
    [[nodiscard]] static bool is_executable_payload(const std::string& url);

    void add_trusted_host(std::string host);
    void remove_trusted_host(const std::string& host);
    [[nodiscard]] std::vector<std::string> trusted_hosts() const;

    void set_allow_any_host(bool allow) { allow_any_ = allow; }
    [[nodiscard]] bool allow_any_host() const { return allow_any_; }

    static const std::vector<std::string>& default_trusted_hosts();

private:
    std::vector<std::string> hosts_;
    bool                     allow_any_ = false;
};

class WebService {
public:
    explicit WebService(std::shared_ptr<HttpClient> http);

    void set_providers(std::vector<SearchProvider> providers);
    [[nodiscard]] std::vector<SearchProvider> providers() const;
    void set_active_provider(const std::string& id);
    [[nodiscard]] std::string active_provider() const;

    [[nodiscard]] TrustPolicy& trust() { return trust_; }
    [[nodiscard]] const TrustPolicy& trust() const { return trust_; }

    void set_enabled(bool enabled);
    [[nodiscard]] bool enabled() const;

    Result<std::vector<SearchResult>> search(const std::string& query, std::size_t max_results = 8,
                                             CancellationToken cancel = {});

    Result<FetchedPage> fetch(const std::string& url, std::size_t max_text_bytes = 60000,
                              CancellationToken cancel = {});

    // Caller is responsible for having obtained confirmation; this only enforces
    // the trust policy.
    Status download(const std::string& url, const std::filesystem::path& destination,
                    const std::function<void(std::uint64_t, std::uint64_t)>& on_progress = {},
                    CancellationToken cancel = {});

    // --- Exposed for testing --------------------------------------------
    static std::string html_to_text(std::string_view html, std::size_t max_bytes);
    static std::string extract_title(std::string_view html);
    static std::string url_encode(std::string_view text);
    static std::string decode_entities(std::string_view text);
    static std::vector<SearchResult> parse_duckduckgo_html(std::string_view html,
                                                           std::size_t max_results);
    static std::vector<SearchResult> parse_searxng_json(std::string_view body,
                                                        std::size_t max_results);
    static std::vector<SearchResult> parse_brave_json(std::string_view body,
                                                      std::size_t max_results);

private:
    mutable std::mutex          mutex_;
    std::shared_ptr<HttpClient> http_;
    std::vector<SearchProvider> providers_;
    std::string                 active_;
    TrustPolicy                 trust_;
    bool                        enabled_ = true;
};

}  // namespace localcode
