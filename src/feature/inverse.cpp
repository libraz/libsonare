#include "feature/inverse.h"

#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <memory>

#include "core/audio.h"
#include "core/spectrum.h"
#include "filters/dct.h"
#include "filters/mel.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/nnls.h"
#include "util/numeric_validation.h"
#include "util/parameter_checks.h"

namespace sonare {

using sonare::constants::kPi;

namespace {

/// Smallest lift-window magnitude mfcc_to_mel divides by; below it the inverse amplifies a
/// coefficient by more than a thousand and no longer recovers the spectrum.
constexpr float kMinLift = 1.0e-3f;

}  // namespace

std::vector<float> mel_to_stft(const float* M, int n_mels, int n_frames,
                               const MelConfig& mel_config, int sr) {
  util::check::non_null(M, "M");
  if (n_mels <= 0 || n_frames <= 0) return {};
  util::check::integer_at_least(mel_config.n_fft, 1, "n_fft");
  util::check::integer_at_least(sr, 1, "sr");
  // A non-finite mel band poisons the NNLS active set, which then returns an
  // all-NaN spectrogram of the expected shape. Rejected here rather than at each
  // binding so the C ABI, the WASM bindings (which call this directly) and the
  // in-process callers all inherit one rule.
  if (!numeric::all_finite(M, n_mels, n_frames)) {
    validate_offline_audio_samples(nullptr, "M", M,
                                   static_cast<size_t>(n_mels) * static_cast<size_t>(n_frames), 0);
  }
  constexpr int kMaxInverseFrames = 4096;
  util::check::integer_in_range(n_frames, 1, kMaxInverseFrames, "n_frames");
  util::check::integer_in_range(mel_config.n_fft, 1, 8192, "n_fft");
  const int n_freq = mel_config.n_fft / 2 + 1;
  MelFilterConfig fcfg = mel_config.to_mel_filter_config();
  std::shared_ptr<const std::vector<float>> filterbank =
      get_mel_filterbank_cached(sr, mel_config.n_fft, fcfg);

  // librosa.feature.inverse.mel_to_stft solves `min ||M - W @ S||^2  s.t. S >= 0`
  // column-wise using scipy.optimize.nnls when available, where W is the mel
  // filterbank `[n_mels x n_freq]`. We mirror that with our own active-set
  // solver. A is W (n_mels x n_freq), B is M (n_mels x n_frames), X is S
  // (n_freq x n_frames).
  std::vector<float> stft_power = nnls(filterbank->data(), n_mels, n_freq, M, n_frames);

  // Return a magnitude spectrogram, matching librosa: mel_to_stft defaults to
  // `power=2.0` and undoes it via `np.power(S, 1 / power)`, i.e. a square root of
  // the non-negative NNLS result. Callers feeding this into Griffin-Lim /
  // plotting / comparison expect magnitude, not squared magnitude.
  for (float& v : stft_power) v = std::sqrt(std::max(0.0f, v));
  return stft_power;
}

Audio mel_to_audio(const float* M, int n_mels, int n_frames, const MelConfig& mel_config,
                   int n_iter, int sr) {
  util::check::integer_at_least(n_iter, 1, "n_iter");
  // mel_to_stft already returns a magnitude spectrogram (librosa-compatible), so
  // no additional square root is applied here before Griffin-Lim.
  std::vector<float> magnitude = mel_to_stft(M, n_mels, n_frames, mel_config, sr);
  const int n_freq = mel_config.n_fft / 2 + 1;

  GriffinLimConfig gcfg;
  gcfg.n_iter = n_iter;
  return griffin_lim(magnitude.data(), n_freq, n_frames, mel_config.n_fft, mel_config.hop_length,
                     sr, gcfg);
}

std::vector<float> mfcc_to_mel(const float* mfcc, int n_mfcc, int n_frames, int n_mels,
                               float lifter) {
  util::check::non_null(mfcc, "mfcc");
  util::check::integer_at_least(n_mfcc, 1, "n_mfcc");
  util::check::integer_at_least(n_frames, 1, "n_frames");
  util::check::integer_at_least(n_mels, 1, "n_mels");
  util::check::at_least(lifter, 0.0, "lifter");
  // A non-finite coefficient spreads across every Mel band through the inverse
  // DCT, and the dB-to-power conversion turns part of it back into 0 or Inf, so
  // the caller gets a plausible-looking spectrogram with no way to detect it.
  // Rejected here rather than at each binding so the C ABI, the WASM bindings
  // (which call this directly) and the in-process callers all inherit one rule.
  if (!numeric::all_finite(mfcc, n_mfcc, n_frames)) {
    validate_offline_audio_samples(nullptr, "mfcc", mfcc,
                                   static_cast<size_t>(n_mfcc) * static_cast<size_t>(n_frames), 0);
  }

  // Undo MFCC liftering before the inverse DCT. MelSpectrogram::mfcc multiplies
  // coefficient k (0-indexed) by `1 + (lifter/2) * sin(pi * (k + 1) / lifter)`;
  // dividing by the same lift window inverts it (matching
  // librosa.feature.inverse.mfcc_to_mel).
  std::vector<float> lift;
  if (lifter > 0.0f) {
    lift.resize(n_mfcc);
    for (int k = 0; k < n_mfcc; ++k) {
      lift[k] = 1.0f + (lifter / 2.0f) * std::sin(kPi * static_cast<float>(k + 1) / lifter);
      // A window that reaches zero cannot be divided back out of the coefficient it zeroed.
      if (std::abs(lift[k]) < kMinLift) {
        util::check::refuse_relation({"lifter", "n_mfcc"},
                                     "{0} must not zero the lift window below {1}");
      }
    }
  }

  // Inverse DCT of each frame: mel_db [n_mels x n_frames].
  std::vector<float> mel_db(static_cast<size_t>(n_mels) * n_frames, 0.0f);
  std::vector<float> col(n_mfcc);
  std::vector<float> rec(n_mels);
  for (int t = 0; t < n_frames; ++t) {
    for (int m = 0; m < n_mfcc; ++m) {
      col[m] = mfcc[m * n_frames + t];
      if (lifter > 0.0f) col[m] /= lift[m];
    }
    idct_ii_into(col.data(), n_mfcc, rec.data(), n_mels);
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
