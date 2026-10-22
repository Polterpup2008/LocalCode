#include <catch2/catch_test_macros.hpp>

#include <limits>

#include "agent/FinalResponseBudget.h"

using namespace localcode;
using namespace std::chrono_literals;

namespace {
FinalResponseBudget::Sample measured_sample() {
    FinalResponseBudget::Sample sample;
    sample.completed_success = true;
    sample.timings_from_server = true;
    sample.evaluated_prompt_tokens = 1000;
    sample.prompt_ms = 1000;
    sample.decoded_tokens = 10;
    sample.decode_ms = 1000;
    sample.request_ms = 2100;
    sample.prompt_build_ms = 50;
    return sample;
}
}   // namespace

TEST_CASE("cold full-cap final cost can exceed a fixed quarter", "[response-budget][forecast]") {
    FinalResponseBudget budget(900s, 32768, 4096, 4096);
    CHECK(budget.reserve() == 225s);
    budget.observe(measured_sample(), 0ms);
    // 28672 cold input at1ms/token +2048 output at100ms/token
    // +100ms measured request overhead +50ms prompt build.
    CHECK(budget.reserve() == 233622ms);
    CHECK(budget.action_cutoff() == 666378ms);
    CHECK(budget.actions_open(666377ms));
    CHECK_FALSE(budget.actions_open(666378ms));
}

TEST_CASE("cached decoder samples independently shorten action time",
          "[response-budget][forecast]") {
    FinalResponseBudget budget(900s, 32768, 4096, 4096);
    budget.observe(measured_sample(), 0ms);
    auto cached = measured_sample();
    cached.evaluated_prompt_tokens = 0;
    cached.prompt_ms = 0;
    cached.decode_ms = 2000;
    cached.request_ms = 2200;
    budget.observe(cached, 1ms);
    CHECK(budget.reserve() == 438522ms);
    CHECK(budget.action_cutoff() == 461478ms);
    auto fast = measured_sample();
    fast.prompt_ms = 1;
    fast.decode_ms = 1;
    fast.request_ms = 2;
    fast.prompt_build_ms = 0;
    budget.observe(fast, 2ms);
    CHECK(budget.action_cutoff() == 461478ms);
    CHECK_FALSE(budget.actions_open(461478ms));
    budget.observe(fast, 461479ms);
    CHECK_FALSE(budget.actions_open(461479ms));
}

TEST_CASE("missing cold rate keeps fallback without inventing prefill",
          "[response-budget][forecast]") {
    FinalResponseBudget budget(900s, 32768, 4096, 4096);
    auto cached = measured_sample();
    cached.evaluated_prompt_tokens = 0;
    cached.prompt_ms = 0;
    cached.decode_ms = 100000;
    budget.observe(cached, 0ms);
    CHECK(budget.reserve() == 225s);
    CHECK(budget.actions_open(674999ms));
    auto prompt_only = measured_sample();
    prompt_only.decoded_tokens = 0;
    prompt_only.decode_ms = 0;
    prompt_only.request_ms = 1000;
    budget.observe(prompt_only, 1ms);
    CHECK(budget.reserve() == 900s);
    CHECK_FALSE(budget.actions_open(1ms));
}

TEST_CASE("an infeasible native forecast may close all first tools on short tasks",
          "[response-budget][forecast][tradeoff]") {
    for (const auto total : {30s, 60s, 120s}) {
        FinalResponseBudget budget(total, 32768, 4096, 4096);
        auto sample = measured_sample();
        sample.decoded_tokens = 1;
        budget.observe(sample, 0ms);
        CHECK(budget.reserve() == total);
        CHECK(budget.action_cutoff() == 0ms);
        CHECK_FALSE(budget.actions_open(0ms));
        CHECK(budget.begin_final(0ms));
        CHECK_FALSE(budget.begin_final(1ms));
    }
}

TEST_CASE("only completed successful native transports supply measurements",
          "[response-budget][admission]") {
    for (const bool failed : {false, true}) {
        FinalResponseBudget budget(900s, 32768, 4096, 4096);
        auto sample = measured_sample();
        sample.prompt_ms = 1000000;
        sample.completed_success = !failed;
        sample.timings_from_server = failed;
        budget.observe(sample, 0ms);
        CHECK(budget.reserve() == 225s);
        CHECK(budget.action_cutoff() == 675s);
    }
}

