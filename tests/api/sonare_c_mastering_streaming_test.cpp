/// @file sonare_c_mastering_streaming_test.cpp
/// @brief Streaming mastering parameter-control C API tests.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "sonare_c_test_helpers.h"

#ifdef SONARE_WITH_MASTERING

TEST_CASE("streaming mastering C API sets prepared realtime parameters",
          "[c_api][mastering][streaming]") {
  const SonareMasteringParam params[] = {
      {"maximizer.truePeakLimiter.enabled", 1.0},
  };
  SonareStreamingMasteringChain* chain = sonare_streaming_mastering_chain_create(params, 1);
  REQUIRE(chain != nullptr);

  REQUIRE(sonare_streaming_mastering_chain_set_parameter(
              chain, "maximizer.truePeakLimiter.ceilingDb", -12.0) == SONARE_ERROR_INVALID_STATE);
  REQUIRE(sonare_streaming_mastering_chain_prepare(chain, 48000, 128, 1) == SONARE_OK);
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(
              chain, "maximizer.truePeakLimiter.ceilingDb", -12.0) == SONARE_OK);

  std::vector<float> block(128, 0.8f);
  REQUIRE(sonare_streaming_mastering_chain_process_mono(chain, block.data(), block.size()) ==
          SONARE_OK);

  REQUIRE(sonare_streaming_mastering_chain_set_parameter(chain,
                                                         "maximizer.truePeakLimiter.lookaheadMs",
                                                         4.0) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(
              chain, "maximizer.truePeakLimiter.ceilingDb",
              std::numeric_limits<double>::quiet_NaN()) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(chain, "missing.parameter", -1.0) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_streaming_mastering_chain_destroy(chain);
}

TEST_CASE("streaming mastering C API validates setter handles and keys",
          "[c_api][mastering][streaming]") {
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(nullptr,
                                                         "maximizer.truePeakLimiter.ceilingDb",
                                                         -12.0) == SONARE_ERROR_INVALID_PARAMETER);

  SonareStreamingMasteringChain* chain = sonare_streaming_mastering_chain_create(nullptr, 0);
  REQUIRE(chain != nullptr);
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(chain, nullptr, -12.0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(chain, "", -12.0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(
              chain, "maximizer.truePeakLimiter.ceilingDb",
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
  const SonareMasteringParam params[] = {{"loudness.targetLufs", -16.0}, {"eq.tilt.tiltDb", 2.0}};

  SonareStreamingLoudnessGain mono{};
  REQUIRE(sonare_streaming_loudness_gain(left.data(), left.size(), kRate, params, 2, &mono) ==
          SONARE_OK);
  SonareMasteringChainResult mono_chain{};
  REQUIRE(sonare_mastering_chain(left.data(), left.size(), kRate, params, 2, &mono_chain) ==
          SONARE_OK);
  CHECK(mono.loudness_static_gain_db == mono_chain.applied_gain_db);
  CHECK(std::isfinite(mono.integrated_lufs));
  CHECK(std::isfinite(mono.true_peak_db));
  sonare_free_mastering_chain_result(&mono_chain);

  SonareStreamingLoudnessGain stereo{};
  REQUIRE(sonare_streaming_loudness_gain_stereo(left.data(), right.data(), left.size(), kRate,
                                                params, 2, &stereo) == SONARE_OK);
  SonareMasteringChainStereoResult stereo_chain{};
  REQUIRE(sonare_mastering_chain_stereo(left.data(), right.data(), left.size(), kRate, params, 2,
                                        &stereo_chain) == SONARE_OK);
  CHECK(stereo.loudness_static_gain_db == stereo_chain.applied_gain_db);
  sonare_free_mastering_chain_stereo_result(&stereo_chain);

  std::vector<float> silence(kRate, 0.0f);
  SonareStreamingLoudnessGain quiet{1.0f, 1.0f, 1.0f};
  REQUIRE(sonare_streaming_loudness_gain(silence.data(), silence.size(), kRate, params, 2,
                                         &quiet) == SONARE_OK);
  CHECK(quiet.loudness_static_gain_db == 0.0f);
  CHECK_FALSE(std::isfinite(quiet.integrated_lufs));
}

TEST_CASE("sonare_streaming_loudness_gain refuses bad arguments", "[c_api][mastering][streaming]") {
  std::vector<float> samples(4800, 0.1f);
  SonareStreamingLoudnessGain out{};
  CHECK(sonare_streaming_loudness_gain(samples.data(), samples.size(), 48000, nullptr, 0,
                                       nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_streaming_loudness_gain(nullptr, samples.size(), 48000, nullptr, 0, &out) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_streaming_loudness_gain(samples.data(), samples.size(), 48000, nullptr, 1, &out) ==
        SONARE_ERROR_INVALID_PARAMETER);
  const SonareMasteringParam unknown[] = {{"no.such.key", 1.0}};
  CHECK(sonare_streaming_loudness_gain(samples.data(), samples.size(), 48000, unknown, 1, &out) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_streaming_loudness_gain_stereo(samples.data(), nullptr, samples.size(), 48000,
                                              nullptr, 0, &out) == SONARE_ERROR_INVALID_PARAMETER);
}

#endif  // SONARE_WITH_MASTERING
