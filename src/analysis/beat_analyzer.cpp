#include "analysis/beat_analyzer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

#include "analysis/analysis_rate.h"
#include "analysis/bpm_analyzer.h"
#include "analysis/downbeat_analyzer.h"
#include "analysis/meter_analyzer.h"
#include "core/convert.h"
#include "feature/onset.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/peak.h"

namespace sonare {

using sonare::constants::kEpsilon;
using sonare::constants::kMsPerSecondD;

namespace {

/// Accent read around an event's own attack, in ms: the attack is the start of the steepest-rise
/// 5 ms frame, so the read opens half a frame early. A +10 ms tail gives the same metered margins
/// as +30 ms on synthetic accent patterns and a lower flat-material maximum (0.85 against 1.02).
constexpr double kAttackAccentLeadMs = 2.5;
constexpr double kAttackAccentTailMs = 10.0;
/// Accent read around a beat with no attack in reach, in ms, both sides widened by half an onset
/// hop for the frame quantization of its reported time. +30 and +60 ms tails scored alike.
constexpr double kFallbackAccentLeadMs = 5.0;
constexpr double kFallbackAccentTailMs = 30.0;

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

  // One STFT feeds both the mel onset envelope and the bar-start evidence; the
  // envelope matches analysis_onset_strength() for the same geometry.
  const RateWindow window = window_at_rate(config.n_fft, audio.sample_rate());
  MelConfig mel_config;
  mel_config.n_fft = window.n_fft;
  mel_config.win_length = window.win_length;
  mel_config.hop_length = config.hop_length;
  mel_config.n_mels = constants::kDefaultNMels;
  OnsetConfig onset_config;
  onset_config.lag = 1;
  onset_config.detrend = true;
  mel_config.center = onset_config.center;
  const StftConfig stft_config = mel_config.to_stft_config();

  const Spectrogram spectrogram = Spectrogram::compute(audio, stft_config);
  const MelSpectrogram mel = MelSpectrogram::from_spectrogram(spectrogram, audio.sample_rate(),
                                                              mel_config.to_mel_filter_config());
  onset_strength_ = center_onset_strength(compute_onset_strength(mel, onset_config), stft_config,
                                          onset_config.center);
  onset_shift_frames_ = onset_center_shift_frames(stft_config, onset_config.center);

