#include "analysis/onset_analyzer.h"

#include <algorithm>
#include <cmath>

#include "feature/mel_spectrogram.h"
#include "feature/onset.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare {

OnsetAnalyzer::OnsetAnalyzer(const Audio& audio, const OnsetDetectConfig& config)
    : sr_(audio.sample_rate()), hop_length_(config.hop_length), config_(config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  // Compute onset strength
  MelConfig mel_config;
  mel_config.n_fft = config.n_fft;
  mel_config.hop_length = config.hop_length;
  mel_config.n_mels = constants::kDefaultNMels;

  // Use the OnsetConfig from feature/onset.h. This internal detection pipeline
  // opts into detrend explicitly (the public OnsetConfig default is
  // detrend=false to match librosa.onset.onset_strength); removing the slow
  // baseline sharpens peak picking, consistent with the bpm / beat / music /
  // audio-profile analyzers, which all force detrend=true here too.
  OnsetConfig feature_onset_cfg;
  feature_onset_cfg.lag = 1;
  feature_onset_cfg.detrend = true;

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

    // Check if local maximum. Use a strict comparison on the left and a
    // non-strict one on the right so a flat-topped peak (a plateau of equal
    // values) yields exactly its first frame as the onset, instead of being
    // rejected entirely because an equal neighbour exists. This matches
    // librosa's peak picking (and the in-repo onset reference convention
    // `x[i] > x[i-1] && x[i] >= x[i+1]`).
    bool is_max = true;
    for (int j = i - config_.pre_max; j <= i + config_.post_max; ++j) {
      if (j == i) continue;
      const bool dominated =
          (j < i) ? (onset_strength_[j] >= current) : (onset_strength_[j] > current);
      if (dominated) {
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

int OnsetAnalyzer::frame_for_time(float time) const {
  // Round rather than truncate: an onset time is `frame * hop / sr` evaluated in
  // float, and truncating that product back can land one frame early (the
  // quotient comes out a hair below the integer). Rounding recovers the frame
  // the onset was actually detected at, which is what both this accessor and
  // the backtracking search need.
  const double frame =
      static_cast<double>(time) * static_cast<double>(sr_) / static_cast<double>(hop_length_);
  return static_cast<int>(std::lround(frame));
}

void OnsetAnalyzer::backtrack_onsets() {
  if (onset_strength_.empty() || onsets_.empty()) {
    return;
  }
  const int last_frame = static_cast<int>(onset_strength_.size()) - 1;

  std::vector<int> frames;
  frames.reserve(onsets_.size());
  for (const auto& onset : onsets_) {
    // Clamp the derived frame into range: a time at the final onset can land on
    // (or past) onset_strength_.size(), which would read out of bounds.
    frames.push_back(std::clamp(frame_for_time(onset.time), 0, last_frame));
  }

  // One backtracking rule for the whole project. onset_backtrack() stops by
  // comparing neighbouring envelope samples, so it is invariant to the offset
  // and scale of the envelope it is handed. That matters here because this
  // analyzer detrends (see the constructor), which subtracts the mean and
  // leaves most of the envelope negative: a stopping rule that compares a
  // sample against a *scaled* running minimum moves its threshold the wrong way
  // once that minimum is negative, and stops at the first ripple instead of the
  // attack.
  const std::vector<int> backtracked = onset_backtrack(frames, onset_strength_);

  const int max_travel = std::max(0, config_.backtrack_range);
  for (size_t i = 0; i < onsets_.size() && i < backtracked.size(); ++i) {
    // backtrack_range bounds how far an onset may travel; it is a limit on the
    // search, not a stopping rule of its own.
    const int limited = std::max(backtracked[i], frames[i] - max_travel);
    onsets_[i].time = static_cast<float>(limited * hop_length_) / static_cast<float>(sr_);
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
    frames.push_back(frame_for_time(onset.time));
  }
  return frames;
}

std::vector<float> detect_onsets(const Audio& audio, const OnsetDetectConfig& config) {
  OnsetAnalyzer analyzer(audio, config);
  return analyzer.onset_times();
}

}  // namespace sonare
