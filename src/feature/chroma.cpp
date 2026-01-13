#include "feature/chroma.h"

#include <algorithm>
#include <cmath>

#include "util/exception.h"

namespace sonare {

Chroma::Chroma() : n_chroma_(0), n_frames_(0), sample_rate_(0), hop_length_(0) {}

Chroma::Chroma(std::vector<float> features, int n_chroma, int n_frames, int sample_rate,
               int hop_length)
    : features_(std::move(features)),
      n_chroma_(n_chroma),
      n_frames_(n_frames),
      sample_rate_(sample_rate),
      hop_length_(hop_length) {}

Chroma Chroma::compute(const Audio& audio, const ChromaConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  // Compute STFT
  Spectrogram spec = Spectrogram::compute(audio, config.to_stft_config());

  return from_spectrogram(spec, audio.sample_rate(), config.to_chroma_filter_config());
}

Chroma Chroma::from_spectrogram(const Spectrogram& spec, int sr,
                                const ChromaFilterConfig& chroma_config) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(sr > 0, ErrorCode::InvalidParameter);

  int n_bins = spec.n_bins();
  int n_frames = spec.n_frames();
  int n_chroma = chroma_config.n_chroma;

  // Create chroma filterbank
  std::vector<float> filterbank = create_chroma_filterbank(sr, spec.n_fft(), chroma_config);

  // Apply filterbank to power spectrum
  const std::vector<float>& power = spec.power();

  std::vector<float> chroma_features =
      apply_chroma_filterbank(power.data(), n_bins, n_frames, filterbank.data(), n_chroma);

  return Chroma(std::move(chroma_features), n_chroma, n_frames, sr, spec.hop_length());
}

float Chroma::duration() const {
  if (sample_rate_ <= 0 || hop_length_ <= 0) {
    return 0.0f;
  }
  return static_cast<float>(n_frames_ * hop_length_) / static_cast<float>(sample_rate_);
}

MatrixView<float> Chroma::features() const {
  return MatrixView<float>(features_.data(), n_chroma_, n_frames_);
}

std::array<float, 12> Chroma::mean_energy() const {
  std::array<float, 12> result = {};

  if (n_frames_ == 0 || n_chroma_ != 12) {
    return result;
  }

  for (int c = 0; c < 12; ++c) {
    float sum = 0.0f;
    for (int t = 0; t < n_frames_; ++t) {
      sum += features_[c * n_frames_ + t];
    }
    result[c] = sum / static_cast<float>(n_frames_);
  }

  return result;
}

std::vector<float> Chroma::normalize(int norm) const {
  std::vector<float> result(features_.size());

  for (int t = 0; t < n_frames_; ++t) {
    float norm_val = 0.0f;

    // Compute norm
    if (norm == 1) {
      // L1 norm
      for (int c = 0; c < n_chroma_; ++c) {
        norm_val += std::abs(features_[c * n_frames_ + t]);
      }
    } else {
      // L2 norm (default)
      for (int c = 0; c < n_chroma_; ++c) {
        float val = features_[c * n_frames_ + t];
        norm_val += val * val;
      }
      norm_val = std::sqrt(norm_val);
    }

    // Normalize
    if (norm_val > 1e-10f) {
      for (int c = 0; c < n_chroma_; ++c) {
        result[c * n_frames_ + t] = features_[c * n_frames_ + t] / norm_val;
      }
    } else {
      // Zero out if norm is too small
      for (int c = 0; c < n_chroma_; ++c) {
        result[c * n_frames_ + t] = 0.0f;
      }
    }
  }

  return result;
}

std::vector<int> Chroma::dominant_pitch_class() const {
  std::vector<int> result(n_frames_);

  for (int t = 0; t < n_frames_; ++t) {
    int max_idx = 0;
    float max_val = features_[t];

    for (int c = 1; c < n_chroma_; ++c) {
      float val = features_[c * n_frames_ + t];
      if (val > max_val) {
        max_val = val;
        max_idx = c;
      }
    }

    result[t] = max_idx;
  }

  return result;
}

float Chroma::at(int chroma, int frame) const {
  SONARE_CHECK(chroma >= 0 && chroma < n_chroma_, ErrorCode::InvalidParameter);
  SONARE_CHECK(frame >= 0 && frame < n_frames_, ErrorCode::InvalidParameter);
  return features_[chroma * n_frames_ + frame];
}

}  // namespace sonare
