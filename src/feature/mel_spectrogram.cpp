#include "feature/mel_spectrogram.h"

#include <algorithm>
#include <cmath>

#include "filters/dct.h"
#include "util/exception.h"

namespace sonare {

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

  // Create Mel filterbank
  MelFilterConfig config = mel_config;
  if (config.fmax <= 0.0f) {
    config.fmax = static_cast<float>(sr) / 2.0f;
  }
  std::vector<float> filterbank = create_mel_filterbank(sr, spec.n_fft(), config);

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

std::vector<float> MelSpectrogram::to_db(float ref, float amin) const {
  std::vector<float> db(power_.size());

  float ref_power = ref * ref;
  float log_amin = 10.0f * std::log10(std::max(amin, 1e-20f));

  for (size_t i = 0; i < power_.size(); ++i) {
    float val = power_[i] / ref_power;
    if (val < amin) {
      db[i] = log_amin;
    } else {
      db[i] = 10.0f * std::log10(val);
    }
  }

  return db;
}

std::vector<float> MelSpectrogram::mfcc(int n_mfcc, float lifter) const {
  SONARE_CHECK(n_mfcc > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(n_mfcc <= n_mels_, ErrorCode::InvalidParameter);

  // Compute log Mel spectrogram in dB (librosa compatible)
  // librosa uses: 10 * log10(S / ref) with ref=1.0, amin=1e-10, top_db=80
  // Clipping is done in dB scale: max(log_spec, log_spec.max() - top_db)
  std::vector<float> log_mel(power_.size());
  const float amin = 1e-10f;
  const float top_db = 80.0f;

  // First pass: convert to dB and find max
  float max_db = -1e30f;
  for (size_t i = 0; i < power_.size(); ++i) {
    float val = std::max(power_[i], amin);
    log_mel[i] = 10.0f * std::log10(val);
    max_db = std::max(max_db, log_mel[i]);
  }

  // Second pass: apply top_db clipping in dB scale
  float threshold_db = max_db - top_db;
  for (size_t i = 0; i < power_.size(); ++i) {
    log_mel[i] = std::max(log_mel[i], threshold_db);
  }

  // Apply DCT-II to each frame
  std::vector<float> mfcc_out(n_mfcc * n_frames_);

  // Create DCT matrix
  std::vector<float> dct_matrix = create_dct_matrix(n_mfcc, n_mels_);

  // For each frame: mfcc = dct_matrix @ log_mel_frame
  for (int t = 0; t < n_frames_; ++t) {
    for (int k = 0; k < n_mfcc; ++k) {
      float sum = 0.0f;
      for (int m = 0; m < n_mels_; ++m) {
        sum += dct_matrix[k * n_mels_ + m] * log_mel[m * n_frames_ + t];
      }
      mfcc_out[k * n_frames_ + t] = sum;
    }
  }

  // Apply liftering if requested
  if (lifter > 0.0f) {
    for (int k = 0; k < n_mfcc; ++k) {
      float lift = 1.0f + (lifter / 2.0f) * std::sin(M_PI * static_cast<float>(k) / lifter);
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

  // Compute denominator for regression
  float denom = 0.0f;
  for (int i = 1; i <= half_width; ++i) {
    denom += static_cast<float>(i * i);
  }
  denom *= 2.0f;

  // Compute delta for each feature and frame
  for (int k = 0; k < n_features; ++k) {
    for (int t = 0; t < n_frames; ++t) {
      float sum = 0.0f;
      for (int i = 1; i <= half_width; ++i) {
        int t_plus = std::min(t + i, n_frames - 1);
        int t_minus = std::max(t - i, 0);
        sum += static_cast<float>(i) *
               (features[k * n_frames + t_plus] - features[k * n_frames + t_minus]);
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
