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

  analyze();
}

RhythmAnalyzer::RhythmAnalyzer(const BeatAnalyzer& beat_analyzer, const RhythmConfig& config)
    : bpm_(beat_analyzer.bpm()),
      config_(config),
      sr_(beat_analyzer.sample_rate()),
      hop_length_(beat_analyzer.hop_length()) {
  beats_ = beat_analyzer.beats();

  analyze();
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

  if (beat_intervals_.size() < 4) {
    return;
  }

  // Analyze ratio between consecutive beat intervals
  // Shuffle/swing typically has alternating long-short pattern
  std::vector<float> ratios;
  ratios.reserve(beat_intervals_.size() - 1);

  for (size_t i = 1; i < beat_intervals_.size(); ++i) {
    if (beat_intervals_[i] > 1e-6f) {
      float ratio = beat_intervals_[i - 1] / beat_intervals_[i];
      ratios.push_back(ratio);
    }
  }

  if (ratios.empty()) {
    return;
  }

  // Check for swing pattern (alternating 2:1 or 1:2 ratios)
  int swing_count = 0;

  for (size_t i = 0; i < ratios.size(); ++i) {
    float ratio = ratios[i];
    // Swing ratio is typically around 2:1 or 1:2
    if ((ratio > 1.5f && ratio < 2.5f) || (ratio > 0.4f && ratio < 0.67f)) {
      swing_count++;
    }
  }

  float swing_score = (ratios.size() > 0) ? static_cast<float>(swing_count) / ratios.size() : 0.0f;

  // Check for shuffle (specific long-short-long-short pattern in triplet feel)
  float shuffle_evidence = 0.0f;

  // In shuffle, the ratio is typically around 2:1 (triplet swing)
  for (size_t i = 0; i + 1 < ratios.size(); i += 2) {
    float r1 = ratios[i];
    float r2 = (i + 1 < ratios.size()) ? ratios[i + 1] : 1.0f;

    // Shuffle: first interval longer, second shorter, alternating
    if (r1 > 1.3f && r1 < 2.5f && r2 > 0.4f && r2 < 0.8f) {
      shuffle_evidence += 1.0f;
    }
  }

  shuffle_evidence /= std::max(1.0f, static_cast<float>(ratios.size() / 2));

  // Determine groove type
  if (shuffle_evidence > config_.swing_threshold && shuffle_evidence > swing_score) {
    features_.groove_type = "shuffle";
  } else if (swing_score > config_.swing_threshold) {
    features_.groove_type = "swing";
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
