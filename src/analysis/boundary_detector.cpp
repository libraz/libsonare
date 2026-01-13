#include "analysis/boundary_detector.h"

#include <algorithm>
#include <cmath>

#include "core/spectrum.h"
#include "feature/chroma.h"
#include "feature/mel_spectrogram.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare {

namespace {

/// @brief Computes cosine similarity between two feature vectors.
float feature_similarity(const float* a, const float* b, int n) {
  float dot = 0.0f;
  float norm_a = 0.0f;
  float norm_b = 0.0f;

  for (int i = 0; i < n; ++i) {
    dot += a[i] * b[i];
    norm_a += a[i] * a[i];
    norm_b += b[i] * b[i];
  }

  if (norm_a < 1e-10f || norm_b < 1e-10f) {
    return 0.0f;
  }

  return dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
}

/// @brief Normalizes a feature vector (L2 normalization).
void normalize_feature(float* feature, int n) {
  float norm = 0.0f;
  for (int i = 0; i < n; ++i) {
    norm += feature[i] * feature[i];
  }

  if (norm > 1e-10f) {
    norm = std::sqrt(norm);
    for (int i = 0; i < n; ++i) {
      feature[i] /= norm;
    }
  }
}

}  // namespace

BoundaryDetector::BoundaryDetector(const Audio& audio, const BoundaryConfig& config)
    : n_frames_(0),
      n_features_(0),
      sr_(audio.sample_rate()),
      hop_length_(config.hop_length),
      config_(config),
      audio_(audio) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  compute_features();
  compute_self_similarity();
  compute_novelty_curve();
  detect_boundaries();
}

void BoundaryDetector::compute_features() {
  // Compute MFCC features
  std::vector<float> mfcc_features;
  int mfcc_frames = 0;

  if (config_.use_mfcc) {
    MelConfig mel_config;
    mel_config.n_fft = config_.n_fft;
    mel_config.hop_length = config_.hop_length;
    mel_config.n_mels = 128;

    MelSpectrogram mel = MelSpectrogram::compute(audio_, mel_config);
    auto mfcc = mel.mfcc(config_.n_mfcc);

    mfcc_frames = mel.n_frames();

    // Flatten MFCC matrix
    mfcc_features.resize(config_.n_mfcc * mfcc_frames);
    for (int f = 0; f < mfcc_frames; ++f) {
      for (int c = 0; c < config_.n_mfcc; ++c) {
        mfcc_features[f * config_.n_mfcc + c] = mfcc[c * mfcc_frames + f];
      }
    }
  }

  // Compute chroma features
  std::vector<float> chroma_features;
  int chroma_frames = 0;

  if (config_.use_chroma) {
    ChromaConfig chroma_config;
    chroma_config.n_fft = config_.n_fft;
    chroma_config.hop_length = config_.hop_length;

    Chroma chroma = Chroma::compute(audio_, chroma_config);
    chroma_frames = chroma.n_frames();

    // Flatten chroma matrix
    chroma_features.resize(config_.n_chroma * chroma_frames);
    for (int f = 0; f < chroma_frames; ++f) {
      for (int c = 0; c < config_.n_chroma; ++c) {
        chroma_features[f * config_.n_chroma + c] = chroma.at(c, f);
      }
    }
  }

  // Combine features
  if (config_.use_mfcc && config_.use_chroma) {
    n_frames_ = std::min(mfcc_frames, chroma_frames);
    n_features_ = config_.n_mfcc + config_.n_chroma;

    features_.resize(n_frames_ * n_features_);

    for (int f = 0; f < n_frames_; ++f) {
      // Copy MFCC
      for (int c = 0; c < config_.n_mfcc; ++c) {
        features_[f * n_features_ + c] = mfcc_features[f * config_.n_mfcc + c];
      }
      // Copy chroma
      for (int c = 0; c < config_.n_chroma; ++c) {
        features_[f * n_features_ + config_.n_mfcc + c] = chroma_features[f * config_.n_chroma + c];
      }
      // Normalize combined feature
      normalize_feature(&features_[f * n_features_], n_features_);
    }
  } else if (config_.use_mfcc) {
    n_frames_ = mfcc_frames;
    n_features_ = config_.n_mfcc;
    features_ = std::move(mfcc_features);

    for (int f = 0; f < n_frames_; ++f) {
      normalize_feature(&features_[f * n_features_], n_features_);
    }
  } else if (config_.use_chroma) {
    n_frames_ = chroma_frames;
    n_features_ = config_.n_chroma;
    features_ = std::move(chroma_features);

    for (int f = 0; f < n_frames_; ++f) {
      normalize_feature(&features_[f * n_features_], n_features_);
    }
  }
}

