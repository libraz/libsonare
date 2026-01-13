/// @file chroma_test.cpp
/// @brief Tests for Chroma feature class.

#include "feature/chroma.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

/// @brief Creates a simple test signal with a specific frequency.
Audio create_sine_audio(float freq, int sr = 22050, float duration = 0.5f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = std::sin(2.0f * M_PI * freq * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates a chord with multiple frequencies.
Audio create_chord_audio(const std::vector<float>& freqs, int sr = 22050, float duration = 0.5f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);

  for (float freq : freqs) {
    for (int i = 0; i < n_samples; ++i) {
      float t = static_cast<float>(i) / static_cast<float>(sr);
      samples[i] += std::sin(2.0f * M_PI * freq * t);
    }
  }

  // Normalize
  float max_val = 0.0f;
  for (float s : samples) {
    max_val = std::max(max_val, std::abs(s));
  }
  if (max_val > 0.0f) {
    for (float& s : samples) {
      s /= max_val;
    }
  }

  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("Chroma compute basic", "[chroma]") {
  Audio audio = create_sine_audio(440.0f);

  ChromaConfig config;
  config.n_chroma = 12;
  config.n_fft = 2048;
  config.hop_length = 512;

  Chroma chroma = Chroma::compute(audio, config);

  REQUIRE(!chroma.empty());
  REQUIRE(chroma.n_chroma() == 12);
  REQUIRE(chroma.n_frames() > 0);
  REQUIRE(chroma.sample_rate() == 22050);
  REQUIRE(chroma.hop_length() == 512);
}

TEST_CASE("Chroma from_spectrogram", "[chroma]") {
  Audio audio = create_sine_audio(440.0f);

  StftConfig stft_config;
  stft_config.n_fft = 2048;
  stft_config.hop_length = 512;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  ChromaFilterConfig chroma_config;
  chroma_config.n_chroma = 12;

  Chroma chroma = Chroma::from_spectrogram(spec, audio.sample_rate(), chroma_config);

  REQUIRE(!chroma.empty());
  REQUIRE(chroma.n_chroma() == 12);
  REQUIRE(chroma.n_frames() == spec.n_frames());
}

TEST_CASE("Chroma A440 detection", "[chroma]") {
  // A4 = 440 Hz should have strong energy at pitch class A (index 9)
  Audio audio = create_sine_audio(440.0f, 22050, 1.0f);

  ChromaConfig config;
  config.n_chroma = 12;
  config.n_fft = 4096;  // Higher resolution
  config.hop_length = 512;

  Chroma chroma = Chroma::compute(audio, config);

  std::array<float, 12> mean_energy = chroma.mean_energy();

  // Find the pitch class with maximum energy
  int max_idx = 0;
  float max_val = mean_energy[0];
  for (int i = 1; i < 12; ++i) {
    if (mean_energy[i] > max_val) {
      max_val = mean_energy[i];
      max_idx = i;
    }
  }

  // A should be at index 9 (C=0, C#=1, ..., A=9)
  REQUIRE(max_idx == 9);
}

TEST_CASE("Chroma C major chord detection", "[chroma]") {
  // C major chord: C4 (261.63), E4 (329.63), G4 (392.00)
  std::vector<float> freqs = {261.63f, 329.63f, 392.00f};
  Audio audio = create_chord_audio(freqs, 22050, 1.0f);

  ChromaConfig config;
  config.n_chroma = 12;
  config.n_fft = 4096;
  config.hop_length = 512;

  Chroma chroma = Chroma::compute(audio, config);

  std::array<float, 12> mean_energy = chroma.mean_energy();

  // C=0, E=4, G=7 should be stronger than other notes
  // Normalize by max
  float max_val = *std::max_element(mean_energy.begin(), mean_energy.end());
  if (max_val > 0.0f) {
    for (auto& e : mean_energy) {
      e /= max_val;
    }
  }

  // Check that C, E, G have relatively high energy
  float threshold = 0.3f;
  REQUIRE(mean_energy[0] > threshold);  // C
  REQUIRE(mean_energy[4] > threshold);  // E
  REQUIRE(mean_energy[7] > threshold);  // G
}

TEST_CASE("Chroma features matrix view", "[chroma]") {
  Audio audio = create_sine_audio(440.0f);

  ChromaConfig config;
  config.n_chroma = 12;

  Chroma chroma = Chroma::compute(audio, config);

  MatrixView<float> features = chroma.features();
  REQUIRE(features.rows() == 12);
  REQUIRE(static_cast<int>(features.cols()) == chroma.n_frames());

  // Values should be non-negative
  for (size_t c = 0; c < features.rows(); ++c) {
    for (size_t t = 0; t < features.cols(); ++t) {
      REQUIRE(features.at(static_cast<int>(c), static_cast<int>(t)) >= 0.0f);
    }
  }
}

TEST_CASE("Chroma normalize L2", "[chroma]") {
  Audio audio = create_sine_audio(440.0f);

  ChromaConfig config;
  config.n_chroma = 12;

  Chroma chroma = Chroma::compute(audio, config);

  std::vector<float> normalized = chroma.normalize(2);
  REQUIRE(normalized.size() == static_cast<size_t>(12 * chroma.n_frames()));

  // Each frame should have unit L2 norm
  for (int t = 0; t < chroma.n_frames(); ++t) {
    float sum_sq = 0.0f;
    for (int c = 0; c < 12; ++c) {
      float val = normalized[c * chroma.n_frames() + t];
      sum_sq += val * val;
    }
    float norm = std::sqrt(sum_sq);
    // Either unit norm or zero
    REQUIRE((norm < 1e-6f || std::abs(norm - 1.0f) < 0.01f));
  }
}

TEST_CASE("Chroma normalize L1", "[chroma]") {
  Audio audio = create_sine_audio(440.0f);

  ChromaConfig config;
  config.n_chroma = 12;

  Chroma chroma = Chroma::compute(audio, config);

  std::vector<float> normalized = chroma.normalize(1);
  REQUIRE(normalized.size() == static_cast<size_t>(12 * chroma.n_frames()));

  // Each frame should have unit L1 norm
  for (int t = 0; t < chroma.n_frames(); ++t) {
    float sum_abs = 0.0f;
    for (int c = 0; c < 12; ++c) {
      sum_abs += std::abs(normalized[c * chroma.n_frames() + t]);
    }
    // Either unit norm or zero
    REQUIRE((sum_abs < 1e-6f || std::abs(sum_abs - 1.0f) < 0.01f));
  }
}

TEST_CASE("Chroma dominant_pitch_class", "[chroma]") {
  Audio audio = create_sine_audio(440.0f);

  ChromaConfig config;
  config.n_chroma = 12;
  config.n_fft = 4096;

  Chroma chroma = Chroma::compute(audio, config);

  std::vector<int> dominant = chroma.dominant_pitch_class();
  REQUIRE(dominant.size() == static_cast<size_t>(chroma.n_frames()));

  // All values should be in range [0, 11]
  for (int pc : dominant) {
    REQUIRE(pc >= 0);
    REQUIRE(pc < 12);
  }

  // Most frames should detect A (index 9) as dominant for 440 Hz
  int count_a = 0;
  for (int pc : dominant) {
    if (pc == 9) count_a++;
  }
  // At least half should be A
  REQUIRE(count_a > static_cast<int>(dominant.size()) / 2);
}

TEST_CASE("Chroma duration", "[chroma]") {
  int sr = 22050;
  float expected_duration = 0.5f;
  Audio audio = create_sine_audio(440.0f, sr, expected_duration);

  ChromaConfig config;
  config.n_fft = 2048;
  config.hop_length = 512;

  Chroma chroma = Chroma::compute(audio, config);

  REQUIRE_THAT(chroma.duration(), WithinRel(expected_duration, 0.1f));
}

TEST_CASE("Chroma at accessor", "[chroma]") {
  Audio audio = create_sine_audio(440.0f);

  ChromaConfig config;
  config.n_chroma = 12;

  Chroma chroma = Chroma::compute(audio, config);

  MatrixView<float> features = chroma.features();
  for (int c = 0; c < std::min(5, chroma.n_chroma()); ++c) {
    for (int t = 0; t < std::min(5, chroma.n_frames()); ++t) {
      REQUIRE_THAT(chroma.at(c, t), WithinAbs(features.at(c, t), 1e-10f));
    }
  }
}
