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
