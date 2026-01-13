/// @file time_stretch_test.cpp
/// @brief Tests for time stretching and phase vocoder.

#include "effects/time_stretch.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "effects/phase_vocoder.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

/// @brief Creates a test signal (sine wave).
Audio create_test_audio(float freq = 440.0f, int sr = 22050, float duration = 0.5f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = std::sin(2.0f * M_PI * freq * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("phase_vocoder basic", "[time_stretch]") {
  Audio audio = create_test_audio();

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  PhaseVocoderConfig pv_config;
  pv_config.hop_length = 256;

  // Rate 1.0 should produce similar number of frames
  Spectrogram stretched = phase_vocoder(spec, 1.0f, pv_config);

  REQUIRE(!stretched.empty());
  REQUIRE(stretched.n_bins() == spec.n_bins());
  REQUIRE_THAT(static_cast<float>(stretched.n_frames()),
               WithinRel(static_cast<float>(spec.n_frames()), 0.1f));
}

TEST_CASE("phase_vocoder slower", "[time_stretch]") {
  Audio audio = create_test_audio();

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  PhaseVocoderConfig pv_config;
  pv_config.hop_length = 256;

  // Rate 0.5 should double the number of frames
  Spectrogram stretched = phase_vocoder(spec, 0.5f, pv_config);

  REQUIRE(!stretched.empty());
  REQUIRE_THAT(static_cast<float>(stretched.n_frames()),
               WithinRel(static_cast<float>(spec.n_frames()) * 2.0f, 0.2f));
}

TEST_CASE("phase_vocoder faster", "[time_stretch]") {
  Audio audio = create_test_audio();

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  PhaseVocoderConfig pv_config;
  pv_config.hop_length = 256;

  // Rate 2.0 should halve the number of frames
  Spectrogram stretched = phase_vocoder(spec, 2.0f, pv_config);

  REQUIRE(!stretched.empty());
  REQUIRE_THAT(static_cast<float>(stretched.n_frames()),
               WithinRel(static_cast<float>(spec.n_frames()) * 0.5f, 0.2f));
}

TEST_CASE("time_stretch basic", "[time_stretch]") {
  Audio audio = create_test_audio(440.0f, 22050, 1.0f);

  TimeStretchConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;

  // Rate 1.0 should preserve duration
  Audio stretched = time_stretch(audio, 1.0f, config);

  REQUIRE(!stretched.empty());
  REQUIRE(stretched.sample_rate() == audio.sample_rate());
  REQUIRE_THAT(stretched.duration(), WithinRel(audio.duration(), 0.1f));
}

TEST_CASE("time_stretch slower doubles duration", "[time_stretch]") {
  Audio audio = create_test_audio(440.0f, 22050, 0.5f);

  TimeStretchConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;

  // Rate 0.5 should double duration
  Audio stretched = time_stretch(audio, 0.5f, config);

  REQUIRE(!stretched.empty());
  REQUIRE_THAT(stretched.duration(), WithinRel(audio.duration() * 2.0f, 0.2f));
}

TEST_CASE("time_stretch faster halves duration", "[time_stretch]") {
  Audio audio = create_test_audio(440.0f, 22050, 1.0f);

  TimeStretchConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;

  // Rate 2.0 should halve duration
  Audio stretched = time_stretch(audio, 2.0f, config);

  REQUIRE(!stretched.empty());
  REQUIRE_THAT(stretched.duration(), WithinRel(audio.duration() * 0.5f, 0.2f));
}

TEST_CASE("time_stretch preserves sample rate", "[time_stretch]") {
  Audio audio = create_test_audio(440.0f, 44100, 0.5f);

  Audio stretched = time_stretch(audio, 1.5f);

  REQUIRE(stretched.sample_rate() == audio.sample_rate());
}
