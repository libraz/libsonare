#include "analysis/rhythm_analyzer.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare {

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

  BeatAnalyzer beat_analyzer(audio, beat_config);
  beats_ = beat_analyzer.beats();
  bpm_ = beat_analyzer.bpm();

  // Detect onsets from onset strength envelope
  detect_onsets(beat_analyzer.onset_strength());

  analyze();
}

RhythmAnalyzer::RhythmAnalyzer(const BeatAnalyzer& beat_analyzer, const RhythmConfig& config)
    : bpm_(beat_analyzer.bpm()),
      config_(config),
      sr_(beat_analyzer.sample_rate()),
      hop_length_(beat_analyzer.hop_length()) {
  beats_ = beat_analyzer.beats();

  // Detect onsets from onset strength envelope
  detect_onsets(beat_analyzer.onset_strength());

  analyze();
}

void RhythmAnalyzer::detect_onsets(const std::vector<float>& onset_strength) {
  onset_times_.clear();
  if (onset_strength.size() < 3) {
    return;
  }

  // Find local maxima in onset strength as onset times
  float hop_duration = static_cast<float>(hop_length_) / static_cast<float>(sr_);

  // Calculate threshold (mean + std of onset strength)
  float mean = 0.0f;
  for (float s : onset_strength) {
    mean += s;
  }
  mean /= static_cast<float>(onset_strength.size());

  float variance = 0.0f;
  for (float s : onset_strength) {
    float diff = s - mean;
    variance += diff * diff;
  }
  variance /= static_cast<float>(onset_strength.size());
  float std_dev = std::sqrt(variance);
  float threshold = mean + 0.5f * std_dev;

  // Find peaks above threshold
  for (size_t i = 1; i + 1 < onset_strength.size(); ++i) {
    if (onset_strength[i] > onset_strength[i - 1] && onset_strength[i] > onset_strength[i + 1] &&
        onset_strength[i] > threshold) {
      onset_times_.push_back(static_cast<float>(i) * hop_duration);
    }
  }
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
  float swing_ratio =
      0.5f + static_cast<float>(swing_count - straight_count) / (2.0f * static_cast<float>(total_count));

  return std::max(0.0f, std::min(1.0f, swing_ratio));
}

void RhythmAnalyzer::analyze() {
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
  // Default to 4/4
  features_.time_signature.numerator = 4;
  features_.time_signature.denominator = 4;
  features_.time_signature.confidence = 0.5f;

  if (beats_.size() < 8) {
    return;
  }

  // Collect beat strengths
  std::vector<float> strengths;
  strengths.reserve(beats_.size());
  for (const auto& beat : beats_) {
    strengths.push_back(beat.strength);
  }

  // Test different time signatures (3, 4, 6)
  std::vector<std::pair<int, float>> candidates;

  for (int beats_per_bar : {3, 4, 6}) {
    // Calculate variance of downbeat vs non-downbeat strengths
    // Try each possible phase offset

    float best_score = -1.0f;

    for (int phase = 0; phase < beats_per_bar; ++phase) {
      float downbeat_sum = 0.0f;
      float other_sum = 0.0f;
      int downbeat_count = 0;
      int other_count = 0;

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

      // Score based on contrast
      float score = (other_avg > 0.0f) ? downbeat_avg / other_avg : 1.0f;
      best_score = std::max(best_score, score);
    }

    candidates.push_back({beats_per_bar, best_score});
  }

  // Find best time signature
  auto best = std::max_element(candidates.begin(), candidates.end(),
                               [](const auto& a, const auto& b) { return a.second < b.second; });

  features_.time_signature.numerator = best->first;
  features_.time_signature.denominator = 4;
  features_.time_signature.confidence = std::min(1.0f, best->second / 2.0f);
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

  // Count off-beat accents
  float syncopation_score = 0.0f;
  int count = 0;

  for (size_t i = 0; i < beats_.size(); ++i) {
    // Determine position within bar
    int bar_position = static_cast<int>(i) % beats_per_bar;

    // Strong beats are typically 0 (downbeat) and 2 (for 4/4)
    bool is_strong_beat = (bar_position == 0) || (beats_per_bar == 4 && bar_position == 2) ||
                          (beats_per_bar == 3 && bar_position == 0);

    // Calculate strength relative to expected
    float relative_strength = beats_[i].strength;

    if (!is_strong_beat && relative_strength > 0.6f) {
      syncopation_score += relative_strength;
    }
    count++;
  }

  features_.syncopation = (count > 0) ? syncopation_score / count : 0.0f;
  features_.syncopation = std::min(1.0f, features_.syncopation);
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
