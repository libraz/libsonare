#include "analysis/meter_analyzer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace sonare {
namespace {

float normalized_strength(const std::vector<float>& onset_strength, int frame) {
  if (onset_strength.empty() || frame < 0 || frame >= static_cast<int>(onset_strength.size())) {
    return 0.0f;
  }
  const float max_value = *std::max_element(onset_strength.begin(), onset_strength.end());
  if (max_value <= 1e-10f) return 0.0f;
  return onset_strength[static_cast<size_t>(frame)] / max_value;
}

float local_normalized_strength(const std::vector<float>& onset_strength, int frame, int radius) {
  if (onset_strength.empty()) return 0.0f;
  float best = 0.0f;
  for (int offset = -radius; offset <= radius; ++offset) {
    best = std::max(best, normalized_strength(onset_strength, frame + offset));
  }
  return best;
}

float mean_or_zero(float sum, int count) {
  return count > 0 ? sum / static_cast<float>(count) : 0.0f;
}

float compound_subdivision_score(const std::vector<float>& onset_strength,
                                 const std::vector<Beat>& beats) {
  if (onset_strength.empty() || beats.size() < 4) return 0.0f;

  float subdivision_sum = 0.0f;
  float beat_sum = 0.0f;
  int count = 0;
  for (size_t i = 0; i + 1 < beats.size(); ++i) {
    const int midpoint = static_cast<int>(
        std::lround(0.5f * static_cast<float>(beats[i].frame + beats[i + 1].frame)));
    subdivision_sum += local_normalized_strength(onset_strength, midpoint, 2);
    beat_sum += local_normalized_strength(onset_strength, beats[i].frame, 2);
    ++count;
  }
  if (count == 0) return 0.0f;

  const float subdivision = subdivision_sum / static_cast<float>(count);
  const float beat = beat_sum / static_cast<float>(count);
  return beat > 1e-6f ? subdivision / beat : subdivision;
}

}  // namespace

MeterAnalyzer::MeterAnalyzer(const std::vector<float>& onset_strength,
                             const std::vector<Beat>& beats, const MeterConfig& config)
    : config_(config) {
  analyze(onset_strength, beats);
}

