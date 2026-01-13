#include "analysis/onset_analyzer.h"

#include <algorithm>
#include <cmath>

#include "feature/mel_spectrogram.h"
#include "feature/onset.h"
#include "util/exception.h"

namespace sonare {

OnsetAnalyzer::OnsetAnalyzer(const Audio& audio, const OnsetDetectConfig& config)
    : sr_(audio.sample_rate()), hop_length_(config.hop_length), config_(config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  // Compute onset strength
  MelConfig mel_config;
  mel_config.n_fft = config.n_fft;
  mel_config.hop_length = config.hop_length;
  mel_config.n_mels = 128;

  // Use the OnsetConfig from feature/onset.h
  OnsetConfig feature_onset_cfg;
  feature_onset_cfg.lag = 1;
  feature_onset_cfg.detrend = true;
  feature_onset_cfg.center = false;

  onset_strength_ = compute_onset_strength(audio, mel_config, feature_onset_cfg);

  detect_onsets();

  if (config_.backtrack) {
    backtrack_onsets();
  }
}

OnsetAnalyzer::OnsetAnalyzer(const std::vector<float>& onset_strength, int sr, int hop_length,
                             const OnsetDetectConfig& config)
    : onset_strength_(onset_strength), sr_(sr), hop_length_(hop_length), config_(config) {
  detect_onsets();

  if (config_.backtrack) {
    backtrack_onsets();
  }
}

void OnsetAnalyzer::detect_onsets() {
  onsets_.clear();

  int n_frames = static_cast<int>(onset_strength_.size());
  if (n_frames == 0) return;

  // Compute adaptive threshold if needed
  float threshold = config_.threshold;
  if (threshold <= 0.0f) {
    // Use mean + delta as threshold
    float mean = 0.0f;
    for (float val : onset_strength_) {
      mean += val;
    }
    mean /= static_cast<float>(n_frames);
    threshold = mean + config_.delta;
  }

  // Track last onset frame for wait constraint
  int last_onset_frame = -config_.wait - 1;

  // Peak picking with local max constraint
  for (int i = config_.pre_max; i < n_frames - config_.post_max; ++i) {
    // Check wait constraint: skip if too close to last onset
    if (i - last_onset_frame <= config_.wait) continue;

    float current = onset_strength_[i];

    // Check if above threshold
    if (current <= threshold) continue;

    // Check if local maximum
    bool is_max = true;
    for (int j = i - config_.pre_max; j <= i + config_.post_max; ++j) {
      if (j != i && onset_strength_[j] >= current) {
        is_max = false;
        break;
      }
    }

    if (!is_max) continue;

    // Compute local average for adaptive thresholding
    float pre_avg = 0.0f;
    int pre_count = 0;
    for (int j = std::max(0, i - config_.pre_avg - config_.pre_max); j < i - config_.pre_max; ++j) {
      pre_avg += onset_strength_[j];
      pre_count++;
    }
    if (pre_count > 0) pre_avg /= static_cast<float>(pre_count);

    float post_avg = 0.0f;
    int post_count = 0;
    for (int j = i + config_.post_max + 1;
         j < std::min(n_frames, i + config_.post_max + config_.post_avg + 1); ++j) {
      post_avg += onset_strength_[j];
      post_count++;
    }
    if (post_count > 0) post_avg /= static_cast<float>(post_count);

    // Check if significantly above local average
    float local_avg = (pre_avg + post_avg) / 2.0f;
    if (current <= local_avg + config_.delta) continue;

    // Convert frame to time
    float time = static_cast<float>(i * hop_length_) / static_cast<float>(sr_);

    onsets_.push_back({time, current});
    last_onset_frame = i;
  }
}

void OnsetAnalyzer::backtrack_onsets() {
  for (auto& onset : onsets_) {
    int frame =
        static_cast<int>(onset.time * static_cast<float>(sr_) / static_cast<float>(hop_length_));

    // Find local minimum before onset
    int best_frame = frame;
    float min_val = onset_strength_[frame];

    for (int i = frame - 1; i >= std::max(0, frame - config_.backtrack_range); --i) {
      if (onset_strength_[i] < min_val) {
        min_val = onset_strength_[i];
        best_frame = i;
      } else if (onset_strength_[i] > min_val * 1.5f) {
        // Stop if we start going up significantly
        break;
      }
    }

    // Update onset time
    onset.time = static_cast<float>(best_frame * hop_length_) / static_cast<float>(sr_);
  }
}

std::vector<float> OnsetAnalyzer::onset_times() const {
  std::vector<float> times;
  times.reserve(onsets_.size());
  for (const auto& onset : onsets_) {
    times.push_back(onset.time);
  }
  return times;
}

std::vector<int> OnsetAnalyzer::onset_frames() const {
  std::vector<int> frames;
  frames.reserve(onsets_.size());
  for (const auto& onset : onsets_) {
    int frame =
        static_cast<int>(onset.time * static_cast<float>(sr_) / static_cast<float>(hop_length_));
    frames.push_back(frame);
  }
  return frames;
}

std::vector<float> detect_onsets(const Audio& audio, const OnsetDetectConfig& config) {
  OnsetAnalyzer analyzer(audio, config);
  return analyzer.onset_times();
}

}  // namespace sonare
