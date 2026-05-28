#include "analysis/beat_analyzer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

#include "analysis/bpm_analyzer.h"
#include "analysis/downbeat_analyzer.h"
#include "analysis/meter_analyzer.h"
#include "core/convert.h"
#include "feature/mel_spectrogram.h"
#include "feature/onset.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare {

using sonare::constants::kEpsilon;

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
  if (max_val < constants::kEpsilon) {
    return std::vector<float>(onset_strength.size(), 0.0f);
  }

  std::vector<float> score(onset_strength.size());
  for (size_t i = 0; i < onset_strength.size(); ++i) {
    score[i] = onset_strength[i] / max_val;
  }
  return score;
}

float estimate_period_for_window(const std::vector<float>& local_score, int center, float fallback,
                                 int min_period, int max_period, int context_frames) {
  if (local_score.empty() || fallback <= 0.0f) return fallback;

  const int start = std::max(0, center - context_frames / 2);
  const int end = std::min(static_cast<int>(local_score.size()), center + context_frames / 2);
  if (end - start < max_period + 2) return fallback;

  float best_score = -1.0f;
  int best_lag = static_cast<int>(std::round(fallback));
  for (int lag = min_period; lag <= max_period; ++lag) {
    float score = 0.0f;
    int count = 0;
    for (int i = start + lag; i < end; ++i) {
      score += local_score[i] * local_score[i - lag];
      ++count;
    }
    if (count > 0) {
      score /= static_cast<float>(count);
    }
    const float prior = 1.0f / (1.0f + 0.05f * std::abs(static_cast<float>(lag) - fallback));
    score *= prior;
    if (score > best_score) {
      best_score = score;
      best_lag = lag;
    }
  }

  return static_cast<float>(best_lag);
}

std::vector<float> estimate_local_periods(const std::vector<float>& local_score,
                                          float fallback_period, const BeatConfig& config, int sr,
                                          int hop_length) {
  std::vector<float> periods(local_score.size(), fallback_period);
  if (!config.adaptive_tempo || local_score.empty() || fallback_period < 1.0f) {
    return periods;
  }

  const int min_period =
      std::max(1, static_cast<int>(std::floor(bpm_to_period(config.bpm_max, sr, hop_length))));
  const int max_period = std::max(
      min_period + 1, static_cast<int>(std::ceil(bpm_to_period(config.bpm_min, sr, hop_length))));
  const int interval_beats = std::max(1, config.tempo_update_interval_beats);
  const int context_frames =
      std::max(max_period * 2, static_cast<int>(fallback_period * interval_beats));

  for (size_t frame = 0; frame < local_score.size(); ++frame) {
    periods[frame] =
        estimate_period_for_window(local_score, static_cast<int>(frame), fallback_period,
                                   min_period, max_period, context_frames);
  }

  return periods;
}

