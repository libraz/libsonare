#include <Eigen/Core>
#include <algorithm>
#include <cmath>

#include "core/fft.h"
#include "streaming/stream_analyzer.h"
#include "streaming/stream_analyzer_utils.h"
#include "util/constants.h"
#include "util/math_utils.h"

namespace sonare {

using sonare::constants::kEpsilon;
using namespace streaming_detail;

void StreamAnalyzer::compute_stft(const float* frame_start) {
  /// Apply window
  for (int i = 0; i < config_.n_fft; ++i) {
    frame_buffer_[i] = frame_start[i] * window_[i];
  }

  /// Forward FFT
  fft_->forward(frame_buffer_.data(), spectrum_.data());

  /// Compute magnitude and power
  int n_bins = config_.n_bins();
  for (int k = 0; k < n_bins; ++k) {
    float re = spectrum_[k].real();
    float im = spectrum_[k].imag();
    magnitude_[k] = std::sqrt(re * re + im * im);
    power_[k] = re * re + im * im;
  }
}

void StreamAnalyzer::compute_mel() {
  /// Apply mel filterbank GEMV: mel = filterbank @ power
  /// Eigen GEMV is ~10x faster than scalar at M=128, N=1025.
  const int n_mels = config_.n_mels;
  const int n_bins = config_.n_bins();

  Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> fb_map(
      mel_filterbank_.data(), n_mels, n_bins);
  Eigen::Map<const Eigen::VectorXf> power_map(power_.data(), n_bins);
  Eigen::Map<Eigen::VectorXf> mel_map(mel_buffer_.data(), n_mels);
  mel_map.noalias() = fb_map * power_map;

  for (int m = 0; m < n_mels; ++m) {
    mel_log_[m] = std::log(std::max(mel_buffer_[m], kLogAmin));
  }
}

void StreamAnalyzer::compute_chroma() {
  /// Apply chroma filterbank: chroma = filterbank @ power
  int n_bins = config_.n_bins();

  for (int c = 0; c < 12; ++c) {
    float sum = 0.0f;
    const float* filter_row = chroma_filterbank_.data() + c * n_bins;
    for (int k = 0; k < n_bins; ++k) {
      sum += filter_row[k] * power_[k];
    }
    chroma_buffer_[c] = sum;
  }

  /// Normalize chroma using L2 norm (more robust than max)
  float l2_norm = 0.0f;
  for (int c = 0; c < 12; ++c) {
    l2_norm += chroma_buffer_[c] * chroma_buffer_[c];
  }
  l2_norm = std::sqrt(l2_norm);
  if (l2_norm > kEpsilon) {
    for (int c = 0; c < 12; ++c) {
      chroma_buffer_[c] /= l2_norm;
    }
  }
}

float StreamAnalyzer::compute_onset() {
  if (!config_.compute_mel) {
    return 0.0f;
  }

  float onset = 0.0f;

  if (has_prev_frame_) {
    /// Onset = sum of positive differences in log mel
    for (int m = 0; m < config_.n_mels; ++m) {
      float diff = mel_log_[m] - prev_mel_log_[m];
      if (diff > 0.0f) {
        onset += diff;
      }
    }
  }

  /// Store current mel_log for next frame
  prev_mel_log_ = mel_log_;
  has_prev_frame_ = true;

  return onset;
}

void StreamAnalyzer::compute_spectral_features(StreamFrame& frame) {
  int n_bins = config_.n_bins();

  /// Spectral centroid
  frame.spectral_centroid = compute_centroid_frame(magnitude_.data(), n_bins, frequencies_.data());

  /// Spectral flatness
  frame.spectral_flatness = compute_flatness_frame(magnitude_.data(), n_bins);
}

}  // namespace sonare
