#include "analysis/boundary_detector.h"

#include <Eigen/Core>
#include <algorithm>
#include <climits>
#include <cmath>

#include "core/resample.h"
#include "core/spectrum.h"
#include "feature/chroma.h"
#include "feature/mel_spectrogram.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare {

using sonare::constants::kEpsilon;

namespace {

/// @brief Normalizes a feature vector (L2 normalization).
void normalize_feature(float* feature, int n) {
  float norm = 0.0f;
  for (int i = 0; i < n; ++i) {
    norm += feature[i] * feature[i];
  }

  if (norm > constants::kEpsilon) {
    norm = std::sqrt(norm);
    for (int i = 0; i < n; ++i) {
      feature[i] /= norm;
    }
  }
}

/// @brief Returns the audio the detector analyses, at most the analysis rate.
/// @details Boundary times must not move with the source rate, so a higher-rate
/// input is resampled before any feature is computed. Matches the resampling the
/// section analysis layered on this detector applies to the same input, so the
/// two entry points segment on one time grid.
Audio boundary_analysis_audio(const Audio& audio) {
  constexpr int kAnalysisSampleRate = constants::kDefaultSampleRate;
  if (!audio.empty() && audio.sample_rate() > kAnalysisSampleRate) {
    return resample(audio, kAnalysisSampleRate);
  }
  return audio;
}

/// @brief Stored half-bandwidth of the self-similarity band.
/// @details The checkerboard kernel centred on frame c reads ssm(c + i, c + j)
/// for i and j in [-half, half), so `row - col` spans [-(2 * half - 1),
/// 2 * half - 1] and nothing outside that band is reachable.
int band_radius(int kernel_size) {
  const int half = std::max(kernel_size, 0) / 2;
  return half > 0 ? 2 * half - 1 : 0;
}

}  // namespace

size_t boundary_ssm_band_width(int kernel_size) {
  return 2u * static_cast<size_t>(band_radius(kernel_size)) + 1u;
}

int boundary_pooled_target_frames(int kernel_size) {
  const size_t per_frame = boundary_ssm_band_width(kernel_size) * sizeof(float);
  const size_t frames = kBoundarySsmBudgetBytes / per_frame;  // per_frame >= sizeof(float)
  return frames > 0 ? static_cast<int>(std::min<size_t>(frames, INT_MAX)) : 1;
}

int boundary_pooling_stride(int n_frames, int kernel_size) {
  const int target = boundary_pooled_target_frames(kernel_size);
  if (n_frames <= target) return 1;
  // Ceil-divide without the `n + target - 1` form, which would overflow for a
  // frame count near INT_MAX.
  return 1 + (n_frames - 1) / target;
}

int boundary_ssm_frames(int n_frames, int kernel_size) {
  if (n_frames <= 0) return 0;
  return 1 + (n_frames - 1) / boundary_pooling_stride(n_frames, kernel_size);
}

BoundaryDetector::BoundaryDetector(const Audio& audio, const BoundaryConfig& config)
    : n_frames_(0),
      n_features_(0),
      audio_(boundary_analysis_audio(audio)),
      sr_(audio_.sample_rate()),
      hop_length_(config.hop_length),
      config_(config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  compute_features();
  compute_self_similarity();
  compute_novelty_curve();
  detect_boundaries();
}

