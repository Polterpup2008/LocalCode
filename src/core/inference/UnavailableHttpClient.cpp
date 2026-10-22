#include "HttpClient.h"

namespace localcode {
namespace {
class UnavailableHttpClient final : public HttpClient {
    static Error unavailable(CancellationToken cancel) {
        if (cancel.cancelled()) return make_error(ErrorCode::Cancelled, "request cancelled");
        return make_error(
            ErrorCode::NotSupported,
            "No HTTP transport is available; rebuild with libcurl development files installed.");
    }

public:
    std::string backend_name() const override { return "unavailable"; }

    Result<HttpResponse> send(const HttpRequest&, CancellationToken cancel) override {
        return unavailable(cancel);
    }

    Result<HttpResponse> stream(const HttpRequest&, const HttpStreamCallback&,
                                CancellationToken cancel) override {
        return unavailable(cancel);
    }

    Status download(const HttpRequest&, const std::filesystem::path&,
                    const std::function<void(std::uint64_t, std::uint64_t)>&,
                    CancellationToken cancel) override {
        return unavailable(cancel);
    }
};
}   // namespace

std::shared_ptr<HttpClient> HttpClient::create() {
    return std::make_shared<UnavailableHttpClient>();
}
}   // namespace localcode
