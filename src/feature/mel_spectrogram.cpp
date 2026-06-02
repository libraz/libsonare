#include "feature/mel_spectrogram.h"

#include <Eigen/Core>
#include <algorithm>
#include <cmath>

#include "filters/dct.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare {

using sonare::constants::kEpsilon;
using sonare::constants::kPi;

MelSpectrogram::MelSpectrogram() : n_mels_(0), n_frames_(0), sample_rate_(0), hop_length_(0) {}

MelSpectrogram::MelSpectrogram(std::vector<float> power, int n_mels, int n_frames, int sample_rate,
                               int hop_length)
    : power_(std::move(power)),
      n_mels_(n_mels),
      n_frames_(n_frames),
      sample_rate_(sample_rate),
      hop_length_(hop_length) {}

MelSpectrogram MelSpectrogram::compute(const Audio& audio, const MelConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  // Compute STFT
  Spectrogram spec = Spectrogram::compute(audio, config.to_stft_config());

  return from_spectrogram(spec, audio.sample_rate(), config.to_mel_filter_config());
}

MelSpectrogram MelSpectrogram::from_spectrogram(const Spectrogram& spec, int sr,
                                                const MelFilterConfig& mel_config) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(sr > 0, ErrorCode::InvalidParameter);

  int n_bins = spec.n_bins();
  int n_frames = spec.n_frames();
  int n_mels = mel_config.n_mels;

  // Create Mel filterbank (cached — same (sr, n_fft, config) hits the same entry).
  MelFilterConfig config = mel_config;
  if (config.fmax <= 0.0f) {
    config.fmax = static_cast<float>(sr) / 2.0f;
  }
  const std::vector<float>& filterbank = get_mel_filterbank_cached(sr, spec.n_fft(), config);

  // Apply filterbank to power spectrum
  const std::vector<float>& power = spec.power();

  // Convert from row-major [n_bins x n_frames] to column-major for matrix multiply
  std::vector<float> mel_power =
      apply_mel_filterbank(power.data(), n_bins, n_frames, filterbank.data(), n_mels);

  return MelSpectrogram(std::move(mel_power), n_mels, n_frames, sr, spec.hop_length());
}

float MelSpectrogram::duration() const {
  if (sample_rate_ <= 0 || hop_length_ <= 0) {
    return 0.0f;
  }
  return static_cast<float>(n_frames_ * hop_length_) / static_cast<float>(sample_rate_);
}

MatrixView<float> MelSpectrogram::power() const {
  return MatrixView<float>(power_.data(), n_mels_, n_frames_);
}

std::vector<float> MelSpectrogram::to_db(float ref, float amin, float top_db) const {
  std::vector<float> db(power_.size());
  power_to_db(power_.data(), power_.size(), ref, amin, top_db, db.data());
  return db;
}

