/// @file acoustic_analyzer_test.cpp
/// @brief Tests for acoustic parameter analysis.

#include "analysis/acoustic_analyzer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "util/constants.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

Audio create_exponential_ir(float rt60_sec, int sample_rate = 48000, float duration_sec = 4.0f) {
  const int n_samples = static_cast<int>(sample_rate * duration_sec);
  std::vector<float> samples(n_samples);
  const float decay = std::log(1000.0f) / rt60_sec;
  for (int i = 0; i < n_samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    samples[i] = std::exp(-decay * t);
  }
  return Audio::from_vector(std::move(samples), sample_rate);
}

Audio create_noisy_exponential_decay(float rt60_sec, int sample_rate = 48000,
                                     float duration_sec = 4.0f, float noise_amplitude = 0.003f) {
  const int n_samples = static_cast<int>(sample_rate * duration_sec);
  std::vector<float> samples(n_samples);
  const float decay = std::log(1000.0f) / rt60_sec;
  uint32_t state = 0x1234567u;
  for (int i = 0; i < n_samples; ++i) {
    state = state * 1664525u + 1013904223u;
    const float noise =
        (static_cast<float>((state >> 8) & 0xffffu) / 32768.0f - 1.0f) * noise_amplitude;
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    samples[static_cast<size_t>(i)] = std::exp(-decay * t) + noise;
  }
  return Audio::from_vector(std::move(samples), sample_rate);
}

Audio create_decay_with_stationary_noise(float rt60_sec, int sample_rate = 48000,
                                         float duration_sec = 5.0f) {
  const int n_samples = static_cast<int>(sample_rate * duration_sec);
  std::vector<float> samples(n_samples);
  const float decay = std::log(1000.0f) / rt60_sec;
  uint32_t state = 0x89abcdefu;
  for (int i = 0; i < n_samples; ++i) {
    state = state * 1664525u + 1013904223u;
    const float white = static_cast<float>((state >> 8) & 0xffffu) / 32768.0f - 1.0f;
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    const float hum =
        0.020f * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 180.0f * t);
    samples[static_cast<size_t>(i)] = std::exp(-decay * t) + hum + white * 0.012f;
  }
  return Audio::from_vector(std::move(samples), sample_rate);
}

Audio create_repeated_free_decays(float rt60_sec, int sample_rate = 48000) {
  const float duration_sec = 5.0f;
  const int n_samples = static_cast<int>(sample_rate * duration_sec);
  std::vector<float> samples(static_cast<size_t>(n_samples), 0.0f);
  const float decay = std::log(1000.0f) / rt60_sec;
  const int starts[] = {0, static_cast<int>(2.2f * sample_rate)};
  for (int start : starts) {
    for (int i = 0; start + i < n_samples; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
      samples[static_cast<size_t>(start + i)] += std::exp(-decay * t);
    }
  }
  return Audio::from_vector(std::move(samples), sample_rate);
}

Audio create_upper_band_free_decay(float rt60_sec, int sample_rate = 48000) {
  const float duration_sec = 4.0f;
  const int n_samples = static_cast<int>(sample_rate * duration_sec);
  std::vector<float> samples(static_cast<size_t>(n_samples), 0.0f);
  const float decay = std::log(1000.0f) / rt60_sec;
  const float frequencies[] = {1000.0f, 2000.0f, 4000.0f};
  for (int i = 0; i < n_samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    const float envelope = std::exp(-decay * t);
    for (float frequency : frequencies) {
      samples[static_cast<size_t>(i)] +=
          envelope * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * frequency * t) /
          3.0f;
    }
  }
  return Audio::from_vector(std::move(samples), sample_rate);
}

float theoretical_clarity(float rt60_sec, float boundary_sec) {
  const float decay = std::log(1000.0f) / rt60_sec;
  return 10.0f * std::log10(std::exp(2.0f * decay * boundary_sec) - 1.0f);
}

float theoretical_d50(float rt60_sec) {
  const float decay = std::log(1000.0f) / rt60_sec;
  return 1.0f - std::exp(-2.0f * decay * 0.05f);
}

}  // namespace

TEST_CASE("AcousticAnalyzer estimates IR-based RT60 and EDT", "[acoustic_analyzer]") {
  const float expected_rt60 = 1.2f;
  const Audio ir = create_exponential_ir(expected_rt60);

  const auto params = AcousticAnalyzer::from_impulse_response(ir).parameters();

  REQUIRE_FALSE(params.is_blind);
  REQUIRE(std::isfinite(params.rt60));
  REQUIRE(std::isfinite(params.edt));
  REQUIRE_THAT(params.rt60, WithinRel(expected_rt60, 0.05f));
  REQUIRE_THAT(params.edt, WithinRel(expected_rt60, 0.05f));
  REQUIRE(params.confidence > 0.8f);
}