BoundaryDetector::BoundaryDetector(const MelSpectrogram& mel, const Chroma& chroma, int sr,
                                   const BoundaryConfig& config)
    : n_frames_(0), n_features_(0), sr_(sr), hop_length_(config.hop_length), config_(config) {
  // Extract features from pre-computed mel spectrogram and chroma
  std::vector<float> mfcc_features;
  int mfcc_frames = 0;

  if (config_.use_mfcc) {
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

  // Extract chroma features
  std::vector<float> chroma_features;
  int chroma_frames = 0;

  if (config_.use_chroma) {
    chroma_frames = chroma.n_frames();

    // Flatten chroma matrix
    chroma_features.resize(config_.n_chroma * chroma_frames);
    for (int f = 0; f < chroma_frames; ++f) {
      for (int c = 0; c < config_.n_chroma; ++c) {
        chroma_features[f * config_.n_chroma + c] = chroma.at(c, f);
      }
    }
  }

  combine_features(mfcc_features, mfcc_frames, chroma_features, chroma_frames);

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
    mel_config.n_mels = constants::kDefaultNMels;

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
    // The flatten loop below reads config_.n_chroma bins per frame, so the
    // chromagram has to be computed with that bin count and not the default.
    chroma_config.n_chroma = config_.n_chroma;

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

  combine_features(mfcc_features, mfcc_frames, chroma_features, chroma_frames);
}

void BoundaryDetector::combine_features(const std::vector<float>& mfcc_features, int mfcc_frames,
                                        const std::vector<float>& chroma_features,
                                        int chroma_frames) {
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
    }
  } else if (config_.use_mfcc) {
    n_frames_ = mfcc_frames;
    n_features_ = config_.n_mfcc;
    features_ = mfcc_features;
  } else if (config_.use_chroma) {
    n_frames_ = chroma_frames;
    n_features_ = config_.n_chroma;
    features_ = chroma_features;
  }

  // A configuration that enables a feature stream but gives it no dimensions
  // has nothing to normalize, and taking &features_[0] of the empty grid would
  // bind a reference past the end. n_frames_ is left as computed so the zeroed
  // similarity band is still sized the way compute_self_similarity() documents
  // for a dimensionless configuration.
  if (n_features_ <= 0) {
    n_features_ = 0;
    features_.clear();
    return;
  }

  // Features are stored one frame per row, so each row normalizes independently.
  for (int f = 0; f < n_frames_; ++f) {
    normalize_feature(&features_[f * n_features_], n_features_);
  }
}

void BoundaryDetector::downsample_features() {
  if (n_features_ <= 0) return;

  const int stride = boundary_pooling_stride(n_frames_, config_.kernel_size);
  if (stride <= 1) return;
  const int reduced = boundary_ssm_frames(n_frames_, config_.kernel_size);

  std::vector<float> pooled(static_cast<size_t>(reduced) * static_cast<size_t>(n_features_), 0.0f);
  for (int r = 0; r < reduced; ++r) {
    const int begin = r * stride;
    const int end = std::min(begin + stride, n_frames_);
    float* dst = &pooled[static_cast<size_t>(r) * static_cast<size_t>(n_features_)];
    for (int f = begin; f < end; ++f) {
      const float* src = &features_[static_cast<size_t>(f) * static_cast<size_t>(n_features_)];
      for (int c = 0; c < n_features_; ++c) {
        dst[c] += src[c];
      }
    }
    // Mean-pool then re-L2-normalize so cosine similarity stays a plain dot
    // product (the SSM relies on unit-length feature vectors).
    const float inv = 1.0f / static_cast<float>(end - begin);
    for (int c = 0; c < n_features_; ++c) {
      dst[c] *= inv;
    }
    normalize_feature(dst, n_features_);
  }

  features_ = std::move(pooled);
  n_frames_ = reduced;
  frame_stride_ = stride;
}

