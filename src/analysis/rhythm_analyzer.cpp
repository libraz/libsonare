#include "analysis/rhythm_analyzer.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "analysis/meter_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare {

using sonare::constants::kEpsilon;

RhythmAnalyzer::RhythmAnalyzer(const Audio& audio, const RhythmConfig& config)
    : bpm_(config.start_bpm),
      config_(config),
      sr_(audio.sample_rate()),
      hop_length_(config.hop_length) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  // Create beat analyzer
  BeatConfig beat_config;
  beat_config.start_bpm = config.start_bpm;
  beat_config.bpm_min = config.bpm_min;
  beat_config.bpm_max = config.bpm_max;
  beat_config.n_fft = config.n_fft;
  beat_config.hop_length = config.hop_length;
  beat_config.meter_candidate_numerators = config.meter_candidate_numerators;
  beat_config.meter_denominator = config.meter_denominator;

  BeatAnalyzer beat_analyzer(audio, beat_config);
  beats_ = beat_analyzer.beats();
  bpm_ = beat_analyzer.bpm();
  onset_strength_ = beat_analyzer.onset_strength();

  // Detect onsets from onset strength envelope
  detect_onsets(onset_strength_);

  analyze();
}

RhythmAnalyzer::RhythmAnalyzer(const BeatAnalyzer& beat_analyzer, const RhythmConfig& config)
    : bpm_(beat_analyzer.bpm()),
      config_(config),
      sr_(beat_analyzer.sample_rate()),
      hop_length_(beat_analyzer.hop_length()) {
  beats_ = beat_analyzer.beats();
  onset_strength_ = beat_analyzer.onset_strength();

  // Detect onsets from onset strength envelope
  detect_onsets(onset_strength_);

  analyze();
}

void RhythmAnalyzer::detect_onsets(const std::vector<float>& onset_strength) {
  // Reuse the library's canonical OnsetAnalyzer peak-picker (pre/post-max
  // windows, wait/refractory constraint, adaptive local-average gating) on the
  // beat-aligned onset-strength envelope instead of a divergent local heuristic,
  // so swing/groove analysis shares a single onset definition with
  // OnsetAnalyzer::onset_times() and the two cannot drift apart (analysis#7).
  onset_times_.clear();
  if (onset_strength.size() < 3) {
    return;
  }

  OnsetDetectConfig onset_config;
  onset_config.n_fft = config_.n_fft;
  onset_config.hop_length = hop_length_;
  OnsetAnalyzer onset_analyzer(onset_strength, sr_, hop_length_, onset_config);
  onset_times_ = onset_analyzer.onset_times();
}

float RhythmAnalyzer::calculate_swing_ratio() const {
  if (onset_times_.size() < 4 || beats_.size() < 4) {
    return 0.5f;  // Default to straight
  }

  // Get beat times
  std::vector<float> beat_times;
  beat_times.reserve(beats_.size());
  for (const auto& beat : beats_) {
    beat_times.push_back(beat.time);
  }

  // Calculate onset positions within each beat (0-1)
  std::vector<float> eighth_note_positions;

  for (size_t i = 0; i + 1 < beat_times.size(); ++i) {
    float beat_start = beat_times[i];
    float beat_end = beat_times[i + 1];
    float beat_duration = beat_end - beat_start;

    if (beat_duration <= 0.0f) continue;

    // Find onsets within this beat
    for (float onset : onset_times_) {
      if (onset >= beat_start && onset < beat_end) {
        float position = (onset - beat_start) / beat_duration;
        eighth_note_positions.push_back(position);
      }
    }
  }

  if (eighth_note_positions.size() < 4) {
    return 0.5f;
  }

  // Count onsets near swing vs straight positions
  int swing_count = 0;
  int straight_count = 0;

  for (float pos : eighth_note_positions) {
    // Check swing positions [0.33, 0.67]
    bool matched_swing = false;
    for (float swing_pos : rhythm_constants::kSwingPositions) {
      if (std::abs(pos - swing_pos) < rhythm_constants::kSwingTolerance) {
        swing_count++;
        matched_swing = true;
        break;
      }
    }

    if (!matched_swing) {
      // Check straight positions [0.25, 0.5, 0.75]
      for (float straight_pos : rhythm_constants::kStraightPositions) {
        if (std::abs(pos - straight_pos) < rhythm_constants::kSwingTolerance) {
          straight_count++;
          break;
        }
      }
    }
  }

  int total_count = swing_count + straight_count;
  if (total_count == 0) {
    return 0.5f;
  }

  // Calculate swing ratio: 0.5 + (swing - straight) / (2 * total)
  float swing_ratio = 0.5f + static_cast<float>(swing_count - straight_count) /
                                 (2.0f * static_cast<float>(total_count));

  return std::max(0.0f, std::min(1.0f, swing_ratio));
}