TEST_CASE("AcousticAnalyzer auto mode routes impulse-like input to IR analysis",
          "[acoustic_analyzer]") {
  const float expected_rt60 = 1.0f;
  const Audio ir = create_exponential_ir(expected_rt60);

  AcousticAnalyzer analyzer(ir);
  const auto& params = analyzer.parameters();

  REQUIRE_FALSE(params.is_blind);
  REQUIRE_THAT(params.rt60, WithinRel(expected_rt60, 0.05f));
  REQUIRE(std::isfinite(params.c50));
  REQUIRE(std::isfinite(params.c80));
  REQUIRE(std::isfinite(params.d50));
}

TEST_CASE("AcousticAnalyzer computes clarity metrics from impulse responses",
          "[acoustic_analyzer]") {
  const float expected_rt60 = 1.0f;
  const Audio ir = create_exponential_ir(expected_rt60);

  const auto params = analyze_impulse_response(ir);

  REQUIRE(std::isfinite(params.c50));
  REQUIRE(std::isfinite(params.c80));
  REQUIRE(std::isfinite(params.d50));
  REQUIRE_THAT(params.c50, WithinAbs(theoretical_clarity(expected_rt60, 0.05f), 0.2f));
  REQUIRE_THAT(params.c80, WithinAbs(theoretical_clarity(expected_rt60, 0.08f), 0.2f));
  REQUIRE_THAT(params.d50, WithinAbs(theoretical_d50(expected_rt60), 0.02f));
}

TEST_CASE("AcousticAnalyzer returns octave-band vectors for IR mode", "[acoustic_analyzer]") {
  AcousticConfig config;
  config.n_octave_bands = 6;
  const Audio ir = create_exponential_ir(0.8f);

  const auto params = AcousticAnalyzer::from_impulse_response(ir, config).parameters();

  REQUIRE(params.rt60_bands.size() == 6);
  REQUIRE(params.edt_bands.size() == 6);
  REQUIRE(params.c50_bands.size() == 6);
  REQUIRE(params.c80_bands.size() == 6);
  for (size_t i = 0; i < params.rt60_bands.size(); ++i) {
    INFO("band index: " << i);
    REQUIRE(std::isfinite(params.rt60_bands[i]));
    REQUIRE(std::isfinite(params.edt_bands[i]));
    REQUIRE(std::isfinite(params.c50_bands[i]));
    REQUIRE(std::isfinite(params.c80_bands[i]));
  }
}

TEST_CASE("AcousticAnalyzer estimates blind RT60 from synthetic free decay",
          "[.][slow][acoustic_analyzer]") {
  const float expected_rt60 = 0.7f;
  const Audio audio = create_exponential_ir(expected_rt60);

  AcousticConfig config;
  config.mode = AcousticConfig::Mode::Blind;
  config.n_third_octave_subbands = 24;
  AcousticAnalyzer analyzer(audio, config);
  const auto& params = analyzer.parameters();

  REQUIRE(params.is_blind);
  REQUIRE(std::isfinite(params.rt60));
  REQUIRE(std::isfinite(params.edt));
  REQUIRE_THAT(params.rt60, WithinRel(expected_rt60, 0.15f));
  REQUIRE(params.confidence >= 0.5f);
  REQUIRE(std::isnan(params.c50));
  REQUIRE(std::isnan(params.c80));
  REQUIRE(std::isnan(params.d50));
  REQUIRE(params.rt60_bands.size() == 6);

  int finite_bands = 0;
  for (float rt60 : params.rt60_bands) {
    if (std::isfinite(rt60)) {
      ++finite_bands;
    }
  }
  REQUIRE(finite_bands >= 3);
}

