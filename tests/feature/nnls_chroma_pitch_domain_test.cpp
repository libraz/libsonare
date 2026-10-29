#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "feature/nnls_chroma.h"
#include "util/constants.h"

namespace {

sonare::Audio sine(float frequency, float duration_s, int sample_rate = 22050) {
  std::vector<float> samples(static_cast<size_t>(duration_s * sample_rate));
  for (size_t i = 0; i < samples.size(); ++i) {
    const float time_s = static_cast<float>(i) / static_cast<float>(sample_rate);
    samples[i] = std::sin(sonare::constants::kTwoPi * frequency * time_s);
  }
  return sonare::Audio::from_vector(std::move(samples), sample_rate);
}

sonare::NnlsChromaConfig small_config() {
  sonare::NnlsChromaConfig config;
  config.cqt.fmin = 440.0f;
  config.cqt.n_bins = 12;
  config.cqt.bins_per_octave = 12;
  config.cqt.hop_length = 128;
  config.midi_min = -1;
  config.n_pitches = 1;
  config.n_harmonics = 1;
  config.whiten = false;
  config.max_iter = 2;
  config.enable_stft_blend = false;
  config.normalize_frames = false;
  return config;
}

int argmax(const std::array<float, 12>& values) {
  return static_cast<int>(std::max_element(values.begin(), values.end()) - values.begin());
}

}  // namespace

TEST_CASE("NNLS chroma wraps negative MIDI pitch classes safely", "[nnls_chroma][domain]") {
  const sonare::Audio audio = sine(493.8833f, 0.04f);  // B4
  const std::vector<float> original_samples(audio.begin(), audio.end());

  sonare::NnlsChromaConfig negative_config = small_config();
  const sonare::Chroma negative_chroma = sonare::nnls_chroma(audio, negative_config);
  REQUIRE(!negative_chroma.empty());
  REQUIRE(negative_chroma.n_chroma() == 12);
  const auto negative_mean = negative_chroma.mean_energy();
  CHECK(argmax(negative_mean) == 11);
  CHECK(negative_mean[11] > 0.0f);
  for (int frame = 0; frame < negative_chroma.n_frames(); ++frame) {
    for (int chroma = 0; chroma < 12; ++chroma) {
      const float value = negative_chroma.at(chroma, frame);
      CHECK(std::isfinite(value));
      CHECK(value >= 0.0f);
    }
  }
  CHECK(std::equal(audio.begin(), audio.end(), original_samples.begin()));

  // -1 and 71 are five octaves apart but name the same pitch class (B).
  sonare::NnlsChromaConfig positive_config = small_config();
  positive_config.midi_min = 71;
  const sonare::Chroma positive_chroma = sonare::nnls_chroma(audio, positive_config);
  REQUIRE(!positive_chroma.empty());
  CHECK(argmax(positive_chroma.mean_energy()) == 11);
}

TEST_CASE("NNLS harmonic templates widen MIDI pitch arithmetic", "[nnls_chroma][domain]") {
  sonare::NnlsChromaConfig config = small_config();
  config.midi_min = std::numeric_limits<int>::max();
  config.n_pitches = 2;
  const std::vector<float> frequencies = {440.0f, 880.0f};

  const auto matrix = sonare::build_nnls_harmonic_template(frequencies, config);
  REQUIRE(matrix.size() == frequencies.size() * static_cast<size_t>(config.n_pitches));
  CHECK(std::all_of(matrix.begin(), matrix.end(),
                    [](float value) { return std::isfinite(value) && value >= 0.0f; }));
}
