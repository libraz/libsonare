/// @file c_bare_refusal_retune_test.cpp
/// @brief The streaming retune C entries record a stated cause for every argument refusal.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

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
  REQUIRE(sonare_streaming_retune_prepare(retune, 48000.0, kBlock) == SONARE_OK);
  return retune;
}

}  // namespace

TEST_CASE("streaming retune refuses a NULL handle or out by name", "[c_api][retune]") {
  CHECK(sonare_streaming_retune_create(0.0f, 1.0f, 0, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");

  CHECK(sonare_streaming_retune_prepare(nullptr, 48000.0, kBlock) ==
        SONARE_ERROR_INVALID_PARAMETER);
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

TEST_CASE("streaming retune process_mono states why a block is refused", "[c_api][retune]") {
  std::vector<float> block(kBlock, 0.1f);

  SonareStreamingRetune* unprepared = nullptr;
  REQUIRE(sonare_streaming_retune_create(0.0f, 1.0f, 0, &unprepared) == SONARE_OK);
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
