#include "feature/inverse.h"

#include <Eigen/Core>
#include <algorithm>
#include <cmath>

#include "core/spectrum.h"
#include "filters/dct.h"
#include "filters/mel.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/nnls.h"

namespace sonare {

using sonare::constants::kPi;

std::vector<float> mel_to_stft(const float* M, int n_mels, int n_frames,
                               const MelConfig& mel_config, int sr) {
  if (M == nullptr) throw SonareException(ErrorCode::InvalidParameter, "mel_to_stft: M is null");
  if (n_mels <= 0 || n_frames <= 0) return {};
  if (mel_config.n_fft <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "mel_to_stft: n_fft must be > 0");
  }
  if (sr <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "mel_to_stft: sr must be > 0");
  }
  const int n_freq = mel_config.n_fft / 2 + 1;
  MelFilterConfig fcfg = mel_config.to_mel_filter_config();
  const std::vector<float>& filterbank = get_mel_filterbank_cached(sr, mel_config.n_fft, fcfg);

  // librosa.feature.inverse.mel_to_stft solves `min ||M - W @ S||^2  s.t. S >= 0`
  // column-wise using scipy.optimize.nnls when available, where W is the mel
  // filterbank `[n_mels x n_freq]`. We mirror that with our own active-set
  // solver. A is W (n_mels x n_freq), B is M (n_mels x n_frames), X is S
  // (n_freq x n_frames).
  return nnls(filterbank.data(), n_mels, n_freq, M, n_frames);
}

Audio mel_to_audio(const float* M, int n_mels, int n_frames, const MelConfig& mel_config,
                   int n_iter, int sr) {
  std::vector<float> power = mel_to_stft(M, n_mels, n_frames, mel_config, sr);
  const int n_freq = mel_config.n_fft / 2 + 1;
  // Convert power -> magnitude before Griffin-Lim.
  for (float& v : power) v = std::sqrt(std::max(0.0f, v));

  GriffinLimConfig gcfg;
  gcfg.n_iter = n_iter;
  return griffin_lim(power.data(), n_freq, n_frames, mel_config.n_fft, mel_config.hop_length, sr,
                     gcfg);
}

std::vector<float> mfcc_to_mel(const float* mfcc, int n_mfcc, int n_frames, int n_mels,
                               float lifter) {
  if (mfcc == nullptr)
    throw SonareException(ErrorCode::InvalidParameter, "mfcc_to_mel: mfcc is null");
  if (n_mfcc <= 0 || n_frames <= 0 || n_mels <= 0) return {};
  if (lifter < 0.0f)
    throw SonareException(ErrorCode::InvalidParameter, "mfcc_to_mel: lifter must be >= 0");

  // Undo MFCC liftering before the inverse DCT. MelSpectrogram::mfcc multiplies
  // coefficient k (0-indexed) by `1 + (lifter/2) * sin(pi * (k + 1) / lifter)`;
  // dividing by the same lift window inverts it (matching
  // librosa.feature.inverse.mfcc_to_mel).
  std::vector<float> lift;
  if (lifter > 0.0f) {
    lift.resize(n_mfcc);
    for (int k = 0; k < n_mfcc; ++k) {
      lift[k] = 1.0f + (lifter / 2.0f) * std::sin(kPi * static_cast<float>(k + 1) / lifter);
    }
  }

  // Inverse DCT of each frame: mel_db [n_mels x n_frames].
  std::vector<float> mel_db(static_cast<size_t>(n_mels) * n_frames, 0.0f);
  std::vector<float> col(n_mfcc);
  for (int t = 0; t < n_frames; ++t) {
    for (int m = 0; m < n_mfcc; ++m) {
      col[m] = mfcc[m * n_frames + t];
      if (lifter > 0.0f) col[m] /= lift[m];
    }
    std::vector<float> rec = idct_ii(col.data(), n_mfcc, n_mels);
    for (int m = 0; m < n_mels; ++m) {
      mel_db[m * n_frames + t] = rec[m];
    }
  }
  // Convert dB -> power. librosa uses ref=1.0 and power=2 -> P = 10^(dB/10).
  std::vector<float> mel_power = mel_db;
  for (float& v : mel_power) v = db_to_power_scalar(v);
  return mel_power;
}

Audio mfcc_to_audio(const float* mfcc, int n_mfcc, int n_frames, const MelConfig& mel_config,
                    int n_iter, int sr, float lifter) {
  std::vector<float> mel = mfcc_to_mel(mfcc, n_mfcc, n_frames, mel_config.n_mels, lifter);
  return mel_to_audio(mel.data(), mel_config.n_mels, n_frames, mel_config, n_iter, sr);
}

}  // namespace sonare
