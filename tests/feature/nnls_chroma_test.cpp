/// @file nnls_chroma_test.cpp
/// @brief Tests for NNLS chroma extraction.

#include "feature/nnls_chroma.h"

#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "util/constants.h"

using namespace sonare;

namespace {

Audio sine(float freq, float duration, int sr = 22050) {
  std::vector<float> samples(static_cast<size_t>(duration * sr), 0.0f);
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * freq * t);
  }
  return Audio::from_vector(std::move(samples), sr);
}

/// A held triad of harmonic tones with 1/n partials, the way an instrument rather than a sine
/// sounds it: every overtone lands on some pitch class, most of them outside the chord.
Audio harmonic_triad(const std::vector<float>& midi_notes, float duration, int sr = 22050) {
  std::vector<float> samples(static_cast<size_t>(duration * sr), 0.0f);
  for (float note : midi_notes) {
    const float f0 = 440.0f * std::exp2((note - 69.0f) / 12.0f);
    for (int h = 1; h <= 8; ++h) {
      const float f = f0 * static_cast<float>(h);
      if (f >= 0.5f * static_cast<float>(sr)) break;
      for (size_t i = 0; i < samples.size(); ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(sr);
        samples[i] += 0.1f / static_cast<float>(h) *
                      std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * f * t);
      }
    }
  }
  return Audio::from_vector(std::move(samples), sr);
}

int argmax(const std::array<float, 12>& values) {
  return static_cast<int>(std::max_element(values.begin(), values.end()) - values.begin());
}

}  // namespace

TEST_CASE("NNLS harmonic template has expected shape and non-negative values", "[nnls_chroma]") {
  NnlsChromaConfig config;
  config.cqt.n_bins = 36;
  config.cqt.bins_per_octave = 12;
  const auto freqs =
      cqt_frequencies(config.cqt.fmin, config.cqt.n_bins, config.cqt.bins_per_octave);

  const auto matrix = build_nnls_harmonic_template(freqs, config);

  REQUIRE(matrix.size() == static_cast<size_t>(config.cqt.n_bins * config.n_pitches));
  REQUIRE(std::any_of(matrix.begin(), matrix.end(), [](float value) { return value > 0.0f; }));
  for (float value : matrix) {
    REQUIRE(value >= 0.0f);
  }
}

TEST_CASE("NNLS harmonic template uses planned harmonic weight ratios", "[nnls_chroma]") {
  NnlsChromaConfig config;
  config.midi_min = 69;
  config.n_pitches = 1;
  config.n_harmonics = 6;
  const std::vector<float> freqs = {440.0f, 880.0f, 1320.0f, 1760.0f, 2200.0f, 2640.0f};

  const auto matrix = build_nnls_harmonic_template(freqs, config);

  REQUIRE(matrix.size() == freqs.size());
  REQUIRE(matrix[0] > 0.0f);
  const float expected[] = {1.0f, 0.6f, 0.5f, 0.4f, 0.3f, 0.2f};
  for (size_t i = 1; i < freqs.size(); ++i) {
    const float ratio = matrix[i] / matrix[0];
    REQUIRE(std::abs(ratio - expected[i]) < 0.02f);
  }
}

TEST_CASE("nnls_chroma emphasizes pitch class of a sine tone", "[.][slow][nnls_chroma]") {
  NnlsChromaConfig config;
  config.cqt.n_bins = 48;
  config.cqt.hop_length = 512;

  Chroma chroma = nnls_chroma(sine(440.0f, 0.75f), config);
  REQUIRE(!chroma.empty());

  const auto mean = chroma.mean_energy();
  REQUIRE(argmax(mean) == static_cast<int>(PitchClass::A));
}

TEST_CASE("nnls_chroma keeps a held harmonic triad on its chord tones", "[nnls_chroma]") {
  // Without the STFT blend, so the salience the NNLS stage itself produces is what is read.
  NnlsChromaConfig config = NnlsChromaConfig::fast(512);
  config.enable_stft_blend = false;
  const Chroma chroma = nnls_chroma(harmonic_triad({48.0f, 52.0f, 55.0f}, 1.0f), config);
  REQUIRE(!chroma.empty());

  const auto mean = chroma.mean_energy();
  const float weakest_chord_tone = std::min({mean[0], mean[4], mean[7]});
  float strongest_other = 0.0f;
  for (int c = 0; c < 12; ++c) {
    if (c != 0 && c != 4 && c != 7) strongest_other = std::max(strongest_other, mean[c]);
  }
  CAPTURE(mean);
  CHECK(strongest_other < 0.6f * weakest_chord_tone);
}

TEST_CASE("sonare_nnls_chroma runs the shared fast profile", "[nnls_chroma][c_api]") {
  const Audio audio = sine(440.0f, 0.5f);
  NnlsChromaConfig config = NnlsChromaConfig::fast(512);
  REQUIRE(config.cqt.bins_per_octave == 12);
  REQUIRE(config.n_pitches == 60);
  config.enable_stft_blend = true;
  config.stft_blend_weight = 0.55f;
  config.stft_blend_n_fft = 4096;
  const Chroma expected = nnls_chroma(audio, config);

  float* data = nullptr;
  size_t length = 0;
  int n_frames = 0;
  REQUIRE(sonare_nnls_chroma(audio.data(), audio.size(), audio.sample_rate(), &data, &length,
                             &n_frames) == SONARE_OK);
  REQUIRE(n_frames == expected.n_frames());
  REQUIRE(length == static_cast<size_t>(expected.n_chroma()) * expected.n_frames());
  for (size_t i = 0; i < length; ++i) {
    REQUIRE(std::abs(data[i] - expected.data()[i]) < 1e-5f);
  }
  sonare_free_floats(data);
}
