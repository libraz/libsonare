/// @file inverse_features_test.cpp
/// @brief Smoke tests for mel_to_stft / mel_to_audio / mfcc_to_mel / mfcc_to_audio.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "core/audio.h"
#include "feature/inverse.h"
#include "feature/mel_spectrogram.h"
#include "filters/mel.h"
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

TEST_CASE("mel_to_stft returns a magnitude spectrogram (librosa power=2.0)",
          "[inverse_features][unit]") {
  // librosa.feature.inverse.mel_to_stft defaults to power=2.0 and returns a
  // MAGNITUDE spectrogram: it square-roots the non-negative least-squares power
  // result. So the mel filterbank applied to the SQUARED output must recover the
  // input mel power far better than the filterbank applied to the raw output.
  Audio audio = make_tone(440.0f, 22050, 0.25f);
  MelConfig mcfg;
  mcfg.n_fft = 1024;
  mcfg.hop_length = 256;
  mcfg.n_mels = 64;
  MelSpectrogram mel = MelSpectrogram::compute(audio, mcfg);
  const int n_mels = mel.n_mels();
  const int n_frames = mel.n_frames();
  const int n_freq = mcfg.n_fft / 2 + 1;
  REQUIRE(n_frames > 0);

  std::vector<float> S = mel_to_stft(mel.power_data(), n_mels, n_frames, mcfg);
  REQUIRE(S.size() == static_cast<size_t>(n_freq) * n_frames);

  // Mel filterbank W [n_mels x n_freq] that produced the mel power.
  std::vector<float> W = create_mel_filterbank(22050, mcfg.n_fft, mcfg.to_mel_filter_config());
  REQUIRE(W.size() == static_cast<size_t>(n_mels) * n_freq);

  const float* M = mel.power_data();
  double res_power = 0.0;            // || W @ (S.^2) - M ||^2 : true NNLS residual
  double res_if_returned_pow = 0.0;  // || W @ S - M ||^2 : residual if S were power
  double norm_m = 0.0;
  for (int m = 0; m < n_mels; ++m) {
    for (int t = 0; t < n_frames; ++t) {
      double proj_sq = 0.0;   // sum_f W[m,f] * S[f,t]^2
      double proj_lin = 0.0;  // sum_f W[m,f] * S[f,t]
      for (int f = 0; f < n_freq; ++f) {
        const double w = W[static_cast<size_t>(m) * n_freq + f];
        const double s = S[static_cast<size_t>(f) * n_frames + t];
        proj_sq += w * s * s;
        proj_lin += w * s;
      }
      const double target = M[static_cast<size_t>(m) * n_frames + t];
      res_power += (proj_sq - target) * (proj_sq - target);
      res_if_returned_pow += (proj_lin - target) * (proj_lin - target);
      norm_m += target * target;
    }
  }
  // Squaring the output reconstructs the mel power well (the actual NNLS fit).
  REQUIRE(res_power < 0.05 * norm_m);
  // Treating the output as if it were already power fits far worse, confirming
  // the returned spectrogram is magnitude, not squared magnitude.
  REQUIRE(res_if_returned_pow > 10.0 * res_power);
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
