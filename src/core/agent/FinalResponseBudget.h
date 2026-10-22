#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace localcode {

// Per-run forecasting only. No clock, transport, cancellation source or model
// state lives here. The caller still owns the absolute task deadline and must
// re-arm/check its action token before dispatching pending tools.
class FinalResponseBudget {
public:
    using Milliseconds = std::chrono::milliseconds;

    struct Sample {
        // True only after a complete successful discovery transport, before
        // user/hard/phase cancellation. Cancelled/truncated/error replies are
        // not measurements even if their partial result contains native fields.
        bool completed_success = false;
        bool timings_from_server = false;
        int evaluated_prompt_tokens = 0;
        double prompt_ms = 0;
        int decoded_tokens = 0;
        double decode_ms = 0;
        double request_ms = 0;        // local request only, excludes prompt build
        double prompt_build_ms = 0;   // local build only, added once
        bool usage_from_server = false;
        int server_prompt_tokens = 0;
        int estimated_prompt_tokens = 0;
    };

    FinalResponseBudget(Milliseconds total, int context_size, int reserved_output,
                        int configured_output)
        : total_(std::max(Milliseconds{0}, total)),
          context_size_(std::max(0, context_size)),
          input_capacity_(context_size_ - std::clamp(reserved_output, 0, context_size_)),
          output_cap_(configured_output > 0 ? std::min(configured_output, 2048) : 2048),
          reserve_(total_ >= std::chrono::seconds{30} ? std::min(Milliseconds{240000}, total_ / 4)
                                                      : Milliseconds{0}),
          action_cutoff_(total_ - reserve_) {}

    [[nodiscard]] Milliseconds reserve() const { return reserve_; }

    [[nodiscard]] Milliseconds action_cutoff() const { return action_cutoff_; }

    [[nodiscard]] bool final_attempted() const { return final_attempted_; }

    // Elapsed time is supplied, making arithmetic/state tests deterministic.
    // A closure is irreversible, including when a later sample is faster.
    [[nodiscard]] bool actions_open(Milliseconds elapsed) {
        if (final_attempted_ || elapsed >= action_cutoff_) actions_closed_ = true;
        return !actions_closed_ && elapsed < total_;
    }

    bool begin_final(Milliseconds elapsed) {
        if (final_attempted_ || elapsed >= total_) return false;
        actions_closed_ = true;
        final_attempted_ = true;
        return true;
    }

    void observe(const Sample& sample, Milliseconds elapsed) {
        if (final_attempted_ || !sample.completed_success || !sample.timings_from_server) return;
        const bool prompt_valid = valid_phase(sample.evaluated_prompt_tokens, sample.prompt_ms);
        const bool decode_valid = valid_phase(sample.decoded_tokens, sample.decode_ms);
        const bool prompt_measured = prompt_valid && sample.evaluated_prompt_tokens > 0;
        const bool decode_measured = decode_valid && sample.decoded_tokens > 0;
        // Empty/unknown timing metadata cannot relabel an entire request as
        // overhead or poison previously measured native rates.
        if (!prompt_measured && !decode_measured) return;
        // Independent phases: a cached prompt cannot invent cold prefill, but
        // its valid decoder can reveal a slowdown. An invalid phase must not
        // poison the other phase or manufacture scheduling overhead.
        if (prompt_measured)
            prompt_cost_ =
                std::max(prompt_cost_, sample.prompt_ms / sample.evaluated_prompt_tokens);
        if (decode_measured)
            decode_cost_ = std::max(decode_cost_, sample.decode_ms / sample.decoded_tokens);
        if (prompt_valid && decode_valid && finite_nonnegative(sample.request_ms))
            overhead_ = std::max(
                overhead_, std::max(0.0, sample.request_ms - sample.prompt_ms - sample.decode_ms));
        if (finite_nonnegative(sample.prompt_build_ms))
            build_cost_ = std::max(build_cost_, sample.prompt_build_ms);
        if (sample.usage_from_server && sample.estimated_prompt_tokens > 0 &&
            sample.server_prompt_tokens >= std::max(0, sample.evaluated_prompt_tokens)) {
            const auto bias = static_cast<std::int64_t>(sample.server_prompt_tokens) -
                              sample.estimated_prompt_tokens;
            positive_bias_ = std::max(
                positive_bias_, static_cast<int>(std::clamp<std::int64_t>(bias, 0, context_size_)));
        }
        // Missing either native rate keeps the existing fixed fallback. Never
        // substitute total request time or local estimated token throughput.
        if (total_ >= std::chrono::seconds{30} && prompt_cost_ > 0 && decode_cost_ > 0) {
            const auto input = std::min<std::int64_t>(
                context_size_, static_cast<std::int64_t>(input_capacity_) + positive_bias_);
            double forecast = 0;
            add_bounded(forecast, static_cast<double>(input), prompt_cost_);
            add_bounded(forecast, output_cap_, decode_cost_);
            add_bounded(forecast, 1, overhead_);
            add_bounded(forecast, 1, build_cost_);
            const double rounded = std::ceil(forecast);
            const auto predicted = rounded >= static_cast<double>(total_.count())
                                       ? total_
                                       : Milliseconds{static_cast<std::int64_t>(rounded)};
            reserve_ = std::max(reserve_, predicted);
            action_cutoff_ = std::min(action_cutoff_, total_ - reserve_);
        }
        (void)actions_open(elapsed);
    }

private:
    static bool finite_nonnegative(double value) { return std::isfinite(value) && value >= 0; }

    static bool valid_phase(int tokens, double ms) {
        return tokens >= 0 && finite_nonnegative(ms) && (tokens == 0 ? ms == 0 : ms > 0);
    }

    // Divide before multiplying so even huge finite native measurements cannot
    // overflow into a chrono deadline. The forecast never exceeds the budget.
    void add_bounded(double& total, double count, double cost) const {
        const double cap = static_cast<double>(total_.count());
        if (count <= 0 || cost <= 0 || total >= cap) return;
        if (!std::isfinite(cost) || cost >= (cap - total) / count)
            total = cap;
        else
            total += count * cost;
    }

    Milliseconds total_;
    int context_size_;
    int input_capacity_;
    int output_cap_;
    Milliseconds reserve_;
    Milliseconds action_cutoff_;
    double prompt_cost_ = 0;
    double decode_cost_ = 0;
    double overhead_ = 0;
    double build_cost_ = 0;
    int positive_bias_ = 0;
    bool actions_closed_ = false;
    bool final_attempted_ = false;
};

}   // namespace localcode
