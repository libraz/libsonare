/// @file vqt_test.cpp
/// @brief Tests for Variable-Q Transform.

#include "feature/vqt.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

/// @brief Generates a pure sine wave.
Audio generate_sine(float freq, float duration, int sr = 22050) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);
  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / sr;
    samples[i] = std::sin(2.0f * M_PI * freq * t);
  }
  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("vqt_frequencies basic", "[vqt]") {
  float fmin = 32.7f;
  int n_bins = 24;
  int bins_per_octave = 12;

  auto freqs = vqt_frequencies(fmin, n_bins, bins_per_octave);

  REQUIRE(freqs.size() == 24);
  REQUIRE_THAT(freqs[0], WithinRel(fmin, 0.001f));
  // 2 octaves up
  REQUIRE_THAT(freqs[23], WithinRel(fmin * std::pow(2.0f, 23.0f / 12.0f), 0.01f));
}

TEST_CASE("vqt_bandwidths with gamma=0", "[vqt]") {
  std::vector<float> freqs = {100.0f, 200.0f, 400.0f};
  int bins_per_octave = 12;
  float gamma = 0.0f;

  auto bw = vqt_bandwidths(freqs, bins_per_octave, gamma);

  REQUIRE(bw.size() == 3);

  // With gamma=0, bandwidth is proportional to frequency
  float alpha = std::pow(2.0f, 1.0f / 12.0f) - 1.0f;
  REQUIRE_THAT(bw[0], WithinRel(alpha * 100.0f, 0.001f));
  REQUIRE_THAT(bw[1], WithinRel(alpha * 200.0f, 0.001f));
}

TEST_CASE("vqt_bandwidths with gamma>0", "[vqt]") {
  std::vector<float> freqs = {100.0f, 200.0f, 400.0f};
  int bins_per_octave = 12;
  float gamma = 50.0f;

  auto bw = vqt_bandwidths(freqs, bins_per_octave, gamma);

  float alpha = std::pow(2.0f, 1.0f / 12.0f) - 1.0f;

  // With gamma>0, bandwidth = alpha*f + gamma
  REQUIRE_THAT(bw[0], WithinRel(alpha * 100.0f + gamma, 0.001f));
  REQUIRE_THAT(bw[1], WithinRel(alpha * 200.0f + gamma, 0.001f));
}

TEST_CASE("VqtConfig to_cqt_config", "[vqt]") {
  VqtConfig vqt_config;
  vqt_config.hop_length = 256;
  vqt_config.fmin = 100.0f;
  vqt_config.n_bins = 36;
  vqt_config.bins_per_octave = 12;
  vqt_config.filter_scale = 2.0f;

  CqtConfig cqt_config = vqt_config.to_cqt_config();

  REQUIRE(cqt_config.hop_length == 256);
  REQUIRE(cqt_config.fmin == 100.0f);
  REQUIRE(cqt_config.n_bins == 36);
  REQUIRE(cqt_config.bins_per_octave == 12);
  REQUIRE(cqt_config.filter_scale == 2.0f);
}

TEST_CASE("VqtKernel creation", "[vqt]") {
  VqtConfig config;
  config.fmin = 65.4f;
  config.n_bins = 24;
  config.gamma = 20.0f;

  auto kernel = VqtKernel::create(22050, config);

  REQUIRE(kernel != nullptr);
  REQUIRE(kernel->n_bins() == 24);
  REQUIRE(kernel->fft_length() > 0);
  REQUIRE(kernel->frequencies().size() == 24);
  REQUIRE(kernel->bandwidths().size() == 24);

  // Bandwidths should be larger than pure CQT due to gamma
  const auto& bw = kernel->bandwidths();
  float alpha = std::pow(2.0f, 1.0f / config.bins_per_octave) - 1.0f;
  for (size_t k = 0; k < bw.size(); ++k) {
    float expected = alpha * kernel->frequencies()[k] + config.gamma;
    REQUIRE_THAT(bw[k], WithinRel(expected, 0.001f));
  }
}

TEST_CASE("vqt with gamma=0 equals cqt", "[vqt]") {
  Audio audio = generate_sine(440.0f, 0.5f, 22050);

  VqtConfig vqt_config;
  vqt_config.fmin = 65.4f;
  vqt_config.n_bins = 24;
  vqt_config.gamma = 0.0f;

  CqtConfig cqt_config = vqt_config.to_cqt_config();

  VqtResult vqt_result = vqt(audio, vqt_config);
  CqtResult cqt_result = cqt(audio, cqt_config);

  REQUIRE(vqt_result.n_bins() == cqt_result.n_bins());
  REQUIRE(vqt_result.n_frames() == cqt_result.n_frames());

  // Results should be identical
  const auto& vqt_mag = vqt_result.magnitude();
  const auto& cqt_mag = cqt_result.magnitude();

  for (size_t i = 0; i < vqt_mag.size(); ++i) {
    REQUIRE_THAT(vqt_mag[i], WithinAbs(cqt_mag[i], 1e-5f));
  }
}

