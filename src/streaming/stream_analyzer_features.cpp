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
  /// Apply window; the padding around it stays zero from construction
  for (int i = 0; i < window_length_; ++i) {
    frame_buffer_[window_offset_ + i] = frame_start[i] * window_[window_offset_ + i];
  }

  /// Forward FFT
  fft_->forward(frame_buffer_.data(), spectrum_.data());

  /// Compute magnitude and power
  int n_bins = this->n_bins();
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
  const int n_bins = this->n_bins();

  Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> fb_map(
      mel_filterbank_.data(), n_mels, n_bins);
  Eigen::Map<const Eigen::VectorXf> power_map(power_.data(), n_bins);
  Eigen::Map<Eigen::VectorXf> mel_map(mel_buffer_.data(), n_mels);
  mel_map.noalias() = fb_map * power_map;

  for (int m = 0; m < n_mels; ++m) {
    mel_log_[m] = std::log(std::max(mel_buffer_[m], kLogAmin));
  }
}

namespace {

/// @brief chroma = filterbank @ power, L2-normalized (more robust than max).
void apply_chroma_filterbank(const float* filterbank, const float* power, int n_bins,
                             float* chroma) {
  for (int c = 0; c < 12; ++c) {
    float sum = 0.0f;
    const float* filter_row = filterbank + c * n_bins;
    for (int k = 0; k < n_bins; ++k) {
      sum += filter_row[k] * power[k];
    }
    chroma[c] = sum;
  }

  float l2_norm = 0.0f;
  for (int c = 0; c < 12; ++c) {
    l2_norm += chroma[c] * chroma[c];
  }
  l2_norm = std::sqrt(l2_norm);
  if (l2_norm > kEpsilon) {
    for (int c = 0; c < 12; ++c) {
      chroma[c] /= l2_norm;
    }
  }
}

}  // namespace

void StreamAnalyzer::compute_chroma() {
  apply_chroma_filterbank(chroma_filterbank_.data(), power_.data(), n_bins(),
                          chroma_buffer_.data());
}

void StreamAnalyzer::feed_key_chroma(size_t end) {
  if (!config_.compute_chroma) return;
  for (; key_fed_pos_ < end; ++key_fed_pos_) {
    key_ring_[key_ring_pos_] = overlap_buffer_[key_fed_pos_];
    if (++key_ring_pos_ == key_ring_.size()) key_ring_pos_ = 0;
    ++key_uncovered_;
    if (--key_until_frame_ == 0) {
      compute_key_chroma(key_window_length_);
      key_until_frame_ = key_hop_length_;
    }
  }
}

void StreamAnalyzer::finish_key_chroma() {
  if (!config_.compute_chroma || key_uncovered_ == 0) return;
  /// Zero-pad the samples since the next frame's start, as the terminal frame does.
  compute_key_chroma(key_window_length_ - key_until_frame_);
  key_until_frame_ = key_hop_length_;
}

void StreamAnalyzer::compute_key_chroma(int n_samples) {
  const size_t ring_size = key_ring_.size();
  const size_t start = (key_ring_pos_ + ring_size - static_cast<size_t>(n_samples)) % ring_size;
  for (int i = 0; i < key_window_length_; ++i) {
    const float sample =
        i < n_samples ? key_ring_[(start + static_cast<size_t>(i)) % ring_size] : 0.0f;
    key_frame_buffer_[key_window_offset_ + i] = sample * key_window_[key_window_offset_ + i];
  }
  key_fft_->forward(key_frame_buffer_.data(), key_spectrum_.data());
  const int key_bins = static_cast<int>(key_power_.size());
  for (int k = 0; k < key_bins; ++k) {
    const float re = key_spectrum_[k].real();
    const float im = key_spectrum_[k].imag();
    key_power_[k] = re * re + im * im;
  }
  apply_chroma_filterbank(key_chroma_filterbank_.data(), key_power_.data(), key_bins,
                          key_chroma_.data());
  for (int c = 0; c < 12; ++c) {
    chroma_sum_[c] += key_chroma_[c];
  }
  ++chroma_frame_count_;
  key_uncovered_ = 0;
}

float StreamAnalyzer::compute_onset() {
  if (!needs_mel_analysis_) {
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
  int n_bins = this->n_bins();

  /// Spectral centroid
  frame.spectral_centroid = compute_centroid_frame(magnitude_.data(), n_bins, frequencies_.data());

  /// Spectral flatness
  frame.spectral_flatness = compute_flatness_frame(magnitude_.data(), n_bins);
}

}  // namespace sonare