  track_beats();
  refine_beat_times(audio, evidence_half_window_s(window.win_length, window.n_fft, sr_));
  // Onset strength is a log-spectral difference, so accents that differ only
  // in amplitude can disappear after dB normalization. Audio-backed analysis
  // can recover that evidence from beat-local low-frequency energy.
  estimate_time_signature(low_frequency_energy_observations(beats_, audio));
  refine_downbeats(downbeat_phase_evidence(spectrogram.power(), spectrogram.n_bins(),
                                           spectrogram.n_frames(), sr_, stft_config.n_fft,
                                           hop_length_, beats_));
}

BeatAnalyzer::BeatAnalyzer(const std::vector<float>& onset_strength, int sr, int hop_length,
                           const BeatConfig& config, int shift_frames)
    : onset_strength_(onset_strength),
      bpm_(config.start_bpm),
      time_signature_{4, 4, 0.0f},
      sr_(sr),
      hop_length_(hop_length),
      onset_shift_frames_(shift_frames),
      config_(config) {
  // No audio, so no bar-start evidence: the downbeats stay seeded at phase 0.
  // Callers that have audio (e.g. MusicAnalyzer) refine afterwards.
  track_beats();
}

void BeatAnalyzer::refine_beat_times(const Audio& audio, float half_window_s) {
  refiner_.emplace(audio);
  refine_half_window_s_ = half_window_s;
  accents_ = {};
  if (beats_.empty()) return;
  std::vector<int> frames;
  frames.reserve(beats_.size());
  for (const Beat& beat : beats_) {
    frames.push_back(beat.frame);
  }
  const std::vector<std::optional<float>> refined = refine_event_times(frames);
  std::vector<float> shifts;
  for (size_t i = 0; i < beats_.size(); ++i) {
    if (refined[i]) shifts.push_back(*refined[i] - beats_[i].time);
  }
  const size_t needed = std::max<size_t>(4, beats_.size() / 4);
  if (shifts.size() >= needed) {
    std::sort(shifts.begin(), shifts.end());
    const size_t mid = shifts.size() / 2;
    const float offset =
        shifts.size() % 2 == 1 ? shifts[mid] : 0.5f * (shifts[mid - 1] + shifts[mid]);
    for (Beat& beat : beats_) {
      beat.time = std::max(0.0f, beat.time + offset);
    }
    for (Beat& beat : downbeats_) {
      beat.time = std::max(0.0f, beat.time + offset);
    }
  }
  read_accents(audio, refined);
}

void BeatAnalyzer::read_accents(const Audio& audio,
                                const std::vector<std::optional<float>>& attacks) {
  const auto at_attack = [&audio](float attack) {
    return peak_short_time_rms(audio, attack - kAttackAccentLeadMs / kMsPerSecondD,
                               attack + kAttackAccentTailMs / kMsPerSecondD);
  };
  const double half_hop_s = 0.5 * hop_length_ / sr_;
  accents_.beat.reserve(beats_.size());
  accents_.beat_has_attack.reserve(beats_.size());
  for (size_t i = 0; i < beats_.size(); ++i) {
    const double t = beats_[i].time;
    accents_.beat.push_back(
        attacks[i]
            ? at_attack(*attacks[i])
            : peak_short_time_rms(audio, t - kFallbackAccentLeadMs / kMsPerSecondD - half_hop_s,
                                  t + kFallbackAccentTailMs / kMsPerSecondD + half_hop_s));
    accents_.beat_has_attack.push_back(attacks[i] ? 1 : 0);
  }

  if (beats_.size() < 2) return;
  // Midpoints in time: rounding a half-integer frame sum puts the search up to 1.5 hops late.
  std::vector<float> midpoints;
  midpoints.reserve(beats_.size() - 1);
  for (size_t i = 0; i + 1 < beats_.size(); ++i) {
    midpoints.push_back(0.5f * (beats_[i].time + beats_[i + 1].time));
  }
  const std::vector<std::optional<float>> midpoint_attacks =
      refiner_ ? refiner_->refine(midpoints, refine_half_window_s_)
               : std::vector<std::optional<float>>(midpoints.size());
  accents_.subdivision.reserve(midpoints.size());
  for (const auto& attack : midpoint_attacks) {
    accents_.subdivision.push_back(attack ? at_attack(*attack) : 0.0f);
  }
}

std::vector<std::optional<float>> BeatAnalyzer::refine_event_times(
    const std::vector<int>& frames) const {
  if (!refiner_) return std::vector<std::optional<float>>(frames.size());
  std::vector<float> centres;
  centres.reserve(frames.size());
  for (int frame : frames) {
    centres.push_back(frames_to_time(frame, sr_, hop_length_));
  }
  return refiner_->refine(centres, refine_half_window_s_);
}

float transition_cost_with_tightness(int from_frame, int to_frame, float period, float tightness) {
  const float interval = static_cast<float>(to_frame - from_frame);
  const float deviation = (interval - period) / period;
  return tightness * deviation * deviation;
}

void BeatAnalyzer::refine_downbeats(const DownbeatObservations& evidence) {
  if (beats_.empty() || (evidence.lf.empty() && evidence.harmonic_change.empty())) {
    return;
  }
  DownbeatResult downbeat_result = select_downbeat_phase(beats_, time_signature_, evidence);
  downbeat_indices_ = std::move(downbeat_result.beat_indices);
  downbeats_ = std::move(downbeat_result.downbeats);
  downbeat_phase_ = downbeat_indices_.empty() ? 0 : downbeat_indices_.front();
}

void BeatAnalyzer::set_chord_change_observations(std::vector<float> chord_changes) {
  chord_change_observations_ = std::move(chord_changes);
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
  std::vector<int> beat_frames_vec;

  // Initialize first beat candidates (within first expected beat period)
  int search_range = static_cast<int>(period * 1.5f);
  search_range = std::min(search_range, n_frames);

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
    std::vector<float> cumulative_score(n_frames, -std::numeric_limits<float>::infinity());
    std::vector<int> backpointer(n_frames, -1);
    for (int i = 0; i < search_range; ++i) {
      cumulative_score[i] = local_score[i];
    }

    for (int i = first_dp_frame; i < n_frames; ++i) {
      const int search_start = std::max(0, i - fixed_window_max);
      const int search_end = std::max(0, i - fixed_window_min);

      float best_score = cumulative_score[i];
      int best_prev = -1;

      for (int j = search_start; j <= search_end; ++j) {
        if (cumulative_score[j] == -std::numeric_limits<float>::infinity()) continue;

        const float transition_cost =
            transition_cost_with_tightness(j, i, period, config_.tightness);
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
    // Clamp frame indices into [0, n_frames-1]: the adaptive DP backtracer and
    // prepend_missed_initial_beat can emit frames slightly outside the valid
    // local_score / onset_strength_ range, which would otherwise be UB.
    const int max_frame_index = n_frames - 1;
    std::vector<float> beat_onsets(beat_frames_vec.size());
    for (size_t i = 0; i < beat_frames_vec.size(); ++i) {
      beat_onsets[i] = onset_strength_[std::clamp(beat_frames_vec[i], 0, max_frame_index)];
    }
    // Reference onset: the highest envelope peak once the loudest few are set aside.
    std::vector<float> peaks;
    for (int i = 1; i + 1 < n_frames; ++i) {
      const size_t k = static_cast<size_t>(i);
      if (onset_strength_[k] > onset_strength_[k - 1] &&
          onset_strength_[k] >= onset_strength_[k + 1]) {
        peaks.push_back(onset_strength_[k]);
      }
    }
    const float reference = peaks.size() > kReferenceIgnoredTopEvents
                                ? max_excluding_top(std::move(peaks), kReferenceIgnoredTopEvents)
                                : *std::max_element(onset_strength_.begin(), onset_strength_.end());
    const float threshold = kOnsetEvidenceRelativeThreshold * reference;
    const bool has_reference = reference >= kEpsilon;

    // Find first frame with significant onset
    int first_valid = -1;
    for (size_t i = 0; has_reference && i < beat_frames_vec.size(); ++i) {
      if (beat_onsets[i] > threshold) {
        first_valid = static_cast<int>(i);
        break;
      }
    }

    // Find last frame with significant onset
    int last_valid = -1;
    for (int i = static_cast<int>(beat_frames_vec.size()) - 1; has_reference && i >= 0; --i) {
      if (beat_onsets[static_cast<size_t>(i)] > threshold) {
        last_valid = i;
        break;
      }
    }

    if (first_valid >= 0 && first_valid <= last_valid) {
      beat_frames_vec = std::vector<int>(beat_frames_vec.begin() + first_valid,
                                         beat_frames_vec.begin() + last_valid + 1);
    } else {
      // No beat carries onset evidence: there is no grid to keep.
      beat_frames_vec.clear();
    }
  }
  prepend_missed_initial_beat(beat_frames_vec, period);

  // Create Beat objects
  beats_.clear();
  beats_.reserve(beat_frames_vec.size());
  // Clamp index into onset_strength_: prepend_missed_initial_beat can insert a
  // 0-frame and the adaptive DP backtracer may emit out-of-range frames, both
  // of which would otherwise read past onset_strength_'s end (UB).
  const int max_onset_index = static_cast<int>(onset_strength_.size()) - 1;
  for (int f : beat_frames_vec) {
    Beat beat;
    beat.frame = f;
    beat.time = envelope_frame_time(f, onset_shift_frames_, sr_, hop_length_);
    beat.strength =
        max_onset_index >= 0 ? onset_strength_[std::clamp(f, 0, max_onset_index)] : 0.0f;
    beats_.push_back(beat);
  }
  if (beats_.size() >= 2 && beats_.front().frame == 0 &&
      beats_.front().strength < beats_[1].strength * 0.1f) {
    beats_.front().strength = beats_[1].strength;
  }

  // Refine BPM from the frame span; event times clamp at 0 and would bias it.
  if (beats_.size() >= 2) {
    const int frame_span = beats_.back().frame - beats_.front().frame;
    if (frame_span > 0) {
      bpm_ = 60.0f * static_cast<float>(sr_) / static_cast<float>(hop_length_) *
             static_cast<float>(beats_.size() - 1) / static_cast<float>(frame_span);
    }
  }

  onset_observations_ = onset_strength_observations(beats_, onset_strength_, sr_, hop_length_);
  // Estimate time signature; it seeds the downbeats at phase 0.
  estimate_time_signature();
}

void BeatAnalyzer::estimate_time_signature(const std::vector<float>& beat_energy) {
  MeterConfig meter_config;
  meter_config.candidate_numerators = config_.meter_candidate_numerators;
  meter_config.denominator = config_.meter_denominator;
  low_frequency_observations_ = beat_energy;
  downbeat_phase_ = 0;

  if (beats_.size() < 8) {
    // Matches MeterAnalyzer's own too-few-beats guard, so the same condition
    // reports the same signature whichever estimator reaches it first.
    time_signature_ = {4, meter_config.denominator, 0.0f};  // Unmeasured default
    time_signature_candidates_ = {time_signature_};
    DownbeatResult downbeat_result = estimate_downbeats(beats_, time_signature_, 0);
    downbeat_indices_ = std::move(downbeat_result.beat_indices);
    downbeats_ = std::move(downbeat_result.downbeats);
    return;
  }

  // The accents read from the audio, when there is audio; the rhythm pass scores the same ones,
  // so the two estimates over these beats cannot disagree.
  MeterResult meter = estimate_meter(beats_, meter_config, accents_, beat_energy);
  time_signature_ = meter.time_signature;
  time_signature_candidates_ = std::move(meter.candidates);
  DownbeatResult downbeat_result = estimate_downbeats(beats_, time_signature_, 0);
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