TEST_CASE("AcousticAnalyzer extrapolates blind low-frequency bands from high bands",
          "[acoustic_analyzer]") {
  const float expected_rt60 = 0.7f;
  const Audio audio = create_exponential_ir(expected_rt60, 8000, 4.0f);

  AcousticConfig config;
  config.mode = AcousticConfig::Mode::Blind;
  config.n_octave_bands = 4;
  config.n_third_octave_subbands = 16;
  AcousticAnalyzer analyzer(audio, config);
  const auto& params = analyzer.parameters();

  REQUIRE(params.rt60_bands.size() == 4);
  REQUIRE(std::isfinite(params.rt60_bands[0]));
  REQUIRE(std::isfinite(params.rt60_bands[1]));
  REQUIRE(std::isfinite(params.rt60_bands[2]));
  REQUIRE(std::isfinite(params.rt60_bands[3]));
  REQUIRE(params.rt60_bands[0] >= params.rt60 * 0.70f);
  REQUIRE(params.rt60_bands[0] <= params.rt60 * 1.50f);
}

TEST_CASE("AcousticAnalyzer models blind low bands from upper-band RT estimates",
          "[.][slow][acoustic_analyzer]") {
  const float expected_rt60 = 0.7f;
  const Audio audio = create_upper_band_free_decay(expected_rt60);

  AcousticConfig config;
  config.mode = AcousticConfig::Mode::Blind;
  config.n_octave_bands = 6;
  config.n_third_octave_subbands = 24;
  AcousticAnalyzer analyzer(audio, config);
  const auto& params = analyzer.parameters();

  REQUIRE(params.is_blind);
  REQUIRE(std::isfinite(params.rt60));
  REQUIRE_THAT(params.rt60, WithinRel(expected_rt60, 0.25f));
  REQUIRE(params.rt60_bands.size() == 6);
  REQUIRE(std::isfinite(params.rt60_bands[0]));
  REQUIRE(std::isfinite(params.rt60_bands[1]));
  REQUIRE(std::isfinite(params.rt60_bands[2]));
  REQUIRE(params.rt60_bands[0] >= params.rt60 * 0.75f);
  REQUIRE(params.rt60_bands[0] <= params.rt60 * 1.50f);
}

TEST_CASE("AcousticAnalyzer avoids noise-floor tail in blind free-decay fitting",
          "[acoustic_analyzer]") {
  const float expected_rt60 = 0.7f;
  const Audio audio = create_noisy_exponential_decay(expected_rt60);

  AcousticConfig config;
  config.mode = AcousticConfig::Mode::Blind;
  config.noise_floor_margin_db = 8.0f;
  AcousticAnalyzer analyzer(audio, config);
  const auto& params = analyzer.parameters();

  REQUIRE(params.is_blind);
  REQUIRE(std::isfinite(params.rt60));
  REQUIRE_THAT(params.rt60, WithinRel(expected_rt60, 0.25f));
  REQUIRE(params.confidence >= 0.5f);
}

TEST_CASE("AcousticAnalyzer fits blind decay above elevated noise floor", "[acoustic_analyzer]") {
  const float expected_rt60 = 0.7f;
  const Audio audio = create_noisy_exponential_decay(expected_rt60, 48000, 4.0f, 0.012f);

  AcousticConfig config;
  config.mode = AcousticConfig::Mode::Blind;
  config.noise_floor_margin_db = 12.0f;
  AcousticAnalyzer analyzer(audio, config);
  const auto& params = analyzer.parameters();

  REQUIRE(params.is_blind);
  REQUIRE(std::isfinite(params.rt60));
  REQUIRE_THAT(params.rt60, WithinRel(expected_rt60, 0.30f));
  REQUIRE(params.confidence >= 0.5f);
}

TEST_CASE("AcousticAnalyzer suppresses stationary noise before blind decay fitting",
          "[.][slow][acoustic_analyzer]") {
  const float expected_rt60 = 0.75f;
  const Audio audio = create_decay_with_stationary_noise(expected_rt60);

  AcousticConfig config;
  config.mode = AcousticConfig::Mode::Blind;
  config.noise_floor_margin_db = 10.0f;
  AcousticAnalyzer analyzer(audio, config);
  const auto& params = analyzer.parameters();

  REQUIRE(params.is_blind);
  REQUIRE(std::isfinite(params.rt60));
  REQUIRE_THAT(params.rt60, WithinRel(expected_rt60, 0.35f));
  REQUIRE(params.confidence >= 0.5f);
}

TEST_CASE("AcousticAnalyzer aggregates repeated blind free-decay regions",
          "[.][slow][acoustic_analyzer]") {
  const float expected_rt60 = 0.8f;
  const Audio audio = create_repeated_free_decays(expected_rt60);

  AcousticConfig config;
  config.mode = AcousticConfig::Mode::Blind;
  config.n_third_octave_subbands = 24;
  AcousticAnalyzer analyzer(audio, config);
  const auto& params = analyzer.parameters();

  REQUIRE(params.is_blind);
  REQUIRE(std::isfinite(params.rt60));
  REQUIRE_THAT(params.rt60, WithinRel(expected_rt60, 0.18f));
  REQUIRE(params.confidence >= 0.5f);
}

