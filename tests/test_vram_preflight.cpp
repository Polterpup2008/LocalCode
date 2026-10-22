// ---------------------------------------------------------------------------
// Warning about video memory without pretending to know how much is needed.
//
// The real case: 5.29 GB of weights, 1.2 GB free, and the only sign of trouble
// was llama.cpp aborting a minute later. File size gives a rough weight
// budget for full offload, not an exact allocation. The tests hold the line
// on what must NOT be claimed: no verdict without a reading, no verdict when
// nothing is going to the GPU, and no "insufficient" derived from a guess about
// the context cache.
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "inference/VramPreflight.h"

using namespace localcode;

namespace {
constexpr std::uint64_t kGiB = 1024ull * 1024ull * 1024ull;
constexpr std::uint64_t kModel = 5687387750;   // Qwen3.5-9B Q4_K_M, 5.29 GiB
}   // namespace

TEST_CASE("the observed failure is caught before the runtime aborts", "[vram]") {
    // 1232 MiB free, as measured when the load actually failed.
    const auto check = check_vram(kModel, 999, 1232ull * 1024 * 1024, 8188ull * 1024 * 1024, 32);
    REQUIRE(check.verdict == VramVerdict::Insufficient);
    CHECK(check.worth_reporting());
    CHECK(check.message.find("5.3 GiB") != std::string::npos);
    CHECK(check.message.find("1.2 GiB") != std::string::npos);
    CHECK(check.message.find("not the runtime allocation") != std::string::npos);
}

TEST_CASE("room to spare says nothing at all", "[vram]") {
    const auto check = check_vram(kModel, -1, 8ull * kGiB, 12ull * kGiB);
    CHECK(check.verdict == VramVerdict::Sufficient);
    CHECK_FALSE(check.worth_reporting());
    CHECK(check.message.empty());
}

TEST_CASE("weights that fit with almost nothing left over are called tight", "[vram]") {
    // They fit, so this is not a refusal - but the cache has to go somewhere.
    const auto check = check_vram(4ull * kGiB, -1, 4ull * kGiB + 64 * 1024 * 1024, 8ull * kGiB);
    CHECK(check.verdict == VramVerdict::Tight);
    CHECK(check.worth_reporting());
    CHECK(check.message.find("may still fail") != std::string::npos);
}

TEST_CASE("no GPU reading produces no verdict", "[vram]") {
    // A machine with no NVIDIA driver is not a machine that is short of memory.
    const auto check = check_vram(kModel, -1, std::nullopt, std::nullopt);
    CHECK(check.verdict == VramVerdict::Unknown);
    CHECK_FALSE(check.worth_reporting());
    CHECK(check.message.empty());
}

TEST_CASE("a CPU-only profile is not a video memory question", "[vram]") {
    // Zero layers on the device: it does not matter that only 128 MiB is free.
    const auto check = check_vram(kModel, 0, 128ull * 1024 * 1024, 8ull * kGiB);
    CHECK(check.verdict == VramVerdict::Unknown);
    CHECK_FALSE(check.worth_reporting());
}

TEST_CASE("partial GPU placement does not require the whole model in VRAM", "[vram]") {
    const auto check = check_vram(kModel, 1, 128ull * 1024 * 1024, 8ull * kGiB);
    CHECK(check.verdict == VramVerdict::Unknown);
    CHECK_FALSE(check.worth_reporting());
    CHECK(check.message.empty());
}

TEST_CASE("positive GPU counts need known layers to establish full offload", "[vram]") {
    // 32 repeating layers plus output: 32 is still partial, 33 requests all.
    CHECK(check_vram(kModel, 1, 128ull * 1024 * 1024, 8ull * kGiB, 32).verdict ==
          VramVerdict::Unknown);
    CHECK(check_vram(kModel, 32, 128ull * 1024 * 1024, 8ull * kGiB, 32).verdict ==
          VramVerdict::Unknown);
    CHECK(check_vram(kModel, 33, 128ull * 1024 * 1024, 8ull * kGiB, 32).verdict ==
          VramVerdict::Insufficient);
    CHECK(check_vram(kModel, 999, 128ull * 1024 * 1024, 8ull * kGiB).verdict ==
          VramVerdict::Unknown);
    CHECK(check_vram(kModel, 999, 128ull * 1024 * 1024, 8ull * kGiB, 0).verdict ==
          VramVerdict::Unknown);
    CHECK(check_vram(kModel, 1, 128ull * 1024 * 1024, 8ull * kGiB, 999).verdict ==
          VramVerdict::Unknown);
}

TEST_CASE("negative GPU counts match the launcher's full offload normalization", "[vram]") {
    const auto check = check_vram(kModel, -2, 128ull * 1024 * 1024, 8ull * kGiB, 32);
    CHECK(check.verdict == VramVerdict::Insufficient);
    CHECK(check.worth_reporting());
}

TEST_CASE("an unknown model size produces no verdict", "[vram]") {
    // The file could not be measured. That is not evidence of anything.
    const auto check = check_vram(0, -1, 1ull * kGiB, 8ull * kGiB);
    CHECK(check.verdict == VramVerdict::Unknown);
    CHECK_FALSE(check.worth_reporting());
}

TEST_CASE("exactly enough for the weights is not called insufficient", "[vram]") {
    // Equal is not over the file-size budget. Cache use is not estimated.
    const auto check = check_vram(4ull * kGiB, -1, 4ull * kGiB, 8ull * kGiB);
    CHECK(check.verdict != VramVerdict::Insufficient);
}

TEST_CASE("every verdict has a word", "[vram]") {
    CHECK(to_string(VramVerdict::Unknown) == "unknown");
    CHECK(to_string(VramVerdict::Insufficient) == "insufficient");
    CHECK(to_string(VramVerdict::Tight) == "tight");
    CHECK(to_string(VramVerdict::Sufficient) == "sufficient");
}
