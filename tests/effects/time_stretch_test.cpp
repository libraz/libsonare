/// @file time_stretch_test.cpp
/// @brief Tests for time stretching backends and phase vocoder.

#include "effects/time_stretch.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "effects/phase_vocoder.h"
#include "util/constants.h"

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
    samples[i] = std::sin(2.0f * sonare::constants::kPiD * freq * t);
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

TEST_CASE("phase_vocoder preserves spectrogram center flag", "[time_stretch]") {
  Audio audio = create_test_audio();

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;
  stft_config.center = false;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);
  Spectrogram stretched = phase_vocoder(spec, 1.0f);

  REQUIRE_FALSE(stretched.center());
}

TEST_CASE("phase_vocoder preserves spectrogram win_length", "[time_stretch]") {
  Audio audio = create_test_audio();

  StftConfig stft_config;
  stft_config.n_fft = 2048;
  stft_config.win_length = 1024;
  stft_config.hop_length = 512;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);
  Spectrogram stretched = phase_vocoder(spec, 1.0f);

  REQUIRE(stretched.win_length() == stft_config.win_length);
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

TEST_CASE("time_stretch native spectral backend preserves sample rate and ratio",
          "[time_stretch]") {
  Audio audio = create_test_audio(440.0f, 44100, 0.25f);

  TimeStretchConfig config;
  config.backend = StretchBackend::NativeSpectral;

  Audio stretched = time_stretch(audio, 0.75f, config);

  REQUIRE(!stretched.empty());
  REQUIRE(stretched.sample_rate() == audio.sample_rate());
  REQUIRE_THAT(stretched.duration(), WithinRel(audio.duration() / 0.75f, 0.05f));
}

TEST_CASE("time_stretch native backend honors non-default n_fft", "[time_stretch]") {
  // Regression: n_fft/hop_length were silently ignored on the NativeSpectral
  // path. A non-default analysis size must now actually change the output.
  Audio audio = create_test_audio(440.0f, 22050, 0.5f);

  TimeStretchConfig default_cfg;
  default_cfg.backend = StretchBackend::NativeSpectral;  // n_fft=2048, hop=512

  TimeStretchConfig custom_cfg;
  custom_cfg.backend = StretchBackend::NativeSpectral;
  custom_cfg.n_fft = 1024;
  custom_cfg.hop_length = 256;

  Audio default_out = time_stretch(audio, 0.8f, default_cfg);
  Audio custom_out = time_stretch(audio, 0.8f, custom_cfg);

  REQUIRE(!default_out.empty());
  REQUIRE(!custom_out.empty());

  // The two analysis settings must produce materially different output; if
  // n_fft were ignored the buffers would be identical.
  const size_t n = std::min(default_out.size(), custom_out.size());
  REQUIRE(n > 0);
  double diff = 0.0;
  for (size_t i = 0; i < n; ++i) {
    diff += std::abs(static_cast<double>(default_out.data()[i]) -
                     static_cast<double>(custom_out.data()[i]));
  }
  REQUIRE(diff > 1e-3);
}

TEST_CASE("time_stretch phase vocoder backend remains available", "[time_stretch]") {
  Audio audio = create_test_audio(440.0f, 22050, 0.5f);

  TimeStretchConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;
  config.backend = StretchBackend::PhaseVocoder;

  Audio stretched = time_stretch(audio, 1.25f, config);

  REQUIRE(!stretched.empty());
  REQUIRE(stretched.sample_rate() == audio.sample_rate());
  REQUIRE_THAT(stretched.duration(), WithinRel(audio.duration() / 1.25f, 0.2f));
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

TEST_CASE("phase_vocoder handles edge cases", "[time_stretch]") {
  Audio audio = create_test_audio(440.0f, 22050, 0.5f);

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  PhaseVocoderConfig pv_config;
  pv_config.hop_length = 256;

  SECTION("very slow rate") {
    // Rate 0.1 = 10x slower (should not hang or produce NaN)
    Spectrogram stretched = phase_vocoder(spec, 0.1f, pv_config);
    REQUIRE(!stretched.empty());
    REQUIRE(stretched.n_frames() > spec.n_frames());

    // Check no NaN in output
    const auto& mag = stretched.magnitude();
    for (size_t i = 0; i < mag.size(); ++i) {
      REQUIRE(std::isfinite(mag[i]));
    }
  }

  SECTION("very fast rate") {
    // Rate 10.0 = 10x faster
    Spectrogram stretched = phase_vocoder(spec, 10.0f, pv_config);
    REQUIRE(!stretched.empty());
    REQUIRE(stretched.n_frames() >= 1);

    const auto& mag = stretched.magnitude();
    for (size_t i = 0; i < mag.size(); ++i) {
      REQUIRE(std::isfinite(mag[i]));
    }
  }
}

TEST_CASE("phase_vocoder validation", "[time_stretch]") {
  SECTION("throws on empty spectrogram") {
    Spectrogram empty_spec;
    REQUIRE_THROWS(phase_vocoder(empty_spec, 1.0f));
  }

  SECTION("throws on single frame spectrogram") {
    // Create spectrogram with only 1 frame (< 2 required)
    Audio short_audio = create_test_audio(440.0f, 22050, 0.01f);  // 10ms

    StftConfig stft_config;
    stft_config.n_fft = 4096;  // Large FFT
    stft_config.hop_length = 512;
    stft_config.center = false;  // No center padding to get minimal frames

    Spectrogram spec = Spectrogram::compute(short_audio, stft_config);

    // Skip test if we got >= 2 frames (depends on audio length/FFT size)
    if (spec.n_frames() < 2) {
      REQUIRE_THROWS(phase_vocoder(spec, 1.0f));
    }
  }

  SECTION("throws on invalid rate") {
    Audio audio = create_test_audio(440.0f, 22050, 0.5f);
    StftConfig stft_config;
    Spectrogram spec = Spectrogram::compute(audio, stft_config);

    REQUIRE_THROWS(phase_vocoder(spec, 0.0f));
    REQUIRE_THROWS(phase_vocoder(spec, -1.0f));
  }
}