TEST_CASE("AcousticAnalyzer reports low-confidence blind RT60 as unavailable",
          "[acoustic_analyzer]") {
  const Audio audio = create_exponential_ir(0.7f);
  std::vector<float> samples(audio.size(), 0.1f);
  const Audio steady_audio = Audio::from_vector(std::move(samples), audio.sample_rate());

  AcousticConfig config;
  config.mode = AcousticConfig::Mode::Blind;
  AcousticAnalyzer analyzer(steady_audio, config);
  const auto& params = analyzer.parameters();

  REQUIRE(params.is_blind);
  REQUIRE(std::isnan(params.rt60));
  REQUIRE(std::isnan(params.edt));
  REQUIRE(params.confidence == 0.0f);
}

// Regression coverage for the percentile/nth_element optimization in
// suppress_stationary_noise_spectral: a pure tone embedded under white noise
// must still yield a finite RT60 estimate of the underlying free-decay
// envelope after spectral subtraction (the noise should not dominate).
TEST_CASE("AcousticAnalyzer preserves tone energy through spectral noise suppression",
          "[.][slow][acoustic_analyzer]") {
  const int sample_rate = 48000;
  const float duration_sec = 4.0f;
  const float expected_rt60 = 0.8f;
  const int n_samples = static_cast<int>(sample_rate * duration_sec);
  std::vector<float> samples(static_cast<size_t>(n_samples), 0.0f);
  const float decay = std::log(1000.0f) / expected_rt60;
  uint32_t state = 0x5eed5eedu;
  for (int i = 0; i < n_samples; ++i) {
    state = state * 1664525u + 1013904223u;
    const float white = static_cast<float>((state >> 8) & 0xffffu) / 32768.0f - 1.0f;
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    const float envelope = std::exp(-decay * t);
    const float tone =
        envelope * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 1000.0f * t);
    samples[static_cast<size_t>(i)] = tone + white * 0.008f;
  }
  const Audio audio = Audio::from_vector(std::move(samples), sample_rate);

  AcousticConfig config;
  config.mode = AcousticConfig::Mode::Blind;
  config.noise_floor_margin_db = 10.0f;
  AcousticAnalyzer analyzer(audio, config);
  const auto& params = analyzer.parameters();

  REQUIRE(params.is_blind);
  REQUIRE(std::isfinite(params.rt60));
  // Tolerance is intentionally loose: nth_element returns the same k-th
  // order statistic as std::sort but unrelated reorderings of equal-valued
  // bins can shift downstream estimates by a small but bounded amount.
  REQUIRE_THAT(params.rt60, WithinRel(expected_rt60, 0.35f));
  REQUIRE(params.confidence >= 0.4f);
}

// Regression coverage for the DC-removal fusion in filter_third_octave_band:
// a DC-offset signal must produce identical third-octave-band downstream
// estimates to its zero-mean counterpart (within tight tolerance).
TEST_CASE("AcousticAnalyzer is invariant to DC offset in blind third-octave fitting",
          "[.][slow][acoustic_analyzer]") {
  const float expected_rt60 = 0.7f;
  const Audio zero_mean = create_upper_band_free_decay(expected_rt60);

  std::vector<float> offset_samples(zero_mean.data(), zero_mean.data() + zero_mean.size());
  for (float& sample : offset_samples) {
    sample += 0.05f;  // arbitrary DC offset
  }
  const Audio with_offset = Audio::from_vector(std::move(offset_samples), zero_mean.sample_rate());

  AcousticConfig config;
  config.mode = AcousticConfig::Mode::Blind;
  config.n_octave_bands = 4;
  config.n_third_octave_subbands = 16;

  const auto params_zero = AcousticAnalyzer(zero_mean, config).parameters();
  const auto params_offset = AcousticAnalyzer(with_offset, config).parameters();

  REQUIRE(std::isfinite(params_zero.rt60));
  REQUIRE(std::isfinite(params_offset.rt60));
  // The bandpass filter plus explicit DC removal should make the analyzer
  // effectively invariant to a constant offset.
  REQUIRE_THAT(params_offset.rt60, WithinRel(params_zero.rt60, 0.05f));
}
