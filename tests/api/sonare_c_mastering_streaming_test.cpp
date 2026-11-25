/// @file sonare_c_mastering_streaming_test.cpp
/// @brief Streaming mastering parameter-control C API tests.

#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "sonare_c_test_helpers.h"

#ifdef SONARE_WITH_MASTERING

namespace {

std::string last_message() {
  const char* message = sonare_last_error_message();
  return message != nullptr ? message : "";
}

}  // namespace

TEST_CASE("streaming chain zero-capacity flush follows its preparation contract",
          "[c_api][mastering][streaming]") {
  SonareStreamingMasteringChain* chain = nullptr;
  REQUIRE(sonare_streaming_mastering_chain_create(nullptr, nullptr, &chain) == SONARE_OK);
  size_t written = 9;
  CHECK(sonare_streaming_mastering_chain_flush_mono(chain, nullptr, 0, &written) ==
        SONARE_ERROR_INVALID_STATE);
  CHECK(written == 0);
  REQUIRE(sonare_streaming_mastering_chain_prepare(chain, 48000, 128, 2) == SONARE_OK);
  written = 9;
  CHECK(sonare_streaming_mastering_chain_flush_mono(chain, nullptr, 0, &written) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(written == 0);
  written = 9;
  CHECK(sonare_streaming_mastering_chain_flush_stereo(chain, nullptr, nullptr, 0, &written) ==
        SONARE_OK);
  CHECK(written == 0);
  sonare_streaming_mastering_chain_destroy(chain);
}

TEST_CASE("streaming chain create validates config and options before its output",
          "[c_api][mastering][streaming]") {
  CHECK(sonare_streaming_mastering_chain_create("{", nullptr, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message().find("config") != std::string::npos);

  CHECK(sonare_streaming_mastering_chain_create(R"({"missing.key":1})", nullptr, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message().find("config.missing.key") != std::string::npos);

  CHECK(sonare_streaming_mastering_chain_create(R"({"repair.declick.enabled":1})", nullptr,
                                                nullptr) == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(last_message().find("config.repair.declick") != std::string::npos);

  SonareStreamingMasteringChainOptions options;
  sonare_streaming_mastering_chain_options_init(&options);
  options.struct_size = 0;
  CHECK(sonare_streaming_mastering_chain_create(nullptr, &options, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message().find("options.struct_size") != std::string::npos);

  sonare_streaming_mastering_chain_options_init(&options);
  options.loudness_static_gain_db = std::numeric_limits<float>::infinity();
  CHECK(sonare_streaming_mastering_chain_create(R"({"loudness.enabled":1})", &options, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message().find("options.loudness_static_gain_db ") == 0);

  CHECK(sonare_streaming_mastering_chain_create(nullptr, nullptr, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");
}

TEST_CASE("streaming chain process checks readiness and prepared channels before samples",
          "[c_api][mastering][streaming]") {
  SonareStreamingMasteringChain* chain = nullptr;
  REQUIRE(sonare_streaming_mastering_chain_create(nullptr, nullptr, &chain) == SONARE_OK);
  float sample = std::numeric_limits<float>::quiet_NaN();

  REQUIRE(sonare_streaming_mastering_chain_process_mono(chain, &sample, 1) ==
          SONARE_ERROR_INVALID_STATE);
  CHECK(last_message() == "the chain has not been prepared");

  REQUIRE(sonare_streaming_mastering_chain_prepare(chain, 48000, 128, 2) == SONARE_OK);
  REQUIRE(sonare_streaming_mastering_chain_process_mono(chain, &sample, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "samples must match the prepared channel count");

  sonare_streaming_mastering_chain_destroy(chain);
}

TEST_CASE("streaming chain failed re-prepare preserves the current C handle state",
          "[c_api][mastering][streaming]") {
  const char* config =
      R"({"eq.tilt.enabled":1,"dynamics.multibandComp.enabled":1,"dynamics.multibandComp.crossover.cutoffsHz.0":120,"dynamics.multibandComp.crossover.cutoffsHz.1":6000,"maximizer.truePeakLimiter.enabled":1})";
  SonareStreamingMasteringChain* actual = nullptr;
  SonareStreamingMasteringChain* expected = nullptr;
  REQUIRE(sonare_streaming_mastering_chain_create(config, nullptr, &actual) == SONARE_OK);
  REQUIRE(sonare_streaming_mastering_chain_create(config, nullptr, &expected) == SONARE_OK);
  REQUIRE(sonare_streaming_mastering_chain_prepare(actual, 48000, 512, 2) == SONARE_OK);
  REQUIRE(sonare_streaming_mastering_chain_prepare(expected, 48000, 512, 2) == SONARE_OK);
  REQUIRE(sonare_streaming_mastering_chain_prepare(actual, 0, -1, 3) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("sample_rate ") == 0);

  std::vector<float> actual_left(512, 0.05f);
  std::vector<float> actual_right(512, 0.03f);
  std::vector<float> expected_left = actual_left;
  std::vector<float> expected_right = actual_right;
  REQUIRE(sonare_streaming_mastering_chain_process_stereo(actual, actual_left.data(),
                                                          actual_right.data(), 512) == SONARE_OK);
  REQUIRE(sonare_streaming_mastering_chain_process_stereo(expected, expected_left.data(),
                                                          expected_right.data(), 512) == SONARE_OK);

  size_t actual_count_before = 0;
  size_t expected_count_before = 0;
  REQUIRE(sonare_streaming_mastering_chain_non_finite_discard_count(actual, &actual_count_before) ==
          SONARE_OK);
  REQUIRE(sonare_streaming_mastering_chain_non_finite_discard_count(
              expected, &expected_count_before) == SONARE_OK);
  CHECK(actual_count_before == expected_count_before);

  CHECK(sonare_streaming_mastering_chain_prepare(actual, 8000, 512, 2) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message().find("cutoff") != std::string::npos);

  int max_block_size = 0;
  REQUIRE(sonare_streaming_mastering_chain_max_block_size(actual, &max_block_size) == SONARE_OK);
  CHECK(max_block_size == 512);

  std::fill(actual_left.begin(), actual_left.end(), 0.04f);
  std::fill(actual_right.begin(), actual_right.end(), 0.02f);
  std::fill(expected_left.begin(), expected_left.end(), 0.04f);
  std::fill(expected_right.begin(), expected_right.end(), 0.02f);
  REQUIRE(sonare_streaming_mastering_chain_process_stereo(actual, actual_left.data(),
                                                          actual_right.data(), 512) == SONARE_OK);
  REQUIRE(sonare_streaming_mastering_chain_process_stereo(expected, expected_left.data(),
                                                          expected_right.data(), 512) == SONARE_OK);
  CHECK(actual_left == expected_left);
  CHECK(actual_right == expected_right);

  size_t actual_count_after = 0;
  size_t expected_count_after = 0;
  REQUIRE(sonare_streaming_mastering_chain_non_finite_discard_count(actual, &actual_count_after) ==
          SONARE_OK);
  REQUIRE(sonare_streaming_mastering_chain_non_finite_discard_count(
              expected, &expected_count_after) == SONARE_OK);
  CHECK(actual_count_after == expected_count_after);

  sonare_streaming_mastering_chain_destroy(actual);
  sonare_streaming_mastering_chain_destroy(expected);
}

TEST_CASE("streaming realtime discard count clears its output on refusal",
          "[c_api][mastering][streaming]") {
  size_t count = 17;
  CHECK(sonare_streaming_mastering_chain_non_finite_discard_count(nullptr, &count) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(count == 0);
}

TEST_CASE("streaming mastering C API sets prepared realtime parameters",
          "[c_api][mastering][streaming]") {
  const char* config = R"({"maximizer.truePeakLimiter.enabled":1})";
  SonareStreamingMasteringChain* chain = nullptr;
  REQUIRE(sonare_streaming_mastering_chain_create(config, nullptr, &chain) == SONARE_OK);
  REQUIRE(chain != nullptr);

  REQUIRE(sonare_streaming_mastering_chain_set_parameter(
              chain, "maximizer.truePeakLimiter.ceilingDbtp", -12.0) == SONARE_ERROR_INVALID_STATE);
  REQUIRE(sonare_streaming_mastering_chain_prepare(chain, 48000, 128, 1) == SONARE_OK);
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(
              chain, "maximizer.truePeakLimiter.ceilingDbtp", -12.0) == SONARE_OK);

  std::vector<float> block(128, 0.8f);
  REQUIRE(sonare_streaming_mastering_chain_process_mono(chain, block.data(), block.size()) ==
          SONARE_OK);

  REQUIRE(sonare_streaming_mastering_chain_set_parameter(chain,
                                                         "maximizer.truePeakLimiter.lookaheadMs",
                                                         4.0) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(
              chain, "maximizer.truePeakLimiter.ceilingDbtp",
              std::numeric_limits<double>::quiet_NaN()) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(chain, "missing.parameter", -1.0) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_streaming_mastering_chain_destroy(chain);
}

TEST_CASE("streaming mastering C API validates setter handles and keys",
          "[c_api][mastering][streaming]") {
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(nullptr,
                                                         "maximizer.truePeakLimiter.ceilingDbtp",
                                                         -12.0) == SONARE_ERROR_INVALID_PARAMETER);

  SonareStreamingMasteringChain* chain = nullptr;
  REQUIRE(sonare_streaming_mastering_chain_create(nullptr, nullptr, &chain) == SONARE_OK);
  REQUIRE(chain != nullptr);
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(chain, nullptr, -12.0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(chain, "", -12.0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(
              chain, "maximizer.truePeakLimiter.ceilingDbtp",
              std::numeric_limits<double>::infinity()) == SONARE_ERROR_INVALID_PARAMETER);
  sonare_streaming_mastering_chain_destroy(chain);
}

TEST_CASE("sonare_streaming_loudness_gain matches the offline chain's applied gain",
          "[c_api][mastering][streaming]") {
  constexpr int kRate = 48000;
  std::vector<float> left(kRate);
  std::vector<float> right(kRate);
  for (size_t i = 0; i < left.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kRate);
    left[i] = 0.05f * std::sin(6.2831853f * 330.0f * t);
    right[i] = 0.04f * std::sin(6.2831853f * 440.0f * t);
  }
  const char* config = R"({"loudness.targetLufs":-16,"eq.tilt.tiltDb":2})";

  SonareStreamingLoudnessGain mono{};
  REQUIRE(sonare_streaming_loudness_gain(left.data(), left.size(), kRate, config, &mono) ==
          SONARE_OK);
  SonareMasteringChainResult mono_chain{};
  REQUIRE(sonare_master_audio(nullptr, left.data(), left.size(), kRate, config, nullptr,
                              &mono_chain) == SONARE_OK);
  CHECK(mono.loudness_static_gain_db == mono_chain.applied_gain_db);
  CHECK(std::isfinite(mono.integrated_lufs));
  CHECK(std::isfinite(mono.true_peak_dbtp));
  sonare_free_mastering_chain_result(&mono_chain);

  SonareStreamingLoudnessGain stereo{};
  REQUIRE(sonare_streaming_loudness_gain_stereo(left.data(), right.data(), left.size(), kRate,
                                                config, &stereo) == SONARE_OK);
  SonareMasteringChainStereoResult stereo_chain{};
  REQUIRE(sonare_master_audio_stereo(nullptr, left.data(), right.data(), left.size(), kRate, config,
                                     nullptr, &stereo_chain) == SONARE_OK);
  CHECK(stereo.loudness_static_gain_db == stereo_chain.applied_gain_db);
  sonare_free_mastering_chain_stereo_result(&stereo_chain);

  std::vector<float> silence(kRate, 0.0f);
  SonareStreamingLoudnessGain quiet{1.0f, 1.0f, 1.0f};
  REQUIRE(sonare_streaming_loudness_gain(silence.data(), silence.size(), kRate, config, &quiet) ==
          SONARE_OK);
  // Silence has no loudness to derive a gain from, so the gain is not measured.
  CHECK(std::isnan(quiet.loudness_static_gain_db));
  CHECK_FALSE(std::isfinite(quiet.integrated_lufs));
}

TEST_CASE("a silent input's streaming loudness gain feeds the chain as no gain",
          "[c_api][mastering][streaming]") {
  constexpr int kRate = 48000;
  constexpr size_t kBlock = 256;
  const char* config = R"({"loudness.enabled":1,"loudness.targetLufs":-14})";
  const std::vector<float> silence(kRate, 0.0f);
  SonareStreamingLoudnessGain quiet{};
  REQUIRE(sonare_streaming_loudness_gain(silence.data(), silence.size(), kRate, config, &quiet) ==
          SONARE_OK);
  REQUIRE(std::isnan(quiet.loudness_static_gain_db));
  REQUIRE(quiet.true_peak_dbtp == -std::numeric_limits<float>::infinity());

  // The result goes in as it is; an explicit 0 dB gain is the reference it must equal.
  auto render = [&](float gain_db, float peak_dbtp) {
    SonareStreamingMasteringChainOptions options;
    sonare_streaming_mastering_chain_options_init(&options);
    options.loudness_static_gain_db = gain_db;
    options.loudness_static_gain_peak_dbtp = peak_dbtp;
    SonareStreamingMasteringChain* chain = nullptr;
    REQUIRE(sonare_streaming_mastering_chain_create(config, &options, &chain) == SONARE_OK);
    REQUIRE(sonare_streaming_mastering_chain_prepare(chain, kRate, kBlock, 1) == SONARE_OK);
    std::vector<float> block(kBlock);
    for (size_t i = 0; i < kBlock; ++i) {
      block[i] = 0.2f * std::sin(6.2831853f * 440.0f * static_cast<float>(i) / kRate);
    }
    REQUIRE(sonare_streaming_mastering_chain_process_mono(chain, block.data(), block.size()) ==
            SONARE_OK);
    sonare_streaming_mastering_chain_destroy(chain);
    return block;
  };
  const std::vector<float> fed = render(quiet.loudness_static_gain_db, quiet.true_peak_dbtp);
  const std::vector<float> unity = render(0.0f, std::numeric_limits<float>::quiet_NaN());
  for (size_t i = 0; i < kBlock; ++i) {
    CHECK(std::abs(fed[i] - unity[i]) <= 1e-6f);
  }
  // The gain is read: 6 dB lifts the same block.
  const std::vector<float> lifted = render(6.0f, std::numeric_limits<float>::quiet_NaN());
  float largest_lift = 0.0f;
  for (size_t i = 0; i < kBlock; ++i) {
    largest_lift = std::max(largest_lift, std::abs(lifted[i]) - std::abs(unity[i]));
  }
  CHECK(largest_lift > 0.05f);
}

TEST_CASE("streaming mastering chain prepare resolves a zero block size to its default",
          "[c_api][mastering][streaming]") {
  SonareStreamingMasteringChain* chain = nullptr;
  REQUIRE(sonare_streaming_mastering_chain_create(nullptr, nullptr, &chain) == SONARE_OK);
  REQUIRE(chain != nullptr);
  CHECK(sonare_streaming_mastering_chain_prepare(chain, 48000, -1, 1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_streaming_mastering_chain_prepare(chain, 48000, 0, 1) == SONARE_OK);

  std::vector<float> block(128, 0.1f);
  CHECK(sonare_streaming_mastering_chain_process_mono(chain, block.data(), block.size()) ==
        SONARE_OK);
  std::vector<float> over(129, 0.1f);
  CHECK(sonare_streaming_mastering_chain_process_mono(chain, over.data(), over.size()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  sonare_streaming_mastering_chain_destroy(chain);
}

TEST_CASE("sonare_eq_create resolves a zero block size to its default",
          "[c_api][mastering][streaming]") {
  SonareEq* refused_eq = nullptr;
  CHECK(sonare_eq_create(48000, -1, &refused_eq) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(refused_eq == nullptr);
  SonareEq* eq = nullptr;
  REQUIRE(sonare_eq_create(48000, 0, &eq) == SONARE_OK);
  REQUIRE(eq != nullptr);

  std::vector<float> left(512, 0.1f);
  std::vector<float> right(512, 0.1f);
  float* channels[] = {left.data(), right.data()};
  CHECK(sonare_eq_process(eq, channels, 2, 512) == SONARE_OK);
  std::vector<float> over_left(513, 0.1f);
  std::vector<float> over_right(513, 0.1f);
  float* over_channels[] = {over_left.data(), over_right.data()};
  CHECK(sonare_eq_process(eq, over_channels, 2, 513) == SONARE_ERROR_INVALID_PARAMETER);
  sonare_eq_destroy(eq);
}

TEST_CASE("sonare_streaming_loudness_gain refuses bad arguments", "[c_api][mastering][streaming]") {
  std::vector<float> samples(4800, 0.1f);
  SonareStreamingLoudnessGain out{};
  CHECK(sonare_streaming_loudness_gain(samples.data(), samples.size(), 48000, nullptr, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_streaming_loudness_gain(nullptr, samples.size(), 48000, nullptr, &out) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_streaming_loudness_gain(samples.data(), samples.size(), 48000, "{", &out) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_streaming_loudness_gain(samples.data(), samples.size(), 48000, "[]", &out) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_streaming_loudness_gain(samples.data(), samples.size(), 48000,
                                       R"({"no.such.key":1})",
                                       &out) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_streaming_loudness_gain_stereo(samples.data(), nullptr, samples.size(), 48000,
                                              nullptr, &out) == SONARE_ERROR_INVALID_PARAMETER);
}

#endif  // SONARE_WITH_MASTERING
