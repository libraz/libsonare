/// @file mel_spectrogram_test.cpp
/// @brief Tests for MelSpectrogram class.

#include "feature/mel_spectrogram.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

/// @brief Creates a simple test signal (440 Hz sine wave).
Audio create_test_audio(int sr = 22050, float duration = 0.5f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  float freq = 440.0f;
  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = std::sin(2.0f * M_PI * freq * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("MelSpectrogram compute basic", "[mel_spectrogram]") {
  Audio audio = create_test_audio();

  MelConfig config;
  config.n_mels = 40;
  config.n_fft = 1024;
  config.hop_length = 256;

  MelSpectrogram mel = MelSpectrogram::compute(audio, config);

  REQUIRE(!mel.empty());
  REQUIRE(mel.n_mels() == 40);
  REQUIRE(mel.n_frames() > 0);
  REQUIRE(mel.sample_rate() == 22050);
  REQUIRE(mel.hop_length() == 256);
}

TEST_CASE("MelSpectrogram from_spectrogram", "[mel_spectrogram]") {
  Audio audio = create_test_audio();

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  MelFilterConfig mel_config;
  mel_config.n_mels = 64;

  MelSpectrogram mel = MelSpectrogram::from_spectrogram(spec, audio.sample_rate(), mel_config);

  REQUIRE(!mel.empty());
  REQUIRE(mel.n_mels() == 64);
  REQUIRE(mel.n_frames() == spec.n_frames());
}

TEST_CASE("MelSpectrogram power matrix view", "[mel_spectrogram]") {
  Audio audio = create_test_audio();

  MelConfig config;
  config.n_mels = 40;
  config.n_fft = 1024;
  config.hop_length = 256;

  MelSpectrogram mel = MelSpectrogram::compute(audio, config);

  MatrixView<float> power = mel.power();
  REQUIRE(power.rows() == 40);
  REQUIRE(static_cast<int>(power.cols()) == mel.n_frames());

  // Check that values are non-negative
  for (size_t m = 0; m < power.rows(); ++m) {
    for (size_t t = 0; t < power.cols(); ++t) {
      REQUIRE(power.at(static_cast<int>(m), static_cast<int>(t)) >= 0.0f);
    }
  }
}

TEST_CASE("MelSpectrogram to_db", "[mel_spectrogram]") {
  Audio audio = create_test_audio();

  MelConfig config;
  config.n_mels = 40;
  config.n_fft = 1024;
  config.hop_length = 256;

  MelSpectrogram mel = MelSpectrogram::compute(audio, config);

  std::vector<float> db = mel.to_db();

  REQUIRE(db.size() == static_cast<size_t>(mel.n_mels() * mel.n_frames()));

  // dB values should be finite
  for (float val : db) {
    REQUIRE(std::isfinite(val));
  }
}

TEST_CASE("MelSpectrogram mfcc basic", "[mel_spectrogram]") {
  Audio audio = create_test_audio();

  MelConfig config;
  config.n_mels = 40;
  config.n_fft = 1024;
  config.hop_length = 256;

  MelSpectrogram mel = MelSpectrogram::compute(audio, config);

  int n_mfcc = 13;
  std::vector<float> mfcc = mel.mfcc(n_mfcc);

  REQUIRE(mfcc.size() == static_cast<size_t>(n_mfcc * mel.n_frames()));

  // MFCC values should be finite
  for (float val : mfcc) {
    REQUIRE(std::isfinite(val));
  }
}

TEST_CASE("MelSpectrogram mfcc with liftering", "[mel_spectrogram]") {
  Audio audio = create_test_audio();

  MelConfig config;
  config.n_mels = 40;
  config.n_fft = 1024;
  config.hop_length = 256;

  MelSpectrogram mel = MelSpectrogram::compute(audio, config);

  int n_mfcc = 13;
  std::vector<float> mfcc_no_lift = mel.mfcc(n_mfcc, 0.0f);
  std::vector<float> mfcc_lift = mel.mfcc(n_mfcc, 22.0f);

  // Liftering should change values
  bool different = false;
  for (size_t i = 0; i < mfcc_no_lift.size(); ++i) {
    if (std::abs(mfcc_no_lift[i] - mfcc_lift[i]) > 1e-6f) {
      different = true;
      break;
    }
  }
  REQUIRE(different);
}

TEST_CASE("MelSpectrogram delta", "[mel_spectrogram]") {
  Audio audio = create_test_audio();

  MelConfig config;
  config.n_mels = 40;
  config.n_fft = 1024;
  config.hop_length = 256;

  MelSpectrogram mel = MelSpectrogram::compute(audio, config);

  std::vector<float> mfcc = mel.mfcc(13);
  std::vector<float> delta = MelSpectrogram::delta(mfcc.data(), 13, mel.n_frames(), 9);

  REQUIRE(delta.size() == mfcc.size());

  // Delta values should be finite
  for (float val : delta) {
    REQUIRE(std::isfinite(val));
  }
}

TEST_CASE("MelSpectrogram duration", "[mel_spectrogram]") {
  int sr = 22050;
  float expected_duration = 0.5f;
  Audio audio = create_test_audio(sr, expected_duration);

  MelConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;

  MelSpectrogram mel = MelSpectrogram::compute(audio, config);

  // Duration should be approximately equal to audio duration
  REQUIRE_THAT(mel.duration(), WithinRel(expected_duration, 0.1f));
}

TEST_CASE("MelSpectrogram at accessor", "[mel_spectrogram]") {
  Audio audio = create_test_audio();

  MelConfig config;
  config.n_mels = 40;

  MelSpectrogram mel = MelSpectrogram::compute(audio, config);

  // at() should match power view
  MatrixView<float> power = mel.power();
  for (int m = 0; m < std::min(5, mel.n_mels()); ++m) {
    for (int t = 0; t < std::min(5, mel.n_frames()); ++t) {
      REQUIRE_THAT(mel.at(m, t), WithinAbs(power.at(m, t), 1e-10f));
    }
  }
}
