#include "WebService.h"

#include <algorithm>
#include <cctype>

#include "../util/Json.h"
#include "../util/Log.h"
#include "../util/Strings.h"

namespace localcode {
namespace {

// HTML elements whose contents are markup machinery, not readable text.
bool is_skipped_element(std::string_view name) {
    static const char* kSkipped[] = {"script", "style", "head", "noscript", "svg", "iframe",
                                     "template", "canvas"};
    for (const char* candidate : kSkipped) {
        if (strings::iequals(name, candidate)) return true;
    }
    return false;
}

// Elements that imply a line break when they close.
bool is_block_element(std::string_view name) {
    static const char* kBlocks[] = {"p",  "div", "br", "li", "tr", "h1", "h2", "h3", "h4",
                                    "h5", "h6",  "ul", "ol", "table", "section", "article",
                                    "header", "footer", "pre", "blockquote", "hr", "td", "th"};
    for (const char* candidate : kBlocks) {
        if (strings::iequals(name, candidate)) return true;
    }
    return false;
}

std::string collapse_blank_lines(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    int consecutive_newlines = 0;
    bool at_line_start = true;
    int trailing_spaces = 0;

    for (char c : text) {
        if (c == '\n') {
            if (++consecutive_newlines > 2) continue;
            trailing_spaces = 0;
            at_line_start = true;
            out.push_back('\n');
            continue;
        }
        if (c == ' ' || c == '\t') {
            if (at_line_start) continue;      // strip leading whitespace
            if (++trailing_spaces > 1) continue;  // collapse runs of spaces
            out.push_back(' ');
            continue;
        }
        consecutive_newlines = 0;
        trailing_spaces = 0;
        at_line_start = false;
        out.push_back(c);
    }
    return strings::trim(out);
}

}  // namespace

std::string_view to_string(SearchProviderKind kind) noexcept {
    switch (kind) {
        case SearchProviderKind::DuckDuckGoHtml: return "duckduckgo";
        case SearchProviderKind::SearxngJson:    return "searxng";
        case SearchProviderKind::BraveJson:      return "brave";
        case SearchProviderKind::Custom:         return "custom";
    }
    return "duckduckgo";
}

// ---------------------------------------------------------------------------
// TrustPolicy
// ---------------------------------------------------------------------------
const std::vector<std::string>& TrustPolicy::default_trusted_hosts() {
    // Official vendor domains, official source forges and the canonical package
    // registries. Deliberately conservative: a user can add to this, and does so
    // knowingly, from Settings.
    static const std::vector<std::string> kHosts = {
        // Source forges
        "github.com", "raw.githubusercontent.com", "objects.githubusercontent.com",
        "codeload.github.com", "gitlab.com", "git.kernel.org", "sourceware.org",
        // Package registries
        "pypi.org", "files.pythonhosted.org", "registry.npmjs.org", "crates.io",
        "static.crates.io", "index.crates.io", "api.nuget.org", "repo.maven.apache.org",
        "rubygems.org", "packagist.org", "conan.io", "center.conan.io",
        // Toolchains and libraries
        "cmake.org", "sqlite.org", "llvm.org", "releases.llvm.org", "gnu.org", "ftp.gnu.org",
        "nodejs.org", "go.dev", "golang.org", "dl.google.com", "rust-lang.org",
        "static.rust-lang.org", "forge.rust-lang.org", "qt.io", "download.qt.io",
        "boost.org", "archives.boost.io", "ffmpeg.org", "khronos.org", "vulkan.lunarg.com",
        "libsdl.org", "openssl.org", "zlib.net", "curl.se",
        // Vendors
        "microsoft.com", "aka.ms", "visualstudio.microsoft.com", "download.visualstudio.microsoft.com",
        "developer.nvidia.com", "apple.com", "developer.apple.com",
        // Documentation (fetch, not download, but harmless to trust)
        "docs.python.org", "developer.mozilla.org", "en.cppreference.com", "cppreference.com",
        "learn.microsoft.com", "docs.rs", "doc.rust-lang.org", "stackoverflow.com",
    };
    return kHosts;
}

TrustPolicy::TrustPolicy() : hosts_(default_trusted_hosts()) {}

std::string TrustPolicy::host_of(const std::string& url) {
    auto position = url.find("://");
    std::string_view rest{url};
    if (position != std::string::npos) rest.remove_prefix(position + 3);

    const auto slash = rest.find_first_of("/?#");
    if (slash != std::string_view::npos) rest = rest.substr(0, slash);

    const auto at = rest.find('@');   // strip userinfo
    if (at != std::string_view::npos) rest.remove_prefix(at + 1);

    const auto colon = rest.rfind(':');
    if (colon != std::string_view::npos &&
        rest.find_first_not_of("0123456789", colon + 1) == std::string_view::npos) {
        rest = rest.substr(0, colon);
    }
    return strings::to_lower(rest);
}

bool TrustPolicy::is_trusted(const std::string& url) const {
    if (allow_any_) return true;
    const auto host = host_of(url);
    if (host.empty()) return false;
    for (const auto& trusted : hosts_) {
        if (host == trusted) return true;
        // Subdomains of a trusted host are trusted; "notgithub.com" is not.
        if (host.size() > trusted.size() &&
            host.compare(host.size() - trusted.size(), trusted.size(), trusted) == 0 &&
            host[host.size() - trusted.size() - 1] == '.') {
            return true;
        }
    }
    return false;
}

bool TrustPolicy::is_executable_payload(const std::string& url) {
    auto path = url;
    const auto query = path.find_first_of("?#");
    if (query != std::string::npos) path = path.substr(0, query);
    path = strings::to_lower(path);

    static const char* kExtensions[] = {".exe", ".msi", ".msix", ".bat", ".cmd", ".com", ".scr",
                                        ".ps1", ".psm1", ".vbs", ".js",  ".jar", ".dll", ".sys",
                                        ".sh",  ".bash", ".zsh", ".appimage", ".deb", ".rpm",
                                        ".pkg", ".dmg", ".zip", ".7z",  ".rar", ".tar", ".gz",
                                        ".tgz", ".xz",  ".bz2", ".zst", ".cab"};
    for (const char* extension : kExtensions) {
        if (strings::ends_with(path, extension)) return true;
    }
    return false;
}

void TrustPolicy::add_trusted_host(std::string host) {
    auto normalized = strings::to_lower(strings::trim(host));
    if (normalized.empty()) return;
    if (std::find(hosts_.begin(), hosts_.end(), normalized) == hosts_.end()) {
        hosts_.push_back(std::move(normalized));
    }
}

void TrustPolicy::remove_trusted_host(const std::string& host) {
    const auto normalized = strings::to_lower(strings::trim(host));
    hosts_.erase(std::remove(hosts_.begin(), hosts_.end(), normalized), hosts_.end());
}

std::vector<std::string> TrustPolicy::trusted_hosts() const { return hosts_; }

// ---------------------------------------------------------------------------
// WebService
// ---------------------------------------------------------------------------
WebService::WebService(std::shared_ptr<HttpClient> http) : http_(std::move(http)) {
    SearchProvider duckduckgo;
    duckduckgo.id = "duckduckgo";
    duckduckgo.display_name = "DuckDuckGo (no API key)";
    duckduckgo.kind = SearchProviderKind::DuckDuckGoHtml;
    duckduckgo.endpoint = "https://html.duckduckgo.com/html/?q={query}";
    providers_.push_back(std::move(duckduckgo));
    active_ = "duckduckgo";
}

void WebService::set_providers(std::vector<SearchProvider> providers) {
    std::lock_guard lock(mutex_);
    providers_ = std::move(providers);
    if (!providers_.empty()) {
        const bool active_still_valid =
            std::any_of(providers_.begin(), providers_.end(),
                        [this](const SearchProvider& p) { return p.id == active_; });
        if (!active_still_valid) active_ = providers_.front().id;
    }
}

std::vector<SearchProvider> WebService::providers() const {
    std::lock_guard lock(mutex_);
    return providers_;
}

void WebService::set_active_provider(const std::string& id) {
    std::lock_guard lock(mutex_);
    active_ = id;
}

std::string WebService::active_provider() const {
    std::lock_guard lock(mutex_);
    return active_;
}

void WebService::set_enabled(bool enabled) {
    std::lock_guard lock(mutex_);
    enabled_ = enabled;
}

bool WebService::enabled() const {
    std::lock_guard lock(mutex_);
    return enabled_;
}

std::string WebService::url_encode(std::string_view text) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::string WebService::decode_entities(std::string_view text) {
    static const std::vector<std::pair<std::string_view, std::string_view>> kNamed = {
        {"&amp;", "&"},   {"&lt;", "<"},    {"&gt;", ">"},   {"&quot;", "\""},
        {"&#39;", "'"},   {"&apos;", "'"},  {"&nbsp;", " "}, {"&mdash;", "\xE2\x80\x94"},
        {"&ndash;", "\xE2\x80\x93"}, {"&hellip;", "..."}, {"&rsquo;", "'"}, {"&lsquo;", "'"},
        {"&ldquo;", "\""}, {"&rdquo;", "\""}, {"&times;", "x"}, {"&copy;", "(c)"},
    };

    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] != '&') {
            out.push_back(text[i++]);
            continue;
        }
        const auto semicolon = text.find(';', i);
        if (semicolon == std::string_view::npos || semicolon - i > 10) {
            out.push_back(text[i++]);
            continue;
        }
        const auto entity = text.substr(i, semicolon - i + 1);

        bool handled = false;
        for (const auto& [name, replacement] : kNamed) {
            if (entity == name) {
                out.append(replacement);
                i = semicolon + 1;
                handled = true;
                break;
            }
        }
        if (handled) continue;

        // Numeric entities: &#123; and &#x7B;
        if (entity.size() > 3 && entity[1] == '#') {
            const bool hex = entity[2] == 'x' || entity[2] == 'X';
            const auto digits = entity.substr(hex ? 3 : 2, entity.size() - (hex ? 4 : 3));
            unsigned long code = 0;
            bool valid = !digits.empty();
            for (char c : digits) {
                const int value = std::isdigit(static_cast<unsigned char>(c))
                                      ? c - '0'
                                      : (hex && std::isxdigit(static_cast<unsigned char>(c))
                                             ? std::tolower(c) - 'a' + 10
                                             : -1);
                if (value < 0) { valid = false; break; }
                code = code * (hex ? 16 : 10) + static_cast<unsigned long>(value);
                if (code > 0x10FFFF) { valid = false; break; }
            }
            if (valid) {
                // Encode as UTF-8.
                if (code < 0x80) {
                    out.push_back(static_cast<char>(code));
                } else if (code < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                } else if (code < 0x10000) {
                    out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xF0 | (code >> 18)));
                    out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                }
                i = semicolon + 1;
                continue;
            }
        }
        out.push_back(text[i++]);
    }
    return out;
}