std::vector<float> MelSpectrogram::mfcc(int n_mfcc, float lifter) const {
  SONARE_CHECK(n_mfcc > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(n_mfcc <= n_mels_, ErrorCode::InvalidParameter);

  // Compute log Mel spectrogram in dB with the standard 10 * log10 scaling and
  // top_db clamp. This is exactly the shared power_to_db (ref=1.0, amin=kEpsilon,
  // top_db=kDefaultTopDb), so route through it instead of reimplementing it.
  std::vector<float> log_mel(power_.size());
  power_to_db(power_.data(), power_.size(), /*ref=*/1.0f, /*amin=*/kEpsilon,
              /*top_db=*/constants::kDefaultTopDb, log_mel.data());

  // Apply DCT-II to each frame using Eigen matrix multiplication
  std::vector<float> mfcc_out(n_mfcc * n_frames_);

  // Get cached DCT matrix [n_mfcc x n_mels]
  const std::vector<float>& dct_matrix = get_dct_matrix_cached(n_mfcc, n_mels_);

  // Use Eigen for optimized matrix multiplication
  // dct_matrix: [n_mfcc x n_mels] (row-major)
  // log_mel: [n_mels x n_frames] (row-major)
  // result: [n_mfcc x n_frames] (row-major)
  Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> dct_map(
      dct_matrix.data(), n_mfcc, n_mels_);
  Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>
      log_mel_map(log_mel.data(), n_mels_, n_frames_);
  Eigen::Map<Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> mfcc_map(
      mfcc_out.data(), n_mfcc, n_frames_);

  // BLAS-optimized matrix multiplication
  mfcc_map.noalias() = dct_map * log_mel_map;

  // Apply liftering if requested.
  // librosa uses np.arange(1, 1 + n_mfcc), so coefficient k (0-indexed) is
  // multiplied by 1 + (L/2) * sin(pi * (k + 1) / L). Using k+1 ensures the DC
  // coefficient (k=0) receives a nonzero lift, matching librosa 0.11.0.
  if (lifter > 0.0f) {
    for (int k = 0; k < n_mfcc; ++k) {
      float lift = 1.0f + (lifter / 2.0f) * std::sin(kPi * static_cast<float>(k + 1) / lifter);
      for (int t = 0; t < n_frames_; ++t) {
        mfcc_out[k * n_frames_ + t] *= lift;
      }
    }
  }

  return mfcc_out;
}

std::vector<float> MelSpectrogram::delta(const float* features, int n_features, int n_frames,
                                         int width) {
  SONARE_CHECK(features != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(n_features > 0 && n_frames > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(width >= 3 && width % 2 == 1, ErrorCode::InvalidParameter);

  int half_width = width / 2;
  std::vector<float> out(n_features * n_frames, 0.0f);

  // Compute denominator for the centered first-order regression (Savitzky-Golay
  // polyorder=1, deriv=1): weight at offset i is i / (2 * sum_{1..h} i^2).
  float denom = 0.0f;
  for (int i = 1; i <= half_width; ++i) {
    denom += static_cast<float>(i * i);
  }
  denom *= 2.0f;

  // librosa uses scipy.signal.savgol_filter(..., mode='interp'): the first and
  // last half_width frames are not edge-clamped but evaluated from a degree-1
  // polynomial fit to the leading/trailing `width`-sample window. For a linear
  // fit the derivative is constant across the window, so every boundary frame on
  // each side takes the least-squares slope of that boundary window. The fit only
  // needs frames inside the boundary window, so it requires width <= n_frames;
  // shorter signals fall back to edge clamping (matching scipy, which switches to
  // 'constant' padding when the window exceeds the data length).
  const bool use_interp_edges = (width <= n_frames);

  // Least-squares slope of the degree-1 fit to `width` consecutive samples
  // starting at `base`. Window x-coordinates are 0..width-1 centered at the mean,
  // so the slope is sum((j - mean_j) * y_j) / sum((j - mean_j)^2).
  auto boundary_slope = [&](const float* row, int base) {
    const float mean_j = static_cast<float>(width - 1) / 2.0f;
    float num = 0.0f;
    for (int j = 0; j < width; ++j) {
      num += (static_cast<float>(j) - mean_j) * row[base + j];
    }
    // sum_{j=0}^{width-1} (j - mean_j)^2 == width * (width^2 - 1) / 12.
    const float ss = static_cast<float>(width) * (static_cast<float>(width) * width - 1.0f) / 12.0f;
    return num / ss;
  };

  // Compute delta for each feature and frame
  for (int k = 0; k < n_features; ++k) {
    const float* row = features + static_cast<std::size_t>(k) * n_frames;
    float left_slope = 0.0f;
    float right_slope = 0.0f;
    if (use_interp_edges) {
      left_slope = boundary_slope(row, 0);
      right_slope = boundary_slope(row, n_frames - width);
    }
    for (int t = 0; t < n_frames; ++t) {
      if (use_interp_edges && t < half_width) {
        out[k * n_frames + t] = left_slope;
        continue;
      }
      if (use_interp_edges && t >= n_frames - half_width) {
        out[k * n_frames + t] = right_slope;
        continue;
      }
      float sum = 0.0f;
      for (int i = 1; i <= half_width; ++i) {
        int t_plus = std::min(t + i, n_frames - 1);
        int t_minus = std::max(t - i, 0);
        sum += static_cast<float>(i) * (row[t_plus] - row[t_minus]);
      }
      out[k * n_frames + t] = sum / denom;
    }
  }

  return out;
}

float MelSpectrogram::at(int mel, int frame) const {
  SONARE_CHECK(mel >= 0 && mel < n_mels_, ErrorCode::InvalidParameter);
  SONARE_CHECK(frame >= 0 && frame < n_frames_, ErrorCode::InvalidParameter);
  return power_[mel * n_frames_ + frame];
}

}  // namespace sonare