void prepend_missed_initial_beat(std::vector<int>& beat_frames, float period) {
  if (beat_frames.size() < 2 || beat_frames.size() > 4 || period < 1.0f ||
      beat_frames.front() <= 0) {
    return;
  }

  std::vector<int> intervals;
  intervals.reserve(beat_frames.size() - 1);
  for (size_t i = 1; i < beat_frames.size(); ++i) {
    const int interval = beat_frames[i] - beat_frames[i - 1];
    if (interval > 0) {
      intervals.push_back(interval);
    }
  }
  if (!intervals.empty()) {
    const size_t mid = intervals.size() / 2;
    std::nth_element(intervals.begin(), intervals.begin() + static_cast<long>(mid),
                     intervals.end());
    period = static_cast<float>(intervals[mid]);
  }

  const int first = beat_frames.front();
  if (std::abs(static_cast<float>(first) - period) > period * 0.25f) {
    return;
  }

  int stable_intervals = 0;
  const size_t interval_count = std::min<size_t>(4, beat_frames.size() - 1);
  for (size_t i = 1; i <= interval_count; ++i) {
    const float interval = static_cast<float>(beat_frames[i] - beat_frames[i - 1]);
    if (std::abs(interval - period) <= period * 0.20f) {
      ++stable_intervals;
    }
  }
  if (stable_intervals < static_cast<int>(std::min<size_t>(3, interval_count))) {
    return;
  }

  const int candidate = std::max(0, static_cast<int>(std::lround(first - period)));
  if (candidate <= std::max(2, static_cast<int>(period * 0.15f))) {
    beat_frames.insert(beat_frames.begin(), candidate);
  }
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
  mel_config.n_mels = constants::kDefaultNMels;

  OnsetConfig onset_config;
  onset_config.lag = 1;
  onset_config.detrend = true;

  onset_strength_ = compute_onset_strength(audio, mel_config, onset_config);

  track_beats();
  refine_downbeats(low_frequency_energy_observations(beats_, audio));
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

float transition_cost_with_tightness(int from_frame, int to_frame, float period, float tightness) {
  const float interval = static_cast<float>(to_frame - from_frame);
  const float deviation = (interval - period) / period;
  return tightness * deviation * deviation;
}

void BeatAnalyzer::refine_downbeats(const std::vector<float>& low_frequency_energy,
                                    const std::vector<float>& chord_changes) {
  if (beats_.empty()) {
    downbeat_indices_.clear();
    downbeats_.clear();
    return;
  }

  DownbeatObservations observations;
  observations.low_frequency_energy = low_frequency_energy;
  observations.chord_changes = chord_changes;
  observations.beat_strengths =
      onset_strength_observations(beats_, onset_strength_, sr_, hop_length_);

  DownbeatResult downbeat_result =
      estimate_downbeats(beats_, time_signature_, downbeat_phase_, observations);
  downbeat_indices_ = std::move(downbeat_result.beat_indices);
  downbeats_ = std::move(downbeat_result.downbeats);
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

  // Dynamic programming: find optimal beat sequence.
  std::vector<float> cumulative_score(n_frames, -std::numeric_limits<float>::infinity());
  std::vector<int> backpointer(n_frames, -1);
  std::vector<int> beat_frames_vec;

  // Initialize first beat candidates (within first expected beat period)
  int search_range = static_cast<int>(period * 1.5f);
  search_range = std::min(search_range, n_frames);
  for (int i = 0; i < search_range; ++i) {
    cumulative_score[i] = local_score[i];
  }

  // Fill DP table
  std::vector<float> local_periods =
      estimate_local_periods(local_score, period, config_, sr_, hop_length_);
  const int fixed_window_min = std::max(1, static_cast<int>(period * 0.5f));
  const int fixed_window_max = std::max(fixed_window_min, static_cast<int>(period * 2.0f));
  const int adaptive_window_min = std::max(
      1, static_cast<int>(std::floor(bpm_to_period(config_.bpm_max, sr_, hop_length_) * 0.5f)));
  const int adaptive_window_max = std::max(
      adaptive_window_min + 1,
      static_cast<int>(std::ceil(bpm_to_period(config_.bpm_min, sr_, hop_length_) * 2.0f)));
  const int first_dp_frame = config_.adaptive_tempo ? adaptive_window_min : fixed_window_min;

  if (config_.adaptive_tempo) {
    constexpr int kStableState = 0;
    constexpr int kChangeState = 1;
    constexpr float kTempoChangePenalty = 0.35f;
    constexpr float kTempoChangeSustainPenalty = 0.06f;
    constexpr float kTempoChangeTightnessScale = 0.35f;

    std::vector<std::array<float, 2>> state_score(
        static_cast<size_t>(n_frames),
        {-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()});
    std::vector<std::array<int, 2>> state_backpointer(static_cast<size_t>(n_frames), {-1, -1});
    std::vector<std::array<int, 2>> state_backstate(static_cast<size_t>(n_frames),
                                                    {kStableState, kChangeState});

    for (int i = 0; i < search_range; ++i) {
      state_score[static_cast<size_t>(i)][kStableState] = local_score[i];
      state_score[static_cast<size_t>(i)][kChangeState] = local_score[i] - kTempoChangePenalty;
    }

    for (int i = first_dp_frame; i < n_frames; ++i) {
      const float target_period = local_periods[static_cast<size_t>(i)];
      int window_min = std::max(adaptive_window_min, static_cast<int>(target_period * 0.5f));
      int window_max = std::min(adaptive_window_max, static_cast<int>(target_period * 2.0f));
      window_min = std::max(1, window_min);
      window_max = std::max(window_min, window_max);

      const int search_start = std::max(0, i - window_max);
      const int search_end = std::max(0, i - window_min);
      for (int j = search_start; j <= search_end; ++j) {
        const auto& previous = state_score[static_cast<size_t>(j)];
        if (previous[kStableState] == -std::numeric_limits<float>::infinity() &&
            previous[kChangeState] == -std::numeric_limits<float>::infinity()) {
          continue;
        }

        const float stable_transition_cost =
            transition_cost_with_tightness(j, i, period, config_.tightness);
        const float stable_from_stable = previous[kStableState] - stable_transition_cost;
        const float stable_from_change =
            previous[kChangeState] - stable_transition_cost - kTempoChangeSustainPenalty;
        const int stable_prev_state =
            stable_from_stable >= stable_from_change ? kStableState : kChangeState;
        const float stable_candidate =
            std::max(stable_from_stable, stable_from_change) + local_score[i];
        if (stable_candidate > state_score[static_cast<size_t>(i)][kStableState]) {
          state_score[static_cast<size_t>(i)][kStableState] = stable_candidate;
          state_backpointer[static_cast<size_t>(i)][kStableState] = j;
          state_backstate[static_cast<size_t>(i)][kStableState] = stable_prev_state;
        }

        const float change_transition_cost = transition_cost_with_tightness(
            j, i, target_period, config_.tightness * kTempoChangeTightnessScale);
        const float change_from_stable =
            previous[kStableState] - change_transition_cost - kTempoChangePenalty;
        const float change_from_change =
            previous[kChangeState] - change_transition_cost - kTempoChangeSustainPenalty;
        const int change_prev_state =
            change_from_stable >= change_from_change ? kStableState : kChangeState;
        const float change_candidate =
            std::max(change_from_stable, change_from_change) + local_score[i];
        if (change_candidate > state_score[static_cast<size_t>(i)][kChangeState]) {
          state_score[static_cast<size_t>(i)][kChangeState] = change_candidate;
          state_backpointer[static_cast<size_t>(i)][kChangeState] = j;
          state_backstate[static_cast<size_t>(i)][kChangeState] = change_prev_state;
        }
      }
    }

    const int end_search_start = std::max(0, n_frames - static_cast<int>(period * 2.0f));
    float best_end_score = -std::numeric_limits<float>::infinity();
    int best_end_frame = n_frames - 1;
    int best_end_state = kStableState;
    for (int i = end_search_start; i < n_frames; ++i) {
      for (int state = 0; state < 2; ++state) {
        if (state_score[static_cast<size_t>(i)][state] > best_end_score) {
          best_end_score = state_score[static_cast<size_t>(i)][state];
          best_end_frame = i;
          best_end_state = state;
        }
      }
    }

    int frame = best_end_frame;
    int state = best_end_state;
    while (frame >= 0) {
      beat_frames_vec.push_back(frame);
      const int previous_frame = state_backpointer[static_cast<size_t>(frame)][state];
      const int previous_state = state_backstate[static_cast<size_t>(frame)][state];
      frame = previous_frame;
      state = previous_state;
    }
  } else {
    for (int i = first_dp_frame; i < n_frames; ++i) {
      const int search_start = std::max(0, i - fixed_window_max);
      const int search_end = std::max(0, i - fixed_window_min);

      float best_score = cumulative_score[i];
      int best_prev = -1;

      for (int j = search_start; j <= search_end; ++j) {
        if (cumulative_score[j] == -std::numeric_limits<float>::infinity()) continue;

        const float transition_cost = compute_transition_cost(j, i, period);
        const float score = cumulative_score[j] + local_score[i] - transition_cost;

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

    const int end_search_start = std::max(0, n_frames - static_cast<int>(period * 2.0f));
    float best_end_score = -std::numeric_limits<float>::infinity();
    int best_end_frame = n_frames - 1;

    for (int i = end_search_start; i < n_frames; ++i) {
      if (cumulative_score[i] > best_end_score) {
        best_end_score = cumulative_score[i];
        best_end_frame = i;
      }
    }

    int frame = best_end_frame;
    while (frame >= 0) {
      beat_frames_vec.push_back(frame);
      frame = backpointer[frame];
    }
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
  prepend_missed_initial_beat(beat_frames_vec, period);

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
  if (beats_.size() >= 2 && beats_.front().frame == 0 &&
      beats_.front().strength < beats_[1].strength * 0.1f) {
    beats_.front().strength = beats_[1].strength;
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
    downbeat_phase_ = 0;
    DownbeatResult downbeat_result = estimate_downbeats(beats_, time_signature_, 0);
    downbeat_indices_ = std::move(downbeat_result.beat_indices);
    downbeats_ = std::move(downbeat_result.downbeats);
    return;
  }

  MeterResult meter = estimate_meter(onset_strength_, beats_);
  time_signature_ = meter.time_signature;
  downbeat_phase_ = meter.downbeat_phase;
  DownbeatResult downbeat_result = estimate_downbeats(beats_, time_signature_, downbeat_phase_);
  downbeat_indices_ = std::move(downbeat_result.beat_indices);
  downbeats_ = std::move(downbeat_result.downbeats);
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