std::string WebService::extract_title(std::string_view html) {
    const auto lowered = strings::to_lower(html);
    const auto open = lowered.find("<title");
    if (open == std::string::npos) return {};
    const auto close_bracket = lowered.find('>', open);
    if (close_bracket == std::string::npos) return {};
    const auto close = lowered.find("</title", close_bracket);
    if (close == std::string::npos) return {};
    return strings::trim(decode_entities(html.substr(close_bracket + 1, close - close_bracket - 1)));
}

std::string WebService::html_to_text(std::string_view html, std::size_t max_bytes) {
    std::string out;
    out.reserve(std::min<std::size_t>(html.size(), max_bytes * 2));

    std::size_t i = 0;
    while (i < html.size()) {
        if (html[i] != '<') {
            out.push_back(html[i++]);
            continue;
        }

        // Comments and doctype/CDATA.
        if (html.compare(i, 4, "<!--") == 0) {
            const auto end = html.find("-->", i + 4);
            i = end == std::string_view::npos ? html.size() : end + 3;
            continue;
        }
        if (html.compare(i, 2, "<!") == 0) {
            const auto end = html.find('>', i);
            i = end == std::string_view::npos ? html.size() : end + 1;
            continue;
        }

        const auto tag_end = html.find('>', i);
        if (tag_end == std::string_view::npos) break;

        auto tag = html.substr(i + 1, tag_end - i - 1);
        const bool closing = !tag.empty() && tag.front() == '/';
        if (closing) tag.remove_prefix(1);

        std::size_t name_end = 0;
        while (name_end < tag.size() && !std::isspace(static_cast<unsigned char>(tag[name_end])) &&
               tag[name_end] != '/') {
            ++name_end;
        }
        const auto name = tag.substr(0, name_end);

        if (!closing && is_skipped_element(name)) {
            // Skip to the matching close tag; the contents are not readable text.
            const std::string close_tag = "</" + strings::to_lower(std::string{name});
            const auto lowered_rest = strings::to_lower(html.substr(tag_end));
            const auto close_position = lowered_rest.find(close_tag);
            if (close_position == std::string::npos) break;
            const auto after = html.find('>', tag_end + close_position);
            i = after == std::string_view::npos ? html.size() : after + 1;
            continue;
        }

        if (is_block_element(name)) out.push_back('\n');
        i = tag_end + 1;

        if (out.size() > max_bytes * 4) break;   // hard stop on pathological pages
    }

    auto text = collapse_blank_lines(decode_entities(out));
    if (text.size() > max_bytes) {
        text = strings::utf8_truncate(text, max_bytes);
        text += "\n\n[page truncated]";
    }
    return text;
}