TEST_CASE("invalid native phases cannot poison a valid independent phase",
          "[response-budget][admission]") {
    FinalResponseBudget budget(900s, 32768, 4096, 4096);
    auto sample = measured_sample();
    sample.prompt_ms = std::numeric_limits<double>::quiet_NaN();
    sample.decoded_tokens = 1;
    sample.decode_ms = 1000;
    sample.request_ms = std::numeric_limits<double>::infinity();
    sample.prompt_build_ms = -100;
    budget.observe(sample, 0ms);
    CHECK(budget.reserve() == 225s);   // no cold rate yet
    sample = measured_sample();
    sample.decoded_tokens = 0;
    sample.decode_ms = 0;
    budget.observe(sample, 1ms);
    CHECK(budget.reserve() == 900s);   // the valid earlier decoder was retained
    CHECK_FALSE(budget.actions_open(1ms));
}

TEST_CASE("zero work with nonzero time is not a measured phase", "[response-budget][admission]") {
    FinalResponseBudget budget(900s, 32768, 4096, 4096);
    auto sample = measured_sample();
    sample.evaluated_prompt_tokens = 0;
    sample.prompt_ms = 1000;
    sample.decoded_tokens = 0;
    sample.decode_ms = 1000;
    budget.observe(sample, 0ms);
    CHECK(budget.reserve() == 225s);
}

TEST_CASE("finite huge timings saturate without overflow", "[response-budget][saturation]") {
    FinalResponseBudget budget(900s, 32768, 4096, 4096);
    auto sample = measured_sample();
    sample.evaluated_prompt_tokens = 1;
    sample.prompt_ms = std::numeric_limits<double>::max();
    budget.observe(sample, 0ms);
    CHECK(budget.reserve() == 900s);
    CHECK(budget.action_cutoff() == 0ms);
}

TEST_CASE("zero-work metadata cannot poison a previously measured forecast",
          "[response-budget][admission]") {
    FinalResponseBudget budget(900s, 32768, 4096, 4096);
    budget.observe(measured_sample(), 0ms);
    auto empty = measured_sample();
    empty.evaluated_prompt_tokens = 0;
    empty.prompt_ms = 0;
    empty.decoded_tokens = 0;
    empty.decode_ms = 0;
    empty.request_ms = 100000000;
    empty.prompt_build_ms = 100000000;
    empty.usage_from_server = true;
    empty.server_prompt_tokens = 32768;
    empty.estimated_prompt_tokens = 1;
    budget.observe(empty, 1ms);
    CHECK(budget.reserve() == 233622ms);
    CHECK(budget.action_cutoff() == 666378ms);
}

TEST_CASE("server estimator bias is provenance checked and context bounded",
          "[response-budget][forecast]") {
    FinalResponseBudget budget(900s, 32768, 4096, 4096);
    auto sample = measured_sample();
    sample.usage_from_server = true;
    sample.server_prompt_tokens = std::numeric_limits<int>::max();
    sample.estimated_prompt_tokens = 1;
    budget.observe(sample, 0ms);
    CHECK(budget.reserve() == 237718ms);   // cold input capped32768, not INT_MAX
    FinalResponseBudget invalid(900s, 32768, 4096, 4096);
    sample.server_prompt_tokens = 999;   // less than evaluated1000
    invalid.observe(sample, 0ms);
    CHECK(invalid.reserve() == 233622ms);
}

TEST_CASE("existing output cap and sub30second fallback remain unchanged",
          "[response-budget][forecast]") {
    FinalResponseBudget small_output(900s, 32768, 4096, 128);
    auto sample = measured_sample();
    sample.decode_ms = 10000;
    small_output.observe(sample, 0ms);
    CHECK(small_output.reserve() == 225s);   // 128k decode does not force2048k
    for (const int configured : {0, -1, 8192}) {
        FinalResponseBudget capped(900s, 32768, 4096, configured);
        capped.observe(measured_sample(), 0ms);
        CHECK(capped.reserve() == 233622ms);
    }
    FinalResponseBudget short_task(29999ms, 32768, 4096, 4096);
    sample.prompt_ms = 1000000;
    short_task.observe(sample, 0ms);
    CHECK(short_task.reserve() == 0ms);
    CHECK(short_task.actions_open(29998ms));
    CHECK_FALSE(short_task.actions_open(29999ms));
}

TEST_CASE("a final attempt closes authority without earning a retry", "[response-budget][state]") {
    FinalResponseBudget budget(900s, 32768, 4096, 4096);
    CHECK(budget.begin_final(675s));
    CHECK_FALSE(budget.actions_open(675s));
    auto sample = measured_sample();
    sample.prompt_ms = 1000000;
    budget.observe(sample, 676s);
    CHECK(budget.reserve() == 225s);
    CHECK_FALSE(budget.begin_final(676s));
    FinalResponseBudget expired(900s, 32768, 4096, 4096);
    CHECK_FALSE(expired.begin_final(900s));
}
