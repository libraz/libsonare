/// @file cqt_test.cpp
/// @brief Tests for Constant-Q Transform.

#include "feature/cqt.h"

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

/// @brief Generates a chord (multiple frequencies).
Audio generate_chord(const std::vector<float>& freqs, float duration, int sr = 22050) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);
  for (float freq : freqs) {
    for (int i = 0; i < n_samples; ++i) {
      float t = static_cast<float>(i) / sr;
      samples[i] += std::sin(2.0f * M_PI * freq * t) / freqs.size();
    }
  }
  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Finds bin index for given frequency.
int find_bin_for_freq(const std::vector<float>& freqs, float target) {
  int best_bin = 0;
  float min_diff = std::abs(freqs[0] - target);
  for (size_t i = 1; i < freqs.size(); ++i) {
    float diff = std::abs(freqs[i] - target);
    if (diff < min_diff) {
      min_diff = diff;
      best_bin = static_cast<int>(i);
    }
  }
  return best_bin;
}

}  // namespace

TEST_CASE("cqt_frequencies basic", "[cqt]") {
  float fmin = 32.7f;  // C1
  int n_bins = 12;
  int bins_per_octave = 12;

  auto freqs = cqt_frequencies(fmin, n_bins, bins_per_octave);

  REQUIRE(freqs.size() == 12);
  REQUIRE_THAT(freqs[0], WithinRel(fmin, 0.001f));

  // One octave up should be 2x
  REQUIRE_THAT(freqs[12 - 1], WithinRel(fmin * std::pow(2.0f, 11.0f / 12.0f), 0.01f));
}

TEST_CASE("cqt_frequencies multiple octaves", "[cqt]") {
  float fmin = 65.4f;  // C2
  int n_bins = 84;     // 7 octaves
  int bins_per_octave = 12;

  auto freqs = cqt_frequencies(fmin, n_bins, bins_per_octave);

  REQUIRE(freqs.size() == 84);

  // Check octave relationship
  // fmin * 2^7 = 7 octaves up
  float fmax_expected = fmin * std::pow(2.0f, 83.0f / 12.0f);
  REQUIRE_THAT(freqs[83], WithinRel(fmax_expected, 0.01f));
}

TEST_CASE("CqtKernel creation", "[cqt]") {
  CqtConfig config;
  config.fmin = 65.4f;
  config.n_bins = 24;  // 2 octaves
  config.bins_per_octave = 12;

  auto kernel = CqtKernel::create(22050, config);

  REQUIRE(kernel != nullptr);
  REQUIRE(kernel->n_bins() == 24);
  REQUIRE(kernel->fft_length() > 0);
  REQUIRE(kernel->frequencies().size() == 24);
}

TEST_CASE("cqt single sine wave", "[cqt]") {
  float freq = 440.0f;  // A4
  Audio audio = generate_sine(freq, 1.0f, 22050);

  CqtConfig config;
  config.fmin = 65.4f;  // C2
  config.n_bins = 48;   // 4 octaves
  config.hop_length = 512;

  CqtResult result = cqt(audio, config);

  REQUIRE(!result.empty());
  REQUIRE(result.n_bins() == 48);
  REQUIRE(result.n_frames() > 0);

  // Find the bin corresponding to 440 Hz
  int expected_bin = find_bin_for_freq(result.frequencies(), freq);

  // Get magnitude
  const auto& mag = result.magnitude();

  // Sum energy in each bin over all frames
  std::vector<float> bin_energy(result.n_bins(), 0.0f);
  for (int k = 0; k < result.n_bins(); ++k) {
    for (int t = 0; t < result.n_frames(); ++t) {
      bin_energy[k] += mag[k * result.n_frames() + t];
    }
  }

  // Expected bin should have significant energy
  REQUIRE(bin_energy[expected_bin] > 0.0f);

  // Find max energy bin
  int max_bin = 0;
  for (int k = 1; k < result.n_bins(); ++k) {
    if (bin_energy[k] > bin_energy[max_bin]) {
      max_bin = k;
    }
  }

  // Max energy bin should be close to expected bin (within 2 bins)
  REQUIRE(std::abs(max_bin - expected_bin) <= 2);
}

TEST_CASE("cqt chord detection", "[cqt]") {
  // C major chord: C4 (261.63), E4 (329.63), G4 (392.00)
  std::vector<float> chord_freqs = {261.63f, 329.63f, 392.0f};
  Audio audio = generate_chord(chord_freqs, 1.0f, 22050);

  CqtConfig config;
  config.fmin = 65.4f;
  config.n_bins = 48;
  config.hop_length = 512;

  CqtResult result = cqt(audio, config);

  const auto& mag = result.magnitude();

  // Sum energy per bin
  std::vector<float> bin_energy(result.n_bins(), 0.0f);
  for (int k = 0; k < result.n_bins(); ++k) {
    for (int t = 0; t < result.n_frames(); ++t) {
      bin_energy[k] += mag[k * result.n_frames() + t];
    }
  }

  // Find bins for each chord note
  for (float freq : chord_freqs) {
    int expected_bin = find_bin_for_freq(result.frequencies(), freq);
    // Check that this bin has non-zero energy
    REQUIRE(bin_energy[expected_bin] > 0.0f);
  }
}

