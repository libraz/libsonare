/// @file nnls_chroma_test.cpp
/// @brief Tests for NNLS chroma extraction.

#include "feature/nnls_chroma.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

using namespace sonare;

namespace {

Audio sine(float freq, float duration, int sr = 22050) {
  std::vector<float> samples(static_cast<size_t>(duration * sr), 0.0f);
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = std::sin(2.0f * static_cast<float>(M_PI) * freq * t);
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

TEST_CASE("nnls_chroma emphasizes pitch class of a sine tone", "[nnls_chroma]") {
  NnlsChromaConfig config;
  config.cqt.n_bins = 48;
  config.cqt.hop_length = 512;
  config.whiten = false;

  Chroma chroma = nnls_chroma(sine(440.0f, 0.75f), config);
  REQUIRE(!chroma.empty());

  const auto mean = chroma.mean_energy();
  REQUIRE(argmax(mean) == static_cast<int>(PitchClass::A));
}