void BoundaryDetector::compute_self_similarity() {
  if (n_frames_ == 0) return;

  // compute_checkerboard_kernel is the only reader of the self-similarity matrix
  // and only ever touches a band of diagonals around it, so only that band is
  // stored: the working set is O(n_frames_ * band width) instead of
  // O(n_frames_^2). Pooling remains as a budget backstop for a pathological
  // length; for every realistic input it is a no-op and full time resolution is
  // preserved.
  downsample_features();

  // Clamp to what can actually be indexed. Whenever the kernel guard admits any
  // centre at all we have n_frames_ > 2 * half_size > band_radius, so this never
  // binds for a configuration that reads the band; it only stops an absurd
  // kernel_size from sizing the allocation.
  band_radius_ = std::min(band_radius(config_.kernel_size), std::max(n_frames_ - 1, 0));
  const size_t width = 2u * static_cast<size_t>(band_radius_) + 1u;

  // Budget backstop: pooling has already brought the frame count within the band
  // budget, so fail loudly rather than allocate past it if that ever stops holding.
  SONARE_CHECK(static_cast<size_t>(n_frames_) * width * sizeof(float) <= kBoundarySsmBudgetBytes,
               ErrorCode::InvalidState);

  ssm_band_.assign(static_cast<size_t>(n_frames_) * width, 0.0f);

  // A configuration with no feature dimensions leaves the band at zero, which is
  // what the full-matrix product produced for it too. Return before indexing
  // features_, which is empty in that case.
  if (n_features_ <= 0) return;

  // Features are already L2-normalized, so cosine similarity is a plain dot
  // product. Row r stores ssm(r, col) for col in [r - band_radius_, r + band_radius_]
  // at offset (col - r + band_radius_); entries clipped by the ends stay zero and
  // are never read.
  for (int row = 0; row < n_frames_; ++row) {
    Eigen::Map<const Eigen::VectorXf> lhs(
        &features_[static_cast<size_t>(row) * static_cast<size_t>(n_features_)], n_features_);
    const int col_begin = std::max(0, row - band_radius_);
    const int col_end = std::min(n_frames_ - 1, row + band_radius_);
    float* dst = &ssm_band_[static_cast<size_t>(row) * width];
    for (int col = col_begin; col <= col_end; ++col) {
      Eigen::Map<const Eigen::VectorXf> rhs(
          &features_[static_cast<size_t>(col) * static_cast<size_t>(n_features_)], n_features_);
      dst[col - row + band_radius_] = lhs.dot(rhs);
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
  const size_t width = 2u * static_cast<size_t>(band_radius_) + 1u;
  float sum = 0.0f;

  for (int i = -half_size; i < half_size; ++i) {
    int row = center + i;
    // Every (row, col) this loop touches satisfies |row - col| = |i - j| <=
    // 2 * half_size - 1 == band_radius_, so it lies inside the stored band.
    const float* band_row = &ssm_band_[static_cast<size_t>(row) * width];

    for (int j = -half_size; j < half_size; ++j) {
      int col = center + j;

      float ssm_val = band_row[col - row + band_radius_];

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

  // Normalize novelty curve to [0, 1]. The divisor is kept: it is the only link
  // back to the absolute scale, which self-normalization otherwise destroys.
  float max_val = 0.0f;
  for (float val : novelty_curve_) {
    max_val = std::max(max_val, val);
  }

  if (max_val > constants::kEpsilon) {
    novelty_peak_ = max_val;
    for (float& val : novelty_curve_) {
      val = std::max(0.0f, val / max_val);
    }
  }
}

void BoundaryDetector::detect_boundaries() {
  if (novelty_curve_.empty()) return;

  // Convert peak distance to frames. Under long-form pooling each analysis frame
  // spans frame_stride_ hops, so the effective hop duration scales accordingly;
  // frame_stride_ is 1 (no change) for all normal-length inputs.
  float hop_duration = static_cast<float>(hop_length_) * static_cast<float>(frame_stride_) / sr_;
  int min_peak_distance = static_cast<int>(config_.peak_distance / hop_duration);
  min_peak_distance = std::max(1, min_peak_distance);

  // Find local maxima above both thresholds. The relative one ranks peaks within
  // this track; the absolute one asks whether the features changed at all, which
  // the normalized curve cannot answer because it was scaled by its own maximum.
  // novelty_peak_ is 0 exactly when the curve was left unnormalized for sitting
  // under the numerical floor, and multiplying by it rejects that curve too.
  for (int i = 1; i < n_frames_ - 1; ++i) {
    bool is_peak =
        (novelty_curve_[i] > novelty_curve_[i - 1] && novelty_curve_[i] > novelty_curve_[i + 1]);
    const float absolute_novelty = novelty_curve_[i] * novelty_peak_;

    if (is_peak && novelty_curve_[i] >= config_.threshold &&
        absolute_novelty >= config_.absolute_threshold) {
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