void RhythmAnalyzer::analyze() {
  // One accent measure feeds both the meter estimate and the syncopation score,
  // so the two cannot rest on different readings of the same beats.
  beat_energy_ = beat_local_energy(onset_strength_, sr_, hop_length_);

  // Compute beat intervals
  beat_intervals_.clear();
  if (beats_.size() >= 2) {
    beat_intervals_.reserve(beats_.size() - 1);
    for (size_t i = 1; i < beats_.size(); ++i) {
      beat_intervals_.push_back(beats_[i].time - beats_[i - 1].time);
    }
  }

  detect_time_signature();
  detect_groove_type();
  compute_syncopation();
  compute_regularity();
}

void RhythmAnalyzer::detect_time_signature() {
  MeterConfig meter_config;
  meter_config.candidate_numerators = config_.meter_candidate_numerators;
  meter_config.denominator = config_.meter_denominator;

  // Default to 4 over the requested beat unit
  features_.time_signature.numerator = 4;
  features_.time_signature.denominator = meter_config.denominator;
  features_.time_signature.confidence = 0.5f;

  if (beats_.size() < 8) {
    return;
  }

  // Pass the beat-local energy envelope (the same one BeatAnalyzer scores) so
  // estimate_meter resolves simple-vs-compound meter from the audio instead of
  // being forced into the empty-envelope compound fallback, and so this estimate
  // and BeatAnalyzer's cannot disagree over the same beats.
  //
  // Keep the full MeterResult so the downbeat phase (which beat index the first
  // downbeat falls on) can offset the strong-beat classification in
  // compute_syncopation(), matching how BeatAnalyzer aligns its downbeats.
  MeterResult meter = estimate_meter(beat_energy_, beats_, meter_config);
  features_.time_signature = meter.time_signature;
  downbeat_phase_ = meter.downbeat_phase;
}

void RhythmAnalyzer::detect_groove_type() {
  features_.groove_type = "straight";

  if (beats_.size() < 4) {
    return;
  }

  // Use position-based swing ratio calculation
  float swing_ratio = calculate_swing_ratio();

  // Determine groove type based on swing ratio
  // (matches bpm-detector Python implementation)
  if (swing_ratio > rhythm_constants::kSwingThreshold) {
    features_.groove_type = "swing";
  } else if (swing_ratio > rhythm_constants::kShuffleThreshold) {
    features_.groove_type = "shuffle";
  } else {
    features_.groove_type = "straight";
  }
}