void MeterAnalyzer::analyze(const std::vector<float>& onset_strength,
                            const std::vector<Beat>& beats) {
  result_ = {};
  result_.time_signature = {4, config_.denominator, 0.0f};
  result_.candidate_scores.assign(config_.candidate_numerators.size(), 0.0f);

  if (beats.size() < 8 || config_.candidate_numerators.empty()) {
    result_.time_signature.confidence = 0.5f;
    return;
  }

  std::vector<float> beat_strengths;
  beat_strengths.reserve(beats.size());
  for (const auto& beat : beats) {
    beat_strengths.push_back(
        std::max(beat.strength, normalized_strength(onset_strength, beat.frame)));
  }

  float best_score = -1.0f;
  int best_numerator = 4;
  int best_phase = 0;

  for (size_t candidate_index = 0; candidate_index < config_.candidate_numerators.size();
       ++candidate_index) {
    const int numerator = config_.candidate_numerators[candidate_index];
    if (numerator <= 1) continue;

    float candidate_best = -1.0f;
    int candidate_phase = 0;
    for (int phase = 0; phase < numerator; ++phase) {
      float downbeat_sum = 0.0f;
      float strong_sum = 0.0f;
      float weak_sum = 0.0f;
      int downbeat_count = 0;
      int strong_count = 0;
      int weak_count = 0;

      for (size_t i = 0; i < beat_strengths.size(); ++i) {
        const int position = (static_cast<int>(i) - phase + numerator) % numerator;
        const float strength = beat_strengths[i];
        if (position == 0) {
          downbeat_sum += strength;
          ++downbeat_count;
        } else if ((numerator == 4 && position == 2) || (numerator == 6 && position == 3)) {
          strong_sum += strength;
          ++strong_count;
        } else {
          weak_sum += strength;
          ++weak_count;
        }
      }

      const float downbeat = mean_or_zero(downbeat_sum, downbeat_count);
      const float strong = mean_or_zero(strong_sum, strong_count);
      const float weak = mean_or_zero(weak_sum, weak_count);
      float contrast = std::max(0.0f, downbeat - weak) * config_.downbeat_weight +
                       std::max(0.0f, strong - weak) * config_.subdivision_weight;
      if (numerator == 6 && downbeat > 1e-6f) {
        const float midpoint_ratio = strong / downbeat;
        if (midpoint_ratio > 0.85f) {
          contrast *= 0.65f;
        } else if (midpoint_ratio > 0.35f) {
          contrast *= 1.15f;
        }
      }

      int complete_measures = 0;
      float measure_consistency = 0.0f;
      for (size_t i = static_cast<size_t>(phase);
           i + static_cast<size_t>(numerator) < beat_strengths.size();
           i += static_cast<size_t>(numerator)) {
        const float current = beat_strengths[i];
        const float next = beat_strengths[i + static_cast<size_t>(numerator)];
        measure_consistency += 1.0f - std::min(std::abs(current - next), 1.0f);
        ++complete_measures;
      }
      measure_consistency = mean_or_zero(measure_consistency, complete_measures);

      const float score = contrast + config_.measure_weight * measure_consistency;
      if (score > candidate_best) {
        candidate_best = score;
        candidate_phase = phase;
      }
    }

    result_.candidate_scores[candidate_index] = candidate_best;
    if (candidate_best > best_score) {
      best_score = candidate_best;
      best_numerator = numerator;
      best_phase = candidate_phase;
    }
  }

  std::vector<float> sorted_scores = result_.candidate_scores;
  sorted_scores.erase(std::remove_if(sorted_scores.begin(), sorted_scores.end(),
                                     [](float value) { return value < 0.0f; }),
                      sorted_scores.end());
  std::sort(sorted_scores.begin(), sorted_scores.end(), std::greater<float>());
  const float runner_up = sorted_scores.size() > 1 ? sorted_scores[1] : 0.0f;
  const float margin = std::max(0.0f, best_score - runner_up);
  float confidence = std::clamp(0.45f + margin, 0.0f, 1.0f);

  int denominator = config_.denominator;
  const float compound_score = compound_subdivision_score(onset_strength, beats);
  if (best_numerator == 6) {
    if (onset_strength.empty() || compound_score >= config_.compound_subdivision_threshold) {
      denominator = 8;
    } else if (config_.candidate_numerators.size() >= 2) {
      // Resolve the 6-vs-(3|4) ambiguity by score, looking candidates up by
      // value rather than positional index so the result is stable regardless
      // of candidate_numerators ordering.
      const auto score_for = [this](int numerator) {
        for (size_t i = 0; i < config_.candidate_numerators.size(); ++i) {
          if (config_.candidate_numerators[i] == numerator) {
            return result_.candidate_scores[i];
          }
        }
        return -std::numeric_limits<float>::infinity();
      };
      const float score_3 = score_for(3);
      const float score_4 = score_for(4);
      // If neither 3 nor 4 is a candidate, fall back to 4.
      best_numerator = score_3 > score_4 ? 3 : 4;
      denominator = config_.denominator;
      confidence = std::max(0.0f, confidence - 0.15f);
    }
  } else if (best_numerator == 3 && compound_score >= config_.compound_subdivision_threshold) {
    best_numerator = 6;
    denominator = 8;
    confidence = std::max(confidence, 0.55f);
  }

  result_.time_signature = {best_numerator, denominator, confidence};
  result_.downbeat_phase = best_phase;
}

MeterResult estimate_meter(const std::vector<float>& onset_strength, const std::vector<Beat>& beats,
                           const MeterConfig& config) {
  MeterAnalyzer analyzer(onset_strength, beats, config);
  return analyzer.result();
}

}  // namespace sonare
