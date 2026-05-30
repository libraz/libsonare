/// @file vqt_test.cpp
/// @brief Tests for Variable-Q Transform.

#include "feature/vqt.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "util/constants.h"

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
    samples[i] = std::sin(2.0f * sonare::constants::kPiD * freq * t);
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
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
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
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

TEST_CASE("VQT phase correctness", "[vqt]") {
  // Generate a 440Hz sine wave
  float freq = 440.0f;
  int sr = 22050;
  float duration = 1.0f;
  Audio audio = generate_sine(freq, duration, sr);

  VqtConfig config;
  config.fmin = 65.4f;
  config.n_bins = 48;
  config.hop_length = 512;
  config.gamma = 20.0f;

  VqtResult result = vqt(audio, config);

  // Find the bin closest to 440Hz
  int target_bin = -1;
  float min_diff = std::numeric_limits<float>::max();
  for (int k = 0; k < result.n_bins(); ++k) {
    float diff = std::abs(result.frequencies()[k] - freq);
    if (diff < min_diff) {
      min_diff = diff;
      target_bin = k;
    }
  }
  REQUIRE(target_bin >= 0);

  // For a pure tone, the phase difference between consecutive frames should be
  // approximately -2*pi*freq*hop_length/sr (modulo 2*pi).
  float expected_phase_diff = -2.0f * sonare::constants::kPiD * freq * config.hop_length / sr;

  // Normalize expected_phase_diff to [-pi, pi]
  while (expected_phase_diff > sonare::constants::kPiD)
    expected_phase_diff -= 2.0f * sonare::constants::kPiD;
  while (expected_phase_diff < -sonare::constants::kPiD)
    expected_phase_diff += 2.0f * sonare::constants::kPiD;

  // Check phase progression in steady-state frames
  int start_frame = result.n_frames() / 4;
  int end_frame = 3 * result.n_frames() / 4;
  int match_count = 0;
  int total_count = 0;

  for (int t = start_frame; t < end_frame - 1; ++t) {
    std::complex<float> c0 = result.at(target_bin, t);
    std::complex<float> c1 = result.at(target_bin, t + 1);

    // Skip frames with very low magnitude (unreliable phase)
    if (std::abs(c0) < 1e-6f || std::abs(c1) < 1e-6f) continue;

    float phase0 = std::arg(c0);
    float phase1 = std::arg(c1);
    float phase_diff = phase1 - phase0;

    // Normalize to [-pi, pi]
    while (phase_diff > sonare::constants::kPiD) phase_diff -= 2.0f * sonare::constants::kPiD;
    while (phase_diff < -sonare::constants::kPiD) phase_diff += 2.0f * sonare::constants::kPiD;

    // Phase difference should match expected (with tolerance for windowing effects)
    if (std::abs(phase_diff - expected_phase_diff) < 0.5f) {
      ++match_count;
    }
    ++total_count;
  }

  // At least 80% of frames should have correct phase progression
  REQUIRE(total_count > 0);
  REQUIRE(static_cast<float>(match_count) / total_count >= 0.8f);
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

// Regression: changing only `window` must invalidate the VQT kernel cache.
// Previously the cache key omitted `window`, so two configurations that differ
// only in the window function reused the first-cached kernel and produced
// identical (incorrect) outputs. The VQT kernel actually consumes
// `config.window` via `create_window(...)`, so different windows must produce
// numerically different spectrograms.
TEST_CASE("VQT kernel cache distinguishes window type", "[vqt][regression][cache]") {
  Audio audio = generate_sine(440.0f, 0.5f, 22050);

  VqtConfig cfg_a;
  cfg_a.fmin = 65.4f;
  cfg_a.n_bins = 36;
  cfg_a.hop_length = 256;
  cfg_a.bins_per_octave = 12;
  cfg_a.gamma = 24.0f;  // gamma > 0 so vqt() does not delegate to cqt()
  cfg_a.window = WindowType::Hann;

  VqtConfig cfg_b = cfg_a;
  cfg_b.window = WindowType::Hamming;  // different window -> different kernel

  VqtResult res_a = vqt(audio, cfg_a);
  VqtResult res_b = vqt(audio, cfg_b);

  REQUIRE(res_a.n_bins() == res_b.n_bins());
  REQUIRE(res_a.n_frames() == res_b.n_frames());

  const auto& mag_a = res_a.magnitude();
  const auto& mag_b = res_b.magnitude();
  REQUIRE(mag_a.size() == mag_b.size());

  // Compare frame by frame and require at least one meaningful difference.
  // A cache key collision (the bug being regression-tested) would force every
  // value to be equal because the Hamming call would reuse the Hann kernel.
  double max_diff = 0.0;
  for (size_t i = 0; i < mag_a.size(); ++i) {
    max_diff = std::max(max_diff, static_cast<double>(std::abs(mag_a[i] - mag_b[i])));
  }
  CAPTURE(max_diff);
  REQUIRE(max_diff > 1e-4);
}

// =============================================================================
// Reference-formula tests
// =============================================================================

TEST_CASE("vqt frequencies match reference formula", "[vqt][reference]") {
  // f_k = fmin * 2^(k / bins_per_octave)
  float fmin = 32.7f;  // C1
  int n_bins = 84;     // 7 octaves
  int bins_per_octave = 12;

  auto freqs = vqt_frequencies(fmin, n_bins, bins_per_octave);

  REQUIRE(freqs.size() == 84);

  // Check each frequency matches the expected formula
  for (int k = 0; k < n_bins; ++k) {
    float expected = fmin * std::pow(2.0f, static_cast<float>(k) / bins_per_octave);
    REQUIRE_THAT(freqs[k], WithinRel(expected, 1e-5f));
  }

  // Check specific musical notes (C1 to C8)
  // C1 = 32.70 Hz, C2 = 65.41 Hz, ..., C8 = 4186.01 Hz
  REQUIRE_THAT(freqs[0], WithinRel(32.7f, 0.01f));     // C1
  REQUIRE_THAT(freqs[12], WithinRel(65.41f, 0.01f));   // C2
  REQUIRE_THAT(freqs[24], WithinRel(130.81f, 0.01f));  // C3
  REQUIRE_THAT(freqs[36], WithinRel(261.63f, 0.01f));  // C4 (middle C)
  REQUIRE_THAT(freqs[48], WithinRel(523.25f, 0.01f));  // C5
}

TEST_CASE("vqt bandwidth formula matches reference", "[vqt][reference]") {
  // variable-bandwidth formula: bw = alpha * f + gamma
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

  // VQT bandwidth (gamma=24)
  SECTION("gamma=24") {
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

TEST_CASE("vqt output dimensions match reference", "[vqt][reference]") {
  // Expected output shape: (n_bins, n_frames)
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

TEST_CASE("vqt center padding matches cqt frame count", "[vqt][reference]") {
  // CQT (gamma=0) and VQT (gamma=very small) should produce the same number of frames
  // because both apply center padding.
  Audio audio = generate_sine(440.0f, 1.0f, 22050);

  VqtConfig vqt_config;
  vqt_config.fmin = 65.4f;
  vqt_config.n_bins = 24;
  vqt_config.gamma = 0.001f;  // Very small gamma to keep VQT path but near-CQT behavior

  CqtConfig cqt_config = vqt_config.to_cqt_config();

  VqtResult vqt_result = vqt(audio, vqt_config);
  CqtResult cqt_result = cqt(audio, cqt_config);

  // Both should produce the same number of frames due to center padding
  REQUIRE(vqt_result.n_frames() == cqt_result.n_frames());
}

TEST_CASE("vqt energy concentration for pure tone", "[vqt][reference]") {
  // For a pure tone, VQT should concentrate energy in bins near the tone frequency
  // This behavior should follow the reference implementation
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

// =============================================================================
// Regression tests for librosa `scale=True` normalisation
// =============================================================================

TEST_CASE("vqt librosa scale=True normalisation direction", "[vqt][regression]") {
  // librosa.vqt(scale=True) (the default) divides the inner product by
  // sqrt(filter_length). Combined with the basis scaling baked into the
  // kernel, the per-bin amplitude at the matched bin grows like ~sqrt(L).
  //
  // Regression guard: a previous implementation multiplied by sqrt(L) at the
  // *output* stage while also missing the basis-side `L/n_fft` factor, which
  // produced magnitudes ~3 orders of magnitude smaller than librosa for the
  // matched bin (the "high-frequency bins over-attenuated" report).
  const int sr = 22050;
  const float freq = 440.0f;
  const float duration = 2.0f;
  Audio audio = generate_sine(freq, duration, sr);

  VqtConfig cfg;
  cfg.fmin = 65.4f;  // C2
  cfg.n_bins = 48;
  cfg.bins_per_octave = 12;
  cfg.gamma = 24.0f;

  VqtResult result = vqt(audio, cfg);

  // Locate the bin nearest 440 Hz.
  const auto& freqs = result.frequencies();
  int target_bin = 0;
  float best_diff = std::abs(freqs[0] - freq);
  for (int k = 1; k < result.n_bins(); ++k) {
    float d = std::abs(freqs[k] - freq);
    if (d < best_diff) {
      best_diff = d;
      target_bin = k;
    }
  }

  // Mid-frame magnitude at the matched bin (steady-state, away from edges).
  const auto& mag = result.magnitude();
  const int n_frames = result.n_frames();
  const int mid_frame = n_frames / 2;
  const float peak_mag = mag[target_bin * n_frames + mid_frame];

  // For a sine of amplitude 1.0, the matched-bin peak magnitude must be of
  // order unity (specifically ~0.5 * sqrt(length) with librosa scaling). The
  // bug produced ~0.01, so even a very loose lower bound catches it.
  REQUIRE(peak_mag > 1.0f);
  // And an upper bound to catch the other failure mode (forgetting the
  // `1/sqrt(L)` and over-amplifying).
  REQUIRE(peak_mag < 100.0f);
}

TEST_CASE("vqt amplitude continuous across gamma=0 boundary", "[vqt][regression]") {
  // gamma=0 delegates to cqt(); gamma>0 uses the dedicated VQT path. The two
  // kernel constructions differ but must agree on the librosa-compatible
  // amplitude convention, otherwise sweeping gamma toward 0 produces a step
  // discontinuity in the output magnitudes.
  const int sr = 22050;
  const float freq = 440.0f;
  Audio audio = generate_sine(freq, 2.0f, sr);

  VqtConfig cfg;
  cfg.fmin = 65.4f;
  cfg.n_bins = 48;
  cfg.bins_per_octave = 12;

  cfg.gamma = 0.0f;
  VqtResult res_cqt = vqt(audio, cfg);
  cfg.gamma = 0.01f;  // tiny but non-zero -> takes the VQT path
  VqtResult res_vqt = vqt(audio, cfg);

  REQUIRE(res_cqt.n_bins() == res_vqt.n_bins());
  REQUIRE(res_cqt.n_frames() == res_vqt.n_frames());

  // Find the peak bin in the CQT result.
  const auto& mag_cqt = res_cqt.magnitude();
  const auto& mag_vqt = res_vqt.magnitude();
  const int n_frames = res_cqt.n_frames();
  int peak_bin = 0;
  float peak_e = 0.0f;
  for (int k = 0; k < res_cqt.n_bins(); ++k) {
    float e = 0.0f;
    for (int t = 0; t < n_frames; ++t) e += mag_cqt[k * n_frames + t];
    if (e > peak_e) {
      peak_e = e;
      peak_bin = k;
    }
  }

  // Peak bin should match between paths.
  int peak_bin_vqt = 0;
  float peak_e_vqt = 0.0f;
  for (int k = 0; k < res_vqt.n_bins(); ++k) {
    float e = 0.0f;
    for (int t = 0; t < n_frames; ++t) e += mag_vqt[k * n_frames + t];
    if (e > peak_e_vqt) {
      peak_e_vqt = e;
      peak_bin_vqt = k;
    }
  }
  REQUIRE(peak_bin == peak_bin_vqt);

  // The mid-frame peak amplitude must agree within a small factor. The two
  // kernel constructions are not identical, but with matching scaling
  // conventions they should agree to better than 20%.
  const int mid = n_frames / 2;
  const float cqt_peak = mag_cqt[peak_bin * n_frames + mid];
  const float vqt_peak = mag_vqt[peak_bin * n_frames + mid];
  REQUIRE(cqt_peak > 0.0f);
  REQUIRE(vqt_peak > 0.0f);
  const float ratio = vqt_peak / cqt_peak;
  CAPTURE(cqt_peak, vqt_peak, ratio);
  REQUIRE(ratio > 0.5f);
  REQUIRE(ratio < 2.0f);
}

TEST_CASE("vqt scaling follows librosa sqrt(L) order of magnitude", "[vqt][regression]") {
  // librosa.vqt(scale=True) emits, for a matched pure tone, a peak magnitude
  // of roughly 0.5 * sqrt(filter_length). Confirm we are within that order of
  // magnitude (factor of 2x in either direction). The previous bug missed this
  // by ~1000x.
  const int sr = 22050;
  const float freq = 440.0f;
  Audio audio = generate_sine(freq, 2.0f, sr);

  VqtConfig cfg;
  cfg.fmin = 65.4f;
  cfg.n_bins = 48;
  cfg.bins_per_octave = 12;
  cfg.gamma = 24.0f;

  auto kernel = VqtKernel::create(sr, cfg);
  REQUIRE(kernel != nullptr);

  VqtResult result = vqt(audio, cfg);

  // Locate matched bin via energy sum.
  const auto& mag = result.magnitude();
  const int n_frames = result.n_frames();
  std::vector<float> bin_energy(result.n_bins(), 0.0f);
  for (int k = 0; k < result.n_bins(); ++k) {
    for (int t = 0; t < n_frames; ++t) bin_energy[k] += mag[k * n_frames + t];
  }
  int peak_bin = 0;
  for (int k = 1; k < result.n_bins(); ++k) {
    if (bin_energy[k] > bin_energy[peak_bin]) peak_bin = k;
  }

  // Expected order of magnitude: 0.5 * sqrt(length).
  const int L = kernel->lengths()[peak_bin];
  REQUIRE(L > 0);
  const float expected = 0.5f * std::sqrt(static_cast<float>(L));

  // Mid-frame mag at peak bin (steady-state).
  const float measured = mag[peak_bin * n_frames + n_frames / 2];
  CAPTURE(L, expected, measured);
  REQUIRE(measured > 0.5f * expected);
  REQUIRE(measured < 2.0f * expected);
}

TEST_CASE("ivqt respects requested output length", "[vqt][inverse]") {
  Audio audio = generate_sine(440.0f, 0.25f, 22050);

  VqtConfig config;
  config.fmin = 65.4f;
  config.n_bins = 24;
  config.gamma = 24.0f;

  VqtResult result = vqt(audio, config);
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
  Audio reconstructed = ivqt(result, 4096);
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

  REQUIRE(reconstructed.size() == 4096);
  REQUIRE(reconstructed.sample_rate() == audio.sample_rate());
}