TEST_CASE("CqtResult accessors", "[cqt]") {
  Audio audio = generate_sine(440.0f, 0.5f, 22050);

  CqtConfig config;
  config.fmin = 100.0f;
  config.n_bins = 24;
  config.hop_length = 256;

  CqtResult result = cqt(audio, config);

  REQUIRE(result.n_bins() == 24);
  REQUIRE(result.n_frames() > 0);
  REQUIRE(result.hop_length() == 256);
  REQUIRE(result.sample_rate() == 22050);
  REQUIRE(result.duration() > 0.0f);
  REQUIRE(!result.empty());
}

TEST_CASE("CqtResult magnitude and power", "[cqt]") {
  Audio audio = generate_sine(440.0f, 0.5f, 22050);

  CqtConfig config;
  config.n_bins = 24;

  CqtResult result = cqt(audio, config);

  const auto& mag = result.magnitude();
  const auto& pwr = result.power();

  REQUIRE(mag.size() == static_cast<size_t>(result.n_bins() * result.n_frames()));
  REQUIRE(pwr.size() == mag.size());

  // Power should be magnitude squared
  for (size_t i = 0; i < mag.size(); ++i) {
    REQUIRE_THAT(pwr[i], WithinRel(mag[i] * mag[i], 0.001f));
  }
}

TEST_CASE("CqtResult to_db", "[cqt]") {
  Audio audio = generate_sine(440.0f, 0.5f, 22050);

  CqtResult result = cqt(audio, CqtConfig());

  auto db = result.to_db();

  REQUIRE(db.size() == static_cast<size_t>(result.n_bins() * result.n_frames()));

  // dB values should be finite
  for (float val : db) {
    REQUIRE(std::isfinite(val));
  }
}

TEST_CASE("cqt_to_chroma", "[cqt]") {
  Audio audio = generate_sine(440.0f, 0.5f, 22050);

  CqtConfig config;
  config.n_bins = 36;  // 3 octaves

  CqtResult result = cqt(audio, config);

  auto chroma = cqt_to_chroma(result, 12);

  REQUIRE(chroma.size() == static_cast<size_t>(12 * result.n_frames()));

  // Chroma values should be in [0, 1]
  for (float val : chroma) {
    REQUIRE(val >= 0.0f);
    REQUIRE(val <= 1.0f + 1e-6f);
  }
}

TEST_CASE("cqt with progress callback", "[cqt]") {
  Audio audio = generate_sine(440.0f, 1.0f, 22050);

  std::vector<float> progress_values;
  CqtResult result = cqt(audio, CqtConfig(), [&](float p) { progress_values.push_back(p); });

  REQUIRE(!progress_values.empty());
  REQUIRE(progress_values.back() >= 0.99f);  // Should reach ~100%
}

// Suppress deprecated warning for icqt test (testing deprecated function)
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

TEST_CASE("icqt reconstruction", "[cqt]") {
  Audio original = generate_sine(440.0f, 0.5f, 22050);

  CqtConfig config;
  config.n_bins = 36;
  config.hop_length = 256;

  CqtResult result = cqt(original, config);
  Audio reconstructed = icqt(result, original.size());

  REQUIRE(reconstructed.sample_rate() == original.sample_rate());
  REQUIRE(!reconstructed.empty());

  // Note: Perfect reconstruction is not guaranteed with this simple iCQT
  // Just verify it doesn't crash and produces reasonable output
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

TEST_CASE("CQT phase correctness", "[cqt]") {
  // Generate a 440Hz sine wave
  float freq = 440.0f;
  int sr = 22050;
  float duration = 1.0f;
  Audio audio = generate_sine(freq, duration, sr);

  CqtConfig config;
  config.fmin = 65.4f;
  config.n_bins = 48;
  config.hop_length = 512;

  CqtResult result = cqt(audio, config);

  // Find the bin closest to 440Hz
  int target_bin = find_bin_for_freq(result.frequencies(), freq);

  // Verify that the phase progresses across frames.
  // For a pure tone, the phase difference between consecutive frames should be
  // approximately -2*pi*freq*hop_length/sr (modulo 2*pi).
  float expected_phase_diff = -2.0f * M_PI * freq * config.hop_length / sr;

  // Normalize expected_phase_diff to [-pi, pi]
  while (expected_phase_diff > M_PI) expected_phase_diff -= 2.0f * M_PI;
  while (expected_phase_diff < -M_PI) expected_phase_diff += 2.0f * M_PI;

  // Check phase progression in steady-state frames (skip edges where windowing effects dominate)
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
    while (phase_diff > M_PI) phase_diff -= 2.0f * M_PI;
    while (phase_diff < -M_PI) phase_diff += 2.0f * M_PI;

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

TEST_CASE("cqt empty audio throws", "[cqt]") {
  Audio empty_audio;
  REQUIRE_THROWS(cqt(empty_audio, CqtConfig()));
}

TEST_CASE("CqtResult at accessor", "[cqt]") {
  Audio audio = generate_sine(440.0f, 0.5f, 22050);
  CqtResult result = cqt(audio, CqtConfig());

  // Valid access
  REQUIRE_NOTHROW(result.at(0, 0));

  // Invalid access
  REQUIRE_THROWS(result.at(-1, 0));
  REQUIRE_THROWS(result.at(0, result.n_frames()));
  REQUIRE_THROWS(result.at(result.n_bins(), 0));
}
