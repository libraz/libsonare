#include "analysis/beat_analyzer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include "analysis/bpm_analyzer.h"
#include "core/convert.h"
#include "feature/mel_spectrogram.h"
#include "feature/onset.h"
#include "util/exception.h"

namespace sonare {

namespace {

/// @brief Converts BPM to period in frames.
float bpm_to_period(float bpm, int sr, int hop_length) {
  if (bpm <= 0.0f) return 0.0f;
  float seconds_per_beat = 60.0f / bpm;
  return seconds_per_beat * static_cast<float>(sr) / static_cast<float>(hop_length);
}

/// @brief Computes local score (onset alignment).
std::vector<float> compute_local_score(const std::vector<float>& onset_strength) {
  if (onset_strength.empty()) return {};

  // Normalize onset strength to [0, 1]
  float max_val = *std::max_element(onset_strength.begin(), onset_strength.end());
  if (max_val < 1e-10f) {
    return std::vector<float>(onset_strength.size(), 0.0f);
  }

  std::vector<float> score(onset_strength.size());
  for (size_t i = 0; i < onset_strength.size(); ++i) {
    score[i] = onset_strength[i] / max_val;
  }
  return score;
}

/// @brief Computes the most likely downbeat phase.
int estimate_downbeat_phase(const std::vector<float>& beat_strengths, int beats_per_bar) {
  if (beat_strengths.empty() || beats_per_bar <= 0) return 0;

  // Sum strengths for each possible phase
  std::vector<float> phase_scores(beats_per_bar, 0.0f);
  for (size_t i = 0; i < beat_strengths.size(); ++i) {
    int phase = static_cast<int>(i) % beats_per_bar;
    phase_scores[phase] += beat_strengths[i];
  }

  // Find phase with maximum total strength (downbeats tend to be stronger)
  auto max_it = std::max_element(phase_scores.begin(), phase_scores.end());
  return static_cast<int>(max_it - phase_scores.begin());
}

}  // namespace

BeatAnalyzer::BeatAnalyzer(const Audio& audio, const BeatConfig& config)
    : bpm_(config.start_bpm),
      time_signature_{4, 4, 0.0f},
      sr_(audio.sample_rate()),
      hop_length_(config.hop_length),
      config_(config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  // Compute onset strength
  MelConfig mel_config;
  mel_config.n_fft = config.n_fft;
  mel_config.hop_length = config.hop_length;
  mel_config.n_mels = 128;

  OnsetConfig onset_config;
  onset_config.lag = 1;
  onset_config.detrend = true;
  onset_config.center = false;

  onset_strength_ = compute_onset_strength(audio, mel_config, onset_config);

  track_beats();
}

BeatAnalyzer::BeatAnalyzer(const std::vector<float>& onset_strength, int sr, int hop_length,
                           const BeatConfig& config)
    : onset_strength_(onset_strength),
      bpm_(config.start_bpm),
      time_signature_{4, 4, 0.0f},
      sr_(sr),
      hop_length_(hop_length),
      config_(config) {
  track_beats();
}

float BeatAnalyzer::compute_transition_cost(int from_frame, int to_frame, float period) const {
  // Cost for deviating from expected beat period
  float interval = static_cast<float>(to_frame - from_frame);
  float deviation = (interval - period) / period;
  return config_.tightness * deviation * deviation;
}

void BeatAnalyzer::track_beats() {
  if (onset_strength_.empty()) {
    beats_.clear();
    return;
  }

  int n_frames = static_cast<int>(onset_strength_.size());

  // First estimate BPM using BpmAnalyzer
  BpmConfig bpm_config;
  bpm_config.bpm_min = config_.bpm_min;
  bpm_config.bpm_max = config_.bpm_max;
  bpm_config.start_bpm = config_.start_bpm;

  BpmAnalyzer bpm_analyzer(onset_strength_, sr_, hop_length_, bpm_config);
  bpm_ = bpm_analyzer.bpm();

  if (bpm_ <= 0.0f) {
    bpm_ = config_.start_bpm;
  }

  // Convert BPM to period in frames
  float period = bpm_to_period(bpm_, sr_, hop_length_);
  if (period < 1.0f) {
    beats_.clear();
    return;
  }

  // Compute local score (onset alignment reward)
  std::vector<float> local_score = compute_local_score(onset_strength_);

  // Dynamic programming: find optimal beat sequence
  // cumulative_score[i] = best score ending at frame i
  // backpointer[i] = previous beat frame for optimal path ending at i
  std::vector<float> cumulative_score(n_frames, -std::numeric_limits<float>::infinity());
  std::vector<int> backpointer(n_frames, -1);

  // Initialize first beat candidates (within first expected beat period)
  int search_range = static_cast<int>(period * 1.5f);
  search_range = std::min(search_range, n_frames);
  for (int i = 0; i < search_range; ++i) {
    cumulative_score[i] = local_score[i];
  }

  // Fill DP table
  int window_min = static_cast<int>(period * 0.5f);
  int window_max = static_cast<int>(period * 2.0f);
  window_min = std::max(1, window_min);

  for (int i = window_min; i < n_frames; ++i) {
    // Search for best previous beat
    int search_start = std::max(0, i - window_max);
    int search_end = std::max(0, i - window_min);

    float best_score = cumulative_score[i];
    int best_prev = -1;

    for (int j = search_start; j <= search_end; ++j) {
      if (cumulative_score[j] == -std::numeric_limits<float>::infinity()) continue;

      float transition_cost = compute_transition_cost(j, i, period);
      float score = cumulative_score[j] + local_score[i] - transition_cost;

      if (score > best_score) {
        best_score = score;
        best_prev = j;
      }
    }

    if (best_prev >= 0) {
      cumulative_score[i] = best_score;
      backpointer[i] = best_prev;
    }
  }

  // Backtrack from best ending point
  // Find best ending frame in the last period
  int end_search_start = std::max(0, n_frames - static_cast<int>(period * 2.0f));
  float best_end_score = -std::numeric_limits<float>::infinity();
  int best_end_frame = n_frames - 1;

  for (int i = end_search_start; i < n_frames; ++i) {
    if (cumulative_score[i] > best_end_score) {
      best_end_score = cumulative_score[i];
      best_end_frame = i;
    }
  }

  // Backtrack to get beat sequence
  std::vector<int> beat_frames_vec;
  int frame = best_end_frame;
  while (frame >= 0) {
    beat_frames_vec.push_back(frame);
    frame = backpointer[frame];
  }

  std::reverse(beat_frames_vec.begin(), beat_frames_vec.end());

  // Trim leading/trailing beats if requested
  if (config_.trim && !beat_frames_vec.empty()) {
    // Find first frame with significant onset
    float threshold = 0.1f;
    int first_valid = 0;
    for (size_t i = 0; i < beat_frames_vec.size(); ++i) {
      if (local_score[beat_frames_vec[i]] > threshold) {
        first_valid = static_cast<int>(i);
        break;
      }
    }

    // Find last frame with significant onset
    int last_valid = static_cast<int>(beat_frames_vec.size()) - 1;
    for (int i = static_cast<int>(beat_frames_vec.size()) - 1; i >= 0; --i) {
      if (local_score[beat_frames_vec[i]] > threshold) {
        last_valid = i;
        break;
      }
    }

    if (first_valid <= last_valid) {
      beat_frames_vec = std::vector<int>(beat_frames_vec.begin() + first_valid,
                                         beat_frames_vec.begin() + last_valid + 1);
    }
  }

  // Create Beat objects
  beats_.clear();
  beats_.reserve(beat_frames_vec.size());
  for (int f : beat_frames_vec) {
    Beat beat;
    beat.frame = f;
    beat.time = frames_to_time(f, sr_, hop_length_);
    beat.strength = onset_strength_[f];
    beats_.push_back(beat);
  }

  // Refine BPM estimate from actual beat intervals
  if (beats_.size() >= 2) {
    float total_interval = 0.0f;
    for (size_t i = 1; i < beats_.size(); ++i) {
      total_interval += beats_[i].time - beats_[i - 1].time;
    }
    float avg_interval = total_interval / static_cast<float>(beats_.size() - 1);
    if (avg_interval > 0.0f) {
      bpm_ = 60.0f / avg_interval;
    }
  }

  // Estimate time signature
  estimate_time_signature();
}

void BeatAnalyzer::estimate_time_signature() {
  if (beats_.size() < 8) {
    time_signature_ = {4, 4, 0.5f};  // Default to 4/4 with low confidence
    return;
  }

  // Collect beat strengths
  std::vector<float> strengths;
  strengths.reserve(beats_.size());
  for (const auto& beat : beats_) {
    strengths.push_back(beat.strength);
  }

  // Test different time signatures (3/4, 4/4, 6/8)
  std::vector<std::pair<int, float>> candidates = {{3, 0.0f}, {4, 0.0f}, {6, 0.0f}};

  for (auto& candidate : candidates) {
    int beats_per_bar = candidate.first;

    // Calculate variance in downbeat strength vs other beats
    float downbeat_sum = 0.0f;
    float other_sum = 0.0f;
    int downbeat_count = 0;
    int other_count = 0;

    int phase = estimate_downbeat_phase(strengths, beats_per_bar);

    for (size_t i = 0; i < strengths.size(); ++i) {
      if (static_cast<int>(i) % beats_per_bar == phase) {
        downbeat_sum += strengths[i];
        downbeat_count++;
      } else {
        other_sum += strengths[i];
        other_count++;
      }
    }

    float downbeat_avg = (downbeat_count > 0) ? downbeat_sum / downbeat_count : 0.0f;
    float other_avg = (other_count > 0) ? other_sum / other_count : 0.0f;

    // Score based on contrast between downbeats and other beats
    if (other_avg > 0.0f) {
      candidate.second = downbeat_avg / other_avg;
    }
  }

  // Find best time signature
  auto best = std::max_element(candidates.begin(), candidates.end(),
                               [](const auto& a, const auto& b) { return a.second < b.second; });

  time_signature_.numerator = best->first;
  time_signature_.denominator = 4;
  time_signature_.confidence = std::min(1.0f, best->second / 2.0f);
}

std::vector<float> BeatAnalyzer::beat_times() const {
  std::vector<float> times;
  times.reserve(beats_.size());
  for (const auto& beat : beats_) {
    times.push_back(beat.time);
  }
  return times;
}

std::vector<int> BeatAnalyzer::beat_frames() const {
  std::vector<int> frames;
  frames.reserve(beats_.size());
  for (const auto& beat : beats_) {
    frames.push_back(beat.frame);
  }
  return frames;
}

std::vector<float> detect_beats(const Audio& audio, const BeatConfig& config) {
  BeatAnalyzer analyzer(audio, config);
  return analyzer.beat_times();
}

}  // namespace sonare
