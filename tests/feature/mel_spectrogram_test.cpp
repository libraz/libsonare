/// @file mel_spectrogram_test.cpp
/// @brief Tests for MelSpectrogram class.

#include "feature/mel_spectrogram.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "feature/inverse.h"
#include "util/constants.h"

using namespace sonare;
using namespace sonare::constants;
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
    samples[i] = std::sin(2.0f * sonare::constants::kPiD * freq * t);
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

  const int n_mfcc = 13;
  const float lifter = 22.0f;
  std::vector<float> mfcc_no_lift = mel.mfcc(n_mfcc, 0.0f);
  std::vector<float> mfcc_lift = mel.mfcc(n_mfcc, lifter);

  // Liftering should change values
  bool different = false;
  for (size_t i = 0; i < mfcc_no_lift.size(); ++i) {
    if (std::abs(mfcc_no_lift[i] - mfcc_lift[i]) > 1e-6f) {
      different = true;
      break;
    }
  }
  REQUIRE(different);

  // Verify the lifter formula matches librosa: 1 + (L/2) * sin(pi * (k+1) / L)
  // The DC coefficient (k=0) must receive a nonzero lift (regression test for
  // an off-by-one bug where the lift factor was computed with k instead of k+1).
  const int n_frames = mel.n_frames();
  for (int k = 0; k < n_mfcc; ++k) {
    const float expected_lift =
        1.0f +
        (lifter / 2.0f) * std::sin(sonare::constants::kPi * static_cast<float>(k + 1) / lifter);
    for (int t = 0; t < n_frames; ++t) {
      const float base = mfcc_no_lift[k * n_frames + t];
      const float expected = base * expected_lift;
      // Use absolute tolerance scaled by magnitude so zero values don't break WithinRel.
      const float tol = std::max(1e-4f, std::abs(expected) * 1e-5f);
      REQUIRE_THAT(mfcc_lift[k * n_frames + t], WithinAbs(expected, tol));
    }
  }
}

TEST_CASE("mfcc_to_mel inverts MFCC liftering when the lifter is passed back",
          "[mel_spectrogram][inverse]") {
  Audio audio = create_test_audio();

  MelConfig config;
  config.n_mels = 40;
  config.n_fft = 1024;
  config.hop_length = 256;

  MelSpectrogram mel = MelSpectrogram::compute(audio, config);

  const int n_mfcc = 13;
  const float lifter = 22.0f;
  const int n_frames = mel.n_frames();

  std::vector<float> mfcc_no_lift = mel.mfcc(n_mfcc, 0.0f);
  std::vector<float> mfcc_lift = mel.mfcc(n_mfcc, lifter);

  // Dividing the lift window back out must recover the unliftered inversion
  // exactly, so the liftered MFCC round-trips to the same mel power.
  std::vector<float> mel_from_lift =
      mfcc_to_mel(mfcc_lift.data(), n_mfcc, n_frames, config.n_mels, lifter);
  std::vector<float> mel_from_no_lift =
      mfcc_to_mel(mfcc_no_lift.data(), n_mfcc, n_frames, config.n_mels, 0.0f);

  REQUIRE(mel_from_lift.size() == mel_from_no_lift.size());
  for (size_t i = 0; i < mel_from_lift.size(); ++i) {
    const float tol = std::max(1e-4f, std::abs(mel_from_no_lift[i]) * 1e-4f);
    REQUIRE_THAT(mel_from_lift[i], WithinAbs(mel_from_no_lift[i], tol));
  }

  // If the lifter is NOT passed back, the liftered MFCC inverts incorrectly,
  // diverging from the unliftered reference. This confirms the lifter argument
  // is actually doing something.
  std::vector<float> mel_lift_unrecovered =
      mfcc_to_mel(mfcc_lift.data(), n_mfcc, n_frames, config.n_mels, 0.0f);
  bool diverges = false;
  for (size_t i = 0; i < mel_lift_unrecovered.size(); ++i) {
    if (std::abs(mel_lift_unrecovered[i] - mel_from_no_lift[i]) >
        std::max(1e-3f, std::abs(mel_from_no_lift[i]) * 1e-2f)) {
      diverges = true;
      break;
    }
  }
  REQUIRE(diverges);
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

TEST_CASE("MelSpectrogram delta matches Savitzky-Golay mode=interp on edges", "[mel_spectrogram]") {
  // For a perfectly linear feature y[t] = a + b*t, the first-order Savitzky-Golay
  // derivative (librosa's delta with mode='interp') is exactly the slope b at
  // EVERY frame, including the first/last width/2 frames. Edge-clamped regression
  // would under-estimate the boundary frames; mode='interp' fits the boundary
  // window and recovers the true slope.
  const int n_frames = 20;
  const int width = 9;
  const float a = 3.0f;
  const float b = 0.7f;
  std::vector<float> feature(n_frames);
  for (int t = 0; t < n_frames; ++t) {
    feature[t] = a + b * static_cast<float>(t);
  }

  std::vector<float> d = MelSpectrogram::delta(feature.data(), 1, n_frames, width);
  REQUIRE(d.size() == feature.size());
  for (int t = 0; t < n_frames; ++t) {
    CAPTURE(t, d[t]);
    REQUIRE_THAT(d[t], WithinAbs(b, 1e-4f));
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