std::vector<SearchResult> WebService::parse_duckduckgo_html(std::string_view html,
                                                            std::size_t max_results) {
    std::vector<SearchResult> results;

    // DuckDuckGo's HTML endpoint wraps each hit in an anchor with the class
    // "result__a" and the snippet in "result__snippet". Parsed by scanning for
    // those markers rather than with a full HTML parser, which is enough for a
    // page whose structure is this stable and this simple.
    std::size_t position = 0;
    while (results.size() < max_results) {
        const auto anchor = html.find("result__a", position);
        if (anchor == std::string_view::npos) break;

        const auto href_start = html.rfind("href=\"", anchor);
        if (href_start == std::string_view::npos) break;
        const auto url_start = href_start + 6;
        const auto url_end = html.find('"', url_start);
        if (url_end == std::string_view::npos) break;

        SearchResult result;
        result.url = decode_entities(html.substr(url_start, url_end - url_start));

        // Their redirector wraps the real URL in a uddg= parameter.
        const auto uddg = result.url.find("uddg=");
        if (uddg != std::string::npos) {
            auto encoded = result.url.substr(uddg + 5);
            const auto ampersand = encoded.find('&');
            if (ampersand != std::string::npos) encoded = encoded.substr(0, ampersand);
            // Percent-decode.
            std::string decoded;
            for (std::size_t k = 0; k < encoded.size(); ++k) {
                if (encoded[k] == '%' && k + 2 < encoded.size()) {
                    const auto hex = encoded.substr(k + 1, 2);
                    decoded.push_back(static_cast<char>(std::stoi(hex, nullptr, 16)));
                    k += 2;
                } else if (encoded[k] == '+') {
                    decoded.push_back(' ');
                } else {
                    decoded.push_back(encoded[k]);
                }
            }
            result.url = decoded;
        }
        if (strings::starts_with(result.url, "//")) result.url = "https:" + result.url;

        const auto title_start = html.find('>', anchor);
        if (title_start != std::string_view::npos) {
            const auto title_end = html.find("</a", title_start);
            if (title_end != std::string_view::npos) {
                result.title = strings::trim(
                    html_to_text(html.substr(title_start + 1, title_end - title_start - 1), 400));
            }
        }

        const auto snippet = html.find("result__snippet", url_end);
        const auto next_anchor = html.find("result__a", anchor + 9);
        if (snippet != std::string_view::npos &&
            (next_anchor == std::string_view::npos || snippet < next_anchor)) {
            const auto snippet_start = html.find('>', snippet);
            const auto snippet_end = html.find("</a", snippet_start);
            if (snippet_start != std::string_view::npos && snippet_end != std::string_view::npos) {
                result.snippet = strings::trim(html_to_text(
                    html.substr(snippet_start + 1, snippet_end - snippet_start - 1), 600));
            }
        }

        if (!result.url.empty() && !result.title.empty() &&
            strings::starts_with(result.url, "http")) {
            results.push_back(std::move(result));
        }
        position = anchor + 9;
    }
    return results;
}

