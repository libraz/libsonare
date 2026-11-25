/// @file c_bare_refusal_retune_test.cpp
/// @brief The streaming retune C entries record a stated cause for every argument refusal.

#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "streaming/realtime_limits.h"
#include "support/alloc_guard.h"

#if defined(SONARE_WITH_VOICE_CHANGER)

namespace {

std::string last_message() {
  const char* message = sonare_last_error_message();
  return message != nullptr ? message : "";
}

constexpr int kBlock = 64;

SonareStreamingRetune* prepared_retune() {
  SonareStreamingRetune* retune = nullptr;
  REQUIRE(sonare_streaming_retune_create(0.0f, 1.0f, 0, &retune) == SONARE_OK);
  REQUIRE(sonare_streaming_retune_prepare(retune, 48000, kBlock) == SONARE_OK);
  return retune;
}

}  // namespace

TEST_CASE("streaming retune refuses a NULL handle or out by name", "[c_api][retune]") {
  CHECK(sonare_streaming_retune_create(0.0f, 1.0f, 0, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");

  CHECK(sonare_streaming_retune_prepare(nullptr, 48000, kBlock) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "retune must not be NULL");
  CHECK(sonare_streaming_retune_reset(nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "retune must not be NULL");
  CHECK(sonare_streaming_retune_set_config(nullptr, 0.0f, 1.0f, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "retune must not be NULL");

  float semitones = 1.0f;
  float mix = 1.0f;
  int grain = 1;
  CHECK(sonare_streaming_retune_config(nullptr, &semitones, &mix, &grain) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "retune must not be NULL");

  float block[4] = {};
  CHECK(sonare_streaming_retune_process_mono(nullptr, block, 4) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "retune must not be NULL");

  int out = 1;
  CHECK(sonare_streaming_retune_grain_size_samples(nullptr, &out) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "retune must not be NULL");
  CHECK(sonare_streaming_retune_latency_samples(nullptr, &out) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "retune must not be NULL");

  SonareStreamingRetune* retune = prepared_retune();
  CHECK(sonare_streaming_retune_grain_size_samples(retune, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_grain_size_samples must not be NULL");
  CHECK(sonare_streaming_retune_latency_samples(retune, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_latency_samples must not be NULL");
  sonare_streaming_retune_destroy(retune);
}

TEST_CASE("streaming retune prepare uses the realtime sample-rate domain",
          "[c_api][retune][numeric]") {
  SonareStreamingRetune* retune = nullptr;
  REQUIRE(sonare_streaming_retune_create(0.0f, 1.0f, 0, &retune) == SONARE_OK);

  CHECK(sonare_streaming_retune_prepare(retune, 7999, kBlock) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "sample_rate must be an integer in [8000, 384000]");
  REQUIRE(sonare_streaming_retune_prepare(retune, 8000, kBlock) == SONARE_OK);
  REQUIRE(sonare_streaming_retune_prepare(retune, 384000, kBlock) == SONARE_OK);

  int grain_before = 0;
  int latency_before = 0;
  REQUIRE(sonare_streaming_retune_grain_size_samples(retune, &grain_before) == SONARE_OK);
  REQUIRE(sonare_streaming_retune_latency_samples(retune, &latency_before) == SONARE_OK);
  CHECK(grain_before > 0);
  CHECK(sonare_streaming_retune_prepare(retune, 384001, kBlock) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "sample_rate must be an integer in [8000, 384000]");
  REQUIRE(sonare_last_refusal_subject_count() == 1);
  CHECK(std::string(sonare_last_refusal_subject(0)) == "sample_rate");

  CHECK(sonare_streaming_retune_prepare(retune, 7999, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "sample_rate must be an integer in [8000, 384000]");
  CHECK(sonare_streaming_retune_prepare(retune, 48000, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "max_block_size must be an integer in [1, 1048576]");
  REQUIRE(sonare_last_refusal_subject_count() == 1);
  CHECK(std::string(sonare_last_refusal_subject(0)) == "max_block_size");
  CHECK(sonare_streaming_retune_prepare(retune, 48000, sonare::kMaxRealtimeBlockSize + 1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "max_block_size must be an integer in [1, 1048576]");
  REQUIRE(sonare_last_refusal_subject_count() == 1);
  CHECK(std::string(sonare_last_refusal_subject(0)) == "max_block_size");

  int grain_after = 0;
  int latency_after = 0;
  REQUIRE(sonare_streaming_retune_grain_size_samples(retune, &grain_after) == SONARE_OK);
  REQUIRE(sonare_streaming_retune_latency_samples(retune, &latency_after) == SONARE_OK);
  CHECK(grain_after == grain_before);
  CHECK(latency_after == latency_before);
  sonare_streaming_retune_destroy(retune);
}

#if defined(SONARE_WITH_MIXING)
TEST_CASE("failed streaming retune re-prepare preserves the previous stage",
          "[c_api][retune][allocation]") {
  SonareStreamingRetune* retune = prepared_retune();
  SonareStreamingRetune* control = prepared_retune();
  SonareStreamingRetune* probe = prepared_retune();

  int grain_before = 0;
  int latency_before = 0;
  REQUIRE(sonare_streaming_retune_grain_size_samples(retune, &grain_before) == SONARE_OK);
  REQUIRE(sonare_streaming_retune_latency_samples(retune, &latency_before) == SONARE_OK);
  REQUIRE(latency_before > 0);

  std::vector<float> actual(kBlock);
  std::vector<float> expected(kBlock);
  constexpr float kWarmAmplitude = 0.2f;
  for (int64_t offset = 0; offset < 2 * static_cast<int64_t>(latency_before); offset += kBlock) {
    for (int i = 0; i < kBlock; ++i) {
      actual[i] = expected[i] = kWarmAmplitude * std::sin(static_cast<float>(offset + i) * 0.07f);
    }
    REQUIRE(sonare_streaming_retune_process_mono(retune, actual.data(), actual.size()) ==
            SONARE_OK);
    REQUIRE(sonare_streaming_retune_process_mono(control, expected.data(), expected.size()) ==
            SONARE_OK);
  }

  size_t allocation_count = 0;
  SonareError measured_error;
  {
    sonare::test::AllocationFailureAtGuard count_allocations(std::numeric_limits<size_t>::max());
    measured_error = sonare_streaming_retune_prepare(probe, 96000, 128);
    allocation_count = sonare::test::g_allocation_attempts.load(std::memory_order_relaxed);
  }
  REQUIRE(measured_error == SONARE_OK);
  REQUIRE(allocation_count > 0);
  sonare_streaming_retune_destroy(probe);

  SonareError error;
  {
    sonare::test::AllocationFailureAtGuard fail_scratch(allocation_count);
    error = sonare_streaming_retune_prepare(retune, 96000, 128);
  }
  CHECK(error == SONARE_ERROR_OUT_OF_MEMORY);

  int grain_after = 0;
  int latency_after = 0;
  REQUIRE(sonare_streaming_retune_grain_size_samples(retune, &grain_after) == SONARE_OK);
  REQUIRE(sonare_streaming_retune_latency_samples(retune, &latency_after) == SONARE_OK);
  CHECK(grain_after == grain_before);
  CHECK(latency_after == latency_before);

  constexpr float kTestAmplitude = 0.3f;
  for (size_t i = 0; i < actual.size(); ++i) {
    actual[i] = expected[i] = kTestAmplitude * std::cos(static_cast<float>(i) * 0.11f);
  }
  REQUIRE(sonare_streaming_retune_process_mono(retune, actual.data(), actual.size()) == SONARE_OK);
  REQUIRE(sonare_streaming_retune_process_mono(control, expected.data(), expected.size()) ==
          SONARE_OK);
  constexpr float kTolerance = kTestAmplitude * 32.0f * std::numeric_limits<float>::epsilon();
  REQUIRE(std::any_of(expected.begin(), expected.end(),
                      [](float sample) { return std::abs(sample) > kTolerance; }));
  for (size_t i = 0; i < actual.size(); ++i) {
    CHECK(std::abs(actual[i] - expected[i]) <= kTolerance);
  }

  sonare_streaming_retune_destroy(retune);
  sonare_streaming_retune_destroy(control);
}
#endif

TEST_CASE("streaming retune process_mono states why a block is refused", "[c_api][retune]") {
  std::vector<float> block(kBlock, 0.1f);

  SonareStreamingRetune* unprepared = nullptr;
  REQUIRE(sonare_streaming_retune_create(0.0f, 1.0f, 0, &unprepared) == SONARE_OK);
  CHECK(sonare_streaming_retune_process_mono(unprepared, nullptr, 0) == SONARE_OK);
  CHECK(sonare_streaming_retune_process_mono(unprepared, block.data(), block.size()) ==
        SONARE_ERROR_INVALID_STATE);
  CHECK(last_message() == "the retune stage has not been prepared");
  sonare_streaming_retune_destroy(unprepared);

  SonareStreamingRetune* retune = prepared_retune();
  CHECK(sonare_streaming_retune_process_mono(retune, nullptr, block.size()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "samples must not be NULL when num_samples is not 0");

  std::vector<float> longer(kBlock + 1, 0.1f);
  CHECK(sonare_streaming_retune_process_mono(retune, longer.data(), longer.size()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "num_samples must be an integer in [0, 64]");

  block[3] = std::numeric_limits<float>::quiet_NaN();
  CHECK(sonare_streaming_retune_process_mono(retune, block.data(), block.size()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "samples must contain only finite samples");
  sonare_streaming_retune_destroy(retune);
}

#endif  // defined(SONARE_WITH_VOICE_CHANGER)