void BoundaryDetector::compute_self_similarity() {
  if (n_frames_ == 0) return;

  ssm_.resize(n_frames_ * n_frames_);

  for (int i = 0; i < n_frames_; ++i) {
    for (int j = 0; j < n_frames_; ++j) {
      float sim =
          feature_similarity(&features_[i * n_features_], &features_[j * n_features_], n_features_);
      ssm_[i * n_frames_ + j] = sim;
    }
  }
}

float BoundaryDetector::compute_checkerboard_kernel(int center) const {
  int half_size = config_.kernel_size / 2;

  // Check bounds
  if (center < half_size || center >= n_frames_ - half_size) {
    return 0.0f;
  }

  // Compute checkerboard kernel response
  // The kernel has +1 in upper-left and lower-right quadrants
  // and -1 in upper-right and lower-left quadrants
  float sum = 0.0f;

  for (int i = -half_size; i < half_size; ++i) {
    for (int j = -half_size; j < half_size; ++j) {
      int row = center + i;
      int col = center + j;

      float ssm_val = ssm_[row * n_frames_ + col];

      // Checkerboard pattern: + - / - +
      int sign = ((i < 0 && j < 0) || (i >= 0 && j >= 0)) ? 1 : -1;
      sum += sign * ssm_val;
    }
  }

  // Normalize by kernel size
  int kernel_area = config_.kernel_size * config_.kernel_size;
  return sum / static_cast<float>(kernel_area);
}

void BoundaryDetector::compute_novelty_curve() {
  if (n_frames_ == 0) return;

  novelty_curve_.resize(n_frames_, 0.0f);

  for (int i = 0; i < n_frames_; ++i) {
    novelty_curve_[i] = compute_checkerboard_kernel(i);
  }

  // Normalize novelty curve to [0, 1]
  float max_val = 0.0f;
  for (float val : novelty_curve_) {
    max_val = std::max(max_val, val);
  }

  if (max_val > 1e-10f) {
    for (float& val : novelty_curve_) {
      val = std::max(0.0f, val / max_val);
    }
  }
}

void BoundaryDetector::detect_boundaries() {
  if (novelty_curve_.empty()) return;

  // Convert peak distance to frames
  float hop_duration = static_cast<float>(hop_length_) / sr_;
  int min_peak_distance = static_cast<int>(config_.peak_distance / hop_duration);
  min_peak_distance = std::max(1, min_peak_distance);

  // Find local maxima above threshold
  for (int i = 1; i < n_frames_ - 1; ++i) {
    bool is_peak =
        (novelty_curve_[i] > novelty_curve_[i - 1] && novelty_curve_[i] > novelty_curve_[i + 1]);

    if (is_peak && novelty_curve_[i] >= config_.threshold) {
      // Check minimum distance from previous boundary
      bool far_enough = true;
      if (!boundaries_.empty()) {
        int prev_frame = boundaries_.back().frame;
        if (i - prev_frame < min_peak_distance) {
          far_enough = false;
          // If this peak is stronger, replace the previous one
          if (novelty_curve_[i] > boundaries_.back().strength) {
            boundaries_.back().frame = i;
            boundaries_.back().time = static_cast<float>(i) * hop_duration;
            boundaries_.back().strength = novelty_curve_[i];
          }
        }
      }

      if (far_enough) {
        Boundary boundary;
        boundary.frame = i;
        boundary.time = static_cast<float>(i) * hop_duration;
        boundary.strength = novelty_curve_[i];
        boundaries_.push_back(boundary);
      }
    }
  }
}

std::vector<float> BoundaryDetector::boundary_times() const {
  std::vector<float> times;
  times.reserve(boundaries_.size());
  for (const auto& b : boundaries_) {
    times.push_back(b.time);
  }
  return times;
}

std::vector<float> detect_boundaries(const Audio& audio, const BoundaryConfig& config) {
  BoundaryDetector detector(audio, config);
  return detector.boundary_times();
}

}  // namespace sonare