std::vector<SearchResult> WebService::parse_searxng_json(std::string_view body,
                                                         std::size_t max_results) {
    std::vector<SearchResult> results;
    auto parsed = parse_json(body, "searxng response");
    if (!parsed || !parsed->contains("results")) return results;
    for (const auto& entry : (*parsed)["results"]) {
        if (results.size() >= max_results) break;
        SearchResult result;
        result.title = json_string(entry, "title");
        result.url = json_string(entry, "url");
        result.snippet = json_string(entry, "content");
        if (!result.url.empty()) results.push_back(std::move(result));
    }
    return results;
}

std::vector<SearchResult> WebService::parse_brave_json(std::string_view body,
                                                       std::size_t max_results) {
    std::vector<SearchResult> results;
    auto parsed = parse_json(body, "brave response");
    if (!parsed || !parsed->contains("web")) return results;
    const auto& web = (*parsed)["web"];
    if (!web.contains("results")) return results;
    for (const auto& entry : web["results"]) {
        if (results.size() >= max_results) break;
        SearchResult result;
        result.title = json_string(entry, "title");
        result.url = json_string(entry, "url");
        result.snippet = json_string(entry, "description");
        if (!result.url.empty()) results.push_back(std::move(result));
    }
    return results;
}

Result<std::vector<SearchResult>> WebService::search(const std::string& query,
                                                     std::size_t max_results,
                                                     CancellationToken cancel) {
    if (!enabled()) {
        return make_error(ErrorCode::PermissionDenied,
                          "web access is disabled in settings");
    }
    if (!http_) {
        return make_error(ErrorCode::NotSupported, "no HTTP transport is available in this build");
    }
    if (strings::trim(query).empty()) {
        return make_error(ErrorCode::InvalidArgument, "the search query is empty");
    }

    SearchProvider provider;
    {
        std::lock_guard lock(mutex_);
        auto it = std::find_if(providers_.begin(), providers_.end(),
                               [this](const SearchProvider& p) { return p.id == active_; });
        if (it == providers_.end()) {
            if (providers_.empty()) {
                return make_error(ErrorCode::NotFound, "no search provider is configured");
            }
            it = providers_.begin();
        }
        provider = *it;
    }
    if (!provider.enabled) {
        return make_error(ErrorCode::PermissionDenied,
                          "the search provider '" + provider.id + "' is disabled");
    }

    HttpRequest request;
    request.url = strings::replace_all(provider.endpoint, "{query}", url_encode(query));
    request.timeout = std::chrono::seconds(30);
    request.max_response_bytes = 4u * 1024 * 1024;
    // A browser-like user agent: the HTML endpoint serves a different, much
    // harder to parse page to unknown clients.
    request.headers = {
        {"User-Agent",
         "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
         "Chrome/124.0 Safari/537.36"},
        {"Accept", "text/html,application/xhtml+xml,application/json;q=0.9,*/*;q=0.8"},
        {"Accept-Language", "en-US,en;q=0.9"}};

    if (provider.kind == SearchProviderKind::BraveJson && !provider.api_key.empty()) {
        request.headers.emplace_back("X-Subscription-Token", provider.api_key);
        request.headers.emplace_back("Accept", "application/json");
    }

    auto response = http_->send(request, cancel);
    if (!response) return response.error();
    if (!response->ok()) {
        return make_error(ErrorCode::NetworkError,
                          "search returned HTTP " + std::to_string(response->status_code),
                          strings::utf8_truncate(response->body, 300));
    }

    std::vector<SearchResult> results;
    switch (provider.kind) {
        case SearchProviderKind::SearxngJson:
            results = parse_searxng_json(response->body, max_results);
            break;
        case SearchProviderKind::BraveJson:
            results = parse_brave_json(response->body, max_results);
            break;
        case SearchProviderKind::DuckDuckGoHtml:
        case SearchProviderKind::Custom:
        default:
            results = parse_duckduckgo_html(response->body, max_results);
            break;
    }

    log_info(LogChannel::Tool, "web search",
             {{"provider", provider.id},
              {"query", strings::utf8_truncate(query, 100)},
              {"results", std::to_string(results.size())}});
    return results;
}

