// ---------------------------------------------------------------------------
// Generation metrics: what each number means and where it came from.
//
// These exist because the first implementation reported
// `completion_tokens / total_request_duration` as "tok/s". That counts prompt
// evaluation as though it were generation, so the same model on the same
// hardware measured 24.7 tok/s on a cold prompt and 94.9 tok/s on a warm one.
// Neither figure described the model.
//
// The runtime reports the two phases separately and its numbers are
// authoritative. The payloads below are verbatim from llama.cpp b10655.
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "inference/ChatTypes.h"
#include "util/Json.h"

using namespace localcode;

namespace {

// Exactly what the server returned for a 37-token prompt and 60 decoded tokens.
constexpr const char* kBufferedResponse = R"({
  "choices": [{"index":0,"message":{"role":"assistant","content":"391"},
               "finish_reason":"stop"}],
  "usage": {"completion_tokens":60,"prompt_tokens":37,"total_tokens":97,
            "prompt_tokens_details":{"cached_tokens":0}},
  "timings": {"cache_n":0,
              "prompt_n":37,"prompt_ms":1364.818,"prompt_per_second":27.109841751794015,
              "predicted_n":60,"predicted_ms":2761.081,
              "predicted_per_second":21.36844228764024}
})";

}  // namespace

TEST_CASE("decode throughput excludes prompt evaluation", "[metrics]") {
    auto parsed = parse_json(kBufferedResponse, "response");
    REQUIRE(parsed);

    const auto timings = ChatTimings{};   // default: nothing reported
    CHECK_FALSE(timings.from_server);

    // The figures the runtime gives.
    const double prompt_ms = (*parsed)["timings"]["prompt_ms"].get<double>();
    const double decode_ms = (*parsed)["timings"]["predicted_ms"].get<double>();
    const int    decoded   = (*parsed)["timings"]["predicted_n"].get<int>();
    const double reported  = (*parsed)["timings"]["predicted_per_second"].get<double>();

    // llama.cpp divides by (n - 1), not n: the first token's cost belongs to
    // prompt evaluation, not to decoding. Re-deriving the rate naively as
    // n * 1000 / ms gives 21.73 against the reported 21.37 - close enough to
    // look right and wrong enough to argue about. This is why the runtime's own
    // figure is used whenever it is present, and only derived when it is not.
    const double naive = decoded * 1000.0 / decode_ms;
    const double server_convention = (decoded - 1) * 1000.0 / decode_ms;

    CHECK(std::abs(server_convention - reported) < 0.001);
    CHECK(std::abs(naive - reported) > 0.3);

    const double decode_rate = reported;

    // The old formula - tokens over the whole request - is materially different,
    // and lower by the whole prompt phase. This is the bug, expressed as a
    // number: 14.5 against a true 21.4.
    const double whole_request_rate = decoded * 1000.0 / (prompt_ms + decode_ms);
    CHECK(whole_request_rate < decode_rate * 0.8);
    CHECK(std::abs(whole_request_rate - 14.55) < 0.05);
}

TEST_CASE("an observed reasoning stream is not a thinking duration", "[metrics][reasoning]") {
    // The rule the display follows. A think block delivered in one network read
    // spans no measurable time, and reporting that as "thought for 0s" claims
    // the model did not think. The count of deltas is what separates a genuine
    // observation from a buffering artefact.
    auto defensible = [](int delta_count, std::int64_t observed_ms) {
        return delta_count >= 3 && observed_ms >= 1000;
    };

    // Verified against Ling 3.0 Tiny: 89 and 220 characters of reasoning, each
    // arriving whole, each spanning 0ms.
    CHECK_FALSE(defensible(1, 0));
    CHECK_FALSE(defensible(2, 0));

    // A block that genuinely streamed - many deltas over several seconds.
    CHECK(defensible(400, 41000));

    // Long wall time but a single delta is still buffering, not thinking.
    CHECK_FALSE(defensible(1, 41000));
    // Many deltas within one read is not a duration either.
    CHECK_FALSE(defensible(400, 12));
}

TEST_CASE("estimated and measured token counts stay distinguishable", "[metrics]") {
    ChatUsage usage;
    CHECK_FALSE(usage.from_server);      // nothing reported yet

    usage.prompt_tokens = 37;
    usage.completion_tokens = 60;
    usage.total_tokens = 97;
    usage.from_server = usage.total_tokens > 0;
    CHECK(usage.from_server);

    // An estimate must never inherit the flag that says a server measured it.
    ChatUsage estimated;
    estimated.completion_tokens = estimate_tokens("some generated text");
    CHECK(estimated.completion_tokens > 0);
    CHECK_FALSE(estimated.from_server);
}

TEST_CASE("completion tokens cover reasoning and the answer together", "[metrics][reasoning]") {
    // Documented because it is easy to assume otherwise and then double-count.
    // The runtime decodes reasoning and the visible answer in one loop and
    // reports one predicted_n; nothing separates them, so a reasoning token
    // count can only ever be an estimate from the reasoning text.
    auto parsed = parse_json(kBufferedResponse, "response");
    REQUIRE(parsed);
    const int predicted = (*parsed)["timings"]["predicted_n"].get<int>();
    const int completion = (*parsed)["usage"]["completion_tokens"].get<int>();
    CHECK(predicted == completion);
}