TEST_CASE("vqt with gamma>0", "[vqt]") {
  Audio audio = generate_sine(440.0f, 0.5f, 22050);

  VqtConfig config;
  config.fmin = 65.4f;
  config.n_bins = 36;
  config.gamma = 24.0f;  // Typical value for VQT

  VqtResult result = vqt(audio, config);

  REQUIRE(!result.empty());
  REQUIRE(result.n_bins() == 36);
  REQUIRE(result.n_frames() > 0);
}

TEST_CASE("vqt single sine wave detection", "[vqt]") {
  float freq = 440.0f;
  Audio audio = generate_sine(freq, 1.0f, 22050);

  VqtConfig config;
  config.fmin = 65.4f;
  config.n_bins = 48;
  config.gamma = 20.0f;

  VqtResult result = vqt(audio, config);

  // Find the bin with max energy
  const auto& mag = result.magnitude();
  std::vector<float> bin_energy(result.n_bins(), 0.0f);
  for (int k = 0; k < result.n_bins(); ++k) {
    for (int t = 0; t < result.n_frames(); ++t) {
      bin_energy[k] += mag[k * result.n_frames() + t];
    }
  }

  int max_bin = 0;
  for (int k = 1; k < result.n_bins(); ++k) {
    if (bin_energy[k] > bin_energy[max_bin]) {
      max_bin = k;
    }
  }

  // Max bin frequency should be close to 440 Hz
  float detected_freq = result.frequencies()[max_bin];
  REQUIRE_THAT(detected_freq, WithinRel(freq, 0.1f));  // Within 10%
}

TEST_CASE("vqt with progress callback", "[vqt]") {
  Audio audio = generate_sine(440.0f, 1.0f, 22050);

  VqtConfig config;
  config.gamma = 20.0f;

  std::vector<float> progress_values;
  VqtResult result = vqt(audio, config, [&](float p) { progress_values.push_back(p); });

  REQUIRE(!progress_values.empty());
  REQUIRE(progress_values.back() >= 0.99f);
}

// Suppress deprecated warning for ivqt test (testing deprecated function)
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