Result<FetchedPage> WebService::fetch(const std::string& url, std::size_t max_text_bytes,
                                      CancellationToken cancel) {
    if (!enabled()) {
        return make_error(ErrorCode::PermissionDenied, "web access is disabled in settings");
    }
    if (!http_) {
        return make_error(ErrorCode::NotSupported, "no HTTP transport is available in this build");
    }
    if (!strings::starts_with(strings::to_lower(url), "http://") &&
        !strings::starts_with(strings::to_lower(url), "https://")) {
        return make_error(ErrorCode::InvalidArgument,
                          "only http and https URLs can be fetched: " + url);
    }

    HttpRequest request;
    request.url = url;
    request.timeout = std::chrono::seconds(45);
    request.max_response_bytes = 8u * 1024 * 1024;
    request.headers = {
        {"User-Agent", "Mozilla/5.0 (compatible; LocalCode/" LOCALCODE_VERSION ")"},
        {"Accept", "text/html,text/plain,application/json;q=0.9,*/*;q=0.8"},
        {"Accept-Language", "en-US,en;q=0.9"}};

    auto response = http_->send(request, cancel);
    if (!response) return response.error();

    FetchedPage page;
    page.url = url;
    page.final_url = response->final_url;
    page.status_code = response->status_code;
    page.content_type = response->header("content-type");
    page.original_bytes = response->body.size();

    if (!response->ok()) {
        return make_error(ErrorCode::NetworkError,
                          "fetching " + url + " returned HTTP " +
                              std::to_string(response->status_code),
                          strings::utf8_truncate(response->body, 300));
    }

    const auto content_type = strings::to_lower(page.content_type);
    if (content_type.find("html") != std::string::npos || content_type.empty()) {
        page.title = extract_title(response->body);
        page.text = html_to_text(response->body, max_text_bytes);
    } else if (content_type.find("json") != std::string::npos) {
        auto parsed = parse_json(response->body, "fetched JSON");
        page.text = parsed ? strings::utf8_truncate(parsed->dump(2), max_text_bytes)
                           : strings::utf8_truncate(response->body, max_text_bytes);
    } else if (content_type.find("text/") != std::string::npos ||
               content_type.find("xml") != std::string::npos) {
        page.text = strings::sanitize_utf8(strings::utf8_truncate(response->body, max_text_bytes));
    } else {
        return make_error(ErrorCode::NotSupported,
                          "the response is not text (" + page.content_type + ")",
                          "Use download_file to save binary content instead.");
    }

    page.truncated = response->truncated || page.text.size() >= max_text_bytes;
    log_info(LogChannel::Tool, "web fetch",
             {{"url", strings::utf8_truncate(url, 150)},
              {"status", std::to_string(page.status_code)},
              {"bytes", std::to_string(page.original_bytes)}});
    return page;
}

Status WebService::download(const std::string& url, const std::filesystem::path& destination,
                            const std::function<void(std::uint64_t, std::uint64_t)>& on_progress,
                            CancellationToken cancel) {
    if (!enabled()) {
        return make_error(ErrorCode::PermissionDenied, "web access is disabled in settings");
    }
    if (!http_) {
        return make_error(ErrorCode::NotSupported, "no HTTP transport is available in this build");
    }

    if (TrustPolicy::is_executable_payload(url) && !trust_.is_trusted(url)) {
        return make_error(
            ErrorCode::PermissionDenied,
            "refusing to download an executable or archive from an untrusted host: " +
                TrustPolicy::host_of(url),
            "Downloads of this kind are limited to the trusted-source list. Add the host in "
            "Settings if it really is an official source.");
    }

    HttpRequest request;
    request.url = url;
    request.timeout = std::chrono::minutes(30);
    request.headers = {{"User-Agent", "LocalCode/" LOCALCODE_VERSION}};

    log_info(LogChannel::Tool, "download started",
             {{"url", strings::utf8_truncate(url, 200)},
              {"destination", destination.string()}});
    return http_->download(request, destination, on_progress, cancel);
}

}  // namespace localcode
