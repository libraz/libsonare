/// @file inverse_features_test.cpp
/// @brief Smoke tests for mel_to_stft / mel_to_audio / mfcc_to_mel / mfcc_to_audio.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "core/audio.h"
#include "feature/inverse.h"
#include "feature/mel_spectrogram.h"
#include "util/constants.h"

using namespace sonare;
using namespace sonare::constants;

namespace {

Audio make_tone(float freq, int sr, float duration) {
  const size_t n = static_cast<size_t>(duration * sr);
  std::vector<float> y(n);
  const double tp = static_cast<double>(constants::kTwoPi);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(sr);
    y[i] = static_cast<float>(std::sin(tp * static_cast<double>(freq) * t));
  }
  return Audio::from_vector(std::move(y), sr);
}

}  // namespace

TEST_CASE("mel_to_stft returns non-negative [n_freq x n_frames]",
          "[inverse_features][unit][smoke]") {
  Audio audio = make_tone(440.0f, 22050, 0.25f);
  MelConfig mcfg;
  mcfg.n_fft = 1024;
  mcfg.hop_length = 256;
  mcfg.n_mels = 64;
  MelSpectrogram mel = MelSpectrogram::compute(audio, mcfg);
  REQUIRE(mel.n_frames() > 0);

  auto S = mel_to_stft(mel.power_data(), mel.n_mels(), mel.n_frames(), mcfg);
  const int n_freq = mcfg.n_fft / 2 + 1;
  REQUIRE(S.size() == static_cast<size_t>(n_freq * mel.n_frames()));
  for (float v : S) {
    REQUIRE(v >= 0.0f);
  }
}

TEST_CASE("mel_to_audio returns a non-empty Audio", "[inverse_features][unit][smoke]") {
  Audio audio = make_tone(440.0f, 22050, 0.25f);
  MelConfig mcfg;
  mcfg.n_fft = 1024;
  mcfg.hop_length = 256;
  mcfg.n_mels = 64;
  MelSpectrogram mel = MelSpectrogram::compute(audio, mcfg);

  Audio out = mel_to_audio(mel.power_data(), mel.n_mels(), mel.n_frames(), mcfg,
                           /*n_iter=*/4);
  REQUIRE(out.size() > 0);
  REQUIRE(out.sample_rate() > 0);
}

TEST_CASE("mfcc_to_mel returns expected shape", "[inverse_features][unit][smoke]") {
  const int n_mfcc = 13;
  const int n_frames = 10;
  const int n_mels = 64;
  std::vector<float> mfcc(static_cast<size_t>(n_mfcc * n_frames), 0.0f);
  // Put a non-trivial DC-like value in the first MFCC coefficient.
  for (int t = 0; t < n_frames; ++t) mfcc[0 * n_frames + t] = -10.0f;

  auto mel = mfcc_to_mel(mfcc.data(), n_mfcc, n_frames, n_mels);
  REQUIRE(mel.size() == static_cast<size_t>(n_mels * n_frames));
  for (float v : mel) {
    REQUIRE(v >= 0.0f);
  }
}

TEST_CASE("mfcc_to_audio returns a non-empty Audio", "[inverse_features][unit][smoke]") {
  const int n_mfcc = 13;
  const int n_frames = 8;
  std::vector<float> mfcc(static_cast<size_t>(n_mfcc * n_frames), 0.0f);
  for (int t = 0; t < n_frames; ++t) mfcc[0 * n_frames + t] = -5.0f;

  MelConfig mcfg;
  mcfg.n_fft = 1024;
  mcfg.hop_length = 256;
  mcfg.n_mels = 64;
  Audio out = mfcc_to_audio(mfcc.data(), n_mfcc, n_frames, mcfg, /*n_iter=*/2);
  REQUIRE(out.size() > 0);
}