TEST_CASE("ivqt reconstruction", "[vqt]") {
  Audio original = generate_sine(440.0f, 0.5f, 22050);

  VqtConfig config;
  config.n_bins = 36;
  config.gamma = 20.0f;

  VqtResult result = vqt(original, config);
  Audio reconstructed = ivqt(result, original.size());

  REQUIRE(reconstructed.sample_rate() == original.sample_rate());
  REQUIRE(!reconstructed.empty());
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

TEST_CASE("vqt empty audio throws", "[vqt]") {
  Audio empty_audio;
  REQUIRE_THROWS(vqt(empty_audio, VqtConfig()));
}

TEST_CASE("vqt different gamma values", "[vqt]") {
  Audio audio = generate_sine(440.0f, 0.5f, 22050);

  VqtConfig config;
  config.n_bins = 24;

  // Test with different gamma values
  std::vector<float> gammas = {0.0f, 10.0f, 24.0f, 50.0f};

  for (float gamma : gammas) {
    config.gamma = gamma;
    VqtResult result = vqt(audio, config);
    REQUIRE(!result.empty());
    REQUIRE(result.n_bins() == 24);
  }
}

// =============================================================================
// Librosa Compatibility Tests
// =============================================================================

TEST_CASE("vqt frequencies match librosa formula", "[vqt][librosa]") {
  // librosa.vqt_frequencies: f_k = fmin * 2^(k / bins_per_octave)
  float fmin = 32.7f;  // C1
  int n_bins = 84;     // 7 octaves
  int bins_per_octave = 12;

  auto freqs = vqt_frequencies(fmin, n_bins, bins_per_octave);

  REQUIRE(freqs.size() == 84);

  // Check each frequency matches librosa formula
  for (int k = 0; k < n_bins; ++k) {
    float expected = fmin * std::pow(2.0f, static_cast<float>(k) / bins_per_octave);
    REQUIRE_THAT(freqs[k], WithinRel(expected, 1e-5f));
  }

  // Check specific musical notes (C1 to C8)
  // C1 = 32.70 Hz, C2 = 65.41 Hz, ..., C8 = 4186.01 Hz
  REQUIRE_THAT(freqs[0], WithinRel(32.7f, 0.01f));    // C1
  REQUIRE_THAT(freqs[12], WithinRel(65.41f, 0.01f));  // C2
  REQUIRE_THAT(freqs[24], WithinRel(130.81f, 0.01f)); // C3
  REQUIRE_THAT(freqs[36], WithinRel(261.63f, 0.01f)); // C4 (middle C)
  REQUIRE_THAT(freqs[48], WithinRel(523.25f, 0.01f)); // C5
}

TEST_CASE("vqt bandwidth formula matches librosa", "[vqt][librosa]") {
  // librosa.variable_bandwidth: bw = alpha * f + gamma
  // where alpha = 2^(1/bins_per_octave) - 1
  int bins_per_octave = 12;
  float alpha = std::pow(2.0f, 1.0f / bins_per_octave) - 1.0f;

  // Standard CQT bandwidth (gamma=0)
  SECTION("gamma=0 (standard CQT)") {
    std::vector<float> freqs = {100.0f, 200.0f, 400.0f, 800.0f};
    auto bw = vqt_bandwidths(freqs, bins_per_octave, 0.0f);

    for (size_t i = 0; i < freqs.size(); ++i) {
      float expected = alpha * freqs[i];
      REQUIRE_THAT(bw[i], WithinRel(expected, 1e-5f));
    }
  }

  // VQT bandwidth (gamma=24, librosa default)
  SECTION("gamma=24 (librosa default)") {
    float gamma = 24.0f;
    std::vector<float> freqs = {100.0f, 200.0f, 400.0f, 800.0f};
    auto bw = vqt_bandwidths(freqs, bins_per_octave, gamma);

    for (size_t i = 0; i < freqs.size(); ++i) {
      float expected = alpha * freqs[i] + gamma;
      REQUIRE_THAT(bw[i], WithinRel(expected, 1e-5f));
    }

    // Lower frequencies should have proportionally more gamma influence
    // bw[0] = 5.95 + 24 = 29.95 (gamma is 80% of bandwidth)
    // bw[3] = 47.6 + 24 = 71.6 (gamma is 34% of bandwidth)
    float gamma_ratio_low = gamma / bw[0];
    float gamma_ratio_high = gamma / bw[3];
    REQUIRE(gamma_ratio_low > gamma_ratio_high);
  }
}

TEST_CASE("vqt output dimensions match librosa", "[vqt][librosa]") {
  // librosa.vqt returns shape (n_bins, n_frames)
  // n_frames depends on audio length and hop_length
  Audio audio = generate_sine(440.0f, 1.0f, 22050);

  VqtConfig config;
  config.hop_length = 512;
  config.fmin = 32.7f;
  config.n_bins = 84;
  config.bins_per_octave = 12;
  config.gamma = 24.0f;

  VqtResult result = vqt(audio, config);

  REQUIRE(result.n_bins() == 84);

  // VQT frame count depends on implementation details (centering, filter length)
  // Check that frame count is reasonable for the given audio length
  // With hop_length=512 and 22050 samples, expect roughly 40-45 frames
  REQUIRE(result.n_frames() >= 40);
  REQUIRE(result.n_frames() <= 50);

  // Verify frequencies are stored
  REQUIRE(result.frequencies().size() == 84);
}

TEST_CASE("vqt energy concentration for pure tone", "[vqt][librosa]") {
  // For a pure tone, VQT should concentrate energy in bins near the tone frequency
  // This behavior should match librosa
  float test_freq = 261.63f;  // C4
  Audio audio = generate_sine(test_freq, 1.0f, 22050);

  VqtConfig config;
  config.fmin = 32.7f;   // C1
  config.n_bins = 60;    // 5 octaves
  config.gamma = 24.0f;  // Standard VQT

  VqtResult result = vqt(audio, config);

  // Find bin closest to test frequency
  int target_bin = -1;
  float min_diff = std::numeric_limits<float>::max();
  for (int k = 0; k < result.n_bins(); ++k) {
    float diff = std::abs(result.frequencies()[k] - test_freq);
    if (diff < min_diff) {
      min_diff = diff;
      target_bin = k;
    }
  }

  REQUIRE(target_bin >= 0);

  // Calculate energy in each bin
  const auto& mag = result.magnitude();
  std::vector<float> bin_energy(result.n_bins(), 0.0f);
  for (int k = 0; k < result.n_bins(); ++k) {
    for (int t = 0; t < result.n_frames(); ++t) {
      bin_energy[k] += mag[k * result.n_frames() + t];
    }
  }

  // Target bin should have more energy than bins 2+ semitones away
  for (int k = 0; k < result.n_bins(); ++k) {
    if (std::abs(k - target_bin) >= 2) {
      REQUIRE(bin_energy[target_bin] > bin_energy[k]);
    }
  }
}