void RhythmAnalyzer::compute_syncopation() {
  features_.syncopation = 0.0f;

  if (beats_.size() < 4) {
    return;
  }

  int beats_per_bar = features_.time_signature.numerator;
  if (beats_per_bar <= 0) beats_per_bar = 4;

  // Read each beat's accent from the beat-local energy envelope rather than from
  // Beat::strength, which is one raw frame and therefore reports the hop jitter
  // of a beat falling between two hops as an accent difference. This is the same
  // envelope the meter estimate above scored, so the two rest on one measure.
  const float accent_max =
      beat_energy_.empty() ? 0.0f : *std::max_element(beat_energy_.begin(), beat_energy_.end());
  const int n_frames = static_cast<int>(beat_energy_.size());

  float strong_sum = 0.0f;
  int strong_count = 0;
  std::vector<float> weak_strengths;
  weak_strengths.reserve(beats_.size());

  for (size_t i = 0; i < beats_.size(); ++i) {
    // Determine position within bar, offset by the estimated downbeat phase so
    // bar_position == 0 lands on the actual downbeat (consistent with BeatAnalyzer).
    const int bar_position =
        ((static_cast<int>(i) - downbeat_phase_) % beats_per_bar + beats_per_bar) % beats_per_bar;

    // Strong beats are typically 0 (downbeat) plus a secondary accent: position 2
    // for 4/4 and position 3 for 6/8 compound meter (consistent with
    // MeterAnalyzer/DownbeatAnalyzer). Position 0 alone covers 3/4.
    const bool is_strong_beat = (bar_position == 0) || (beats_per_bar == 4 && bar_position == 2) ||
                                (beats_per_bar == 6 && bar_position == 3);

    // Bring the accents onto a common [0, 1] scale before comparing them. They
    // are sums of log-power differences whose range depends entirely on the
    // material, so no fixed threshold can tell an accent from an ordinary beat.
    const float accent =
        n_frames == 0
            ? 0.0f
            : beat_energy_[static_cast<size_t>(std::clamp(beats_[i].frame, 0, n_frames - 1))];
    const float scaled = accent_max <= kEpsilon ? 0.0f : accent / accent_max;
    const float normalized = std::clamp(scaled, 0.0f, 1.0f);

    if (is_strong_beat) {
      strong_sum += normalized;
      ++strong_count;
    } else {
      weak_strengths.push_back(normalized);
    }
  }

  if (strong_count == 0 || weak_strengths.empty()) {
    return;
  }

  // Syncopation is accent energy landing where the meter does not expect it,
  // measured against the level the meter does expect: only the part of a
  // weak-position beat that rises above the mean strong-beat level counts, and
  // that excess is reported as a fraction of the same level so the result is a
  // ratio in [0, 1] on every path rather than a clamped absolute quantity.
  const float expected = strong_sum / static_cast<float>(strong_count);
  if (expected <= kEpsilon) {
    return;
  }

  float excess = 0.0f;
  for (float strength : weak_strengths) {
    excess += std::max(0.0f, strength - expected);
  }
  const float mean_excess = excess / static_cast<float>(weak_strengths.size());
  features_.syncopation = std::clamp(mean_excess / expected, 0.0f, 1.0f);
}

void RhythmAnalyzer::compute_regularity() {
  features_.pattern_regularity = 1.0f;
  features_.tempo_stability = 1.0f;

  if (beat_intervals_.empty()) {
    return;
  }

  // Compute mean and standard deviation of beat intervals
  float mean_interval = 0.0f;
  for (float interval : beat_intervals_) {
    mean_interval += interval;
  }
  mean_interval /= static_cast<float>(beat_intervals_.size());

  float variance = 0.0f;
  for (float interval : beat_intervals_) {
    float diff = interval - mean_interval;
    variance += diff * diff;
  }
  variance /= static_cast<float>(beat_intervals_.size());
  float std_dev = std::sqrt(variance);

  // Coefficient of variation (lower = more regular)
  float cv = (mean_interval > 0.0f) ? std_dev / mean_interval : 1.0f;

  // Convert to regularity score (inverse of cv, clamped to [0, 1])
  features_.pattern_regularity = std::max(0.0f, 1.0f - cv * 2.0f);

  // Tempo stability: analyze local tempo changes
  if (beat_intervals_.size() >= 4) {
    std::vector<float> local_tempos;
    local_tempos.reserve(beat_intervals_.size());

    for (float interval : beat_intervals_) {
      if (interval > 0.0f) {
        local_tempos.push_back(60.0f / interval);
      }
    }

    if (local_tempos.size() >= 2) {
      float tempo_mean = 0.0f;
      for (float t : local_tempos) {
        tempo_mean += t;
      }
      tempo_mean /= static_cast<float>(local_tempos.size());

      float tempo_var = 0.0f;
      for (float t : local_tempos) {
        float diff = t - tempo_mean;
        tempo_var += diff * diff;
      }
      tempo_var /= static_cast<float>(local_tempos.size());
      float tempo_std = std::sqrt(tempo_var);

      float tempo_cv = (tempo_mean > 0.0f) ? tempo_std / tempo_mean : 1.0f;
      features_.tempo_stability = std::max(0.0f, 1.0f - tempo_cv * 5.0f);
    }
  }
}

}  // namespace sonare
