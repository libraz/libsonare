#include "analysis/meter_analyzer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include "util/constants.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare {

using sonare::constants::kEpsilon;
namespace {

float normalized_strength(const std::vector<float>& onset_strength, int frame, float max_value) {
  if (onset_strength.empty() || frame < 0 || frame >= static_cast<int>(onset_strength.size())) {
    return 0.0f;
  }
  if (max_value <= constants::kEpsilon) return 0.0f;
  return onset_strength[static_cast<size_t>(frame)] / max_value;
}

float local_normalized_strength(const std::vector<float>& onset_strength, int frame, int radius,
                                float max_value) {
  if (onset_strength.empty()) return 0.0f;
  float best = 0.0f;
  for (int offset = -radius; offset <= radius; ++offset) {
    best = std::max(best, normalized_strength(onset_strength, frame + offset, max_value));
  }
  return best;
}

float mean_or_zero(float sum, int count) {
  return count > 0 ? sum / static_cast<float>(count) : 0.0f;
}

float compound_subdivision_score(const std::vector<float>& onset_strength,
                                 const std::vector<Beat>& beats, float max_value) {
  if (onset_strength.empty() || beats.size() < 4) return 0.0f;

  float subdivision_sum = 0.0f;
  float beat_sum = 0.0f;
  int count = 0;
  for (size_t i = 0; i + 1 < beats.size(); ++i) {
    const int midpoint = static_cast<int>(
        std::lround(0.5f * static_cast<float>(beats[i].frame + beats[i + 1].frame)));
    subdivision_sum += local_normalized_strength(onset_strength, midpoint, 2, max_value);
    beat_sum += local_normalized_strength(onset_strength, beats[i].frame, 2, max_value);
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
    result_.candidates = {result_.time_signature};
    return;
  }

  // Compute the global onset-strength maximum once and reuse it for every
  // normalization rather than rescanning the envelope on each lookup.
  const float onset_max = onset_strength.empty()
                              ? 0.0f
                              : *std::max_element(onset_strength.begin(), onset_strength.end());

  std::vector<float> beat_strengths;
  beat_strengths.reserve(beats.size());
  for (const auto& beat : beats) {
    const float strength = onset_max <= constants::kEpsilon
                               ? beat.strength
                               : normalized_strength(onset_strength, beat.frame, onset_max);
    beat_strengths.push_back(std::clamp(strength, 0.0f, 1.0f));
  }

  float best_score = -1.0f;
  int best_numerator = 4;
  int best_phase = 0;
  // The phase that scored best for each candidate, kept alongside the scores so
  // the compound-meter fallback below can adopt the phase belonging to the
  // numerator it substitutes rather than the one it rejected.
  std::vector<int> candidate_phases(config_.candidate_numerators.size(), 0);

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
    candidate_phases[candidate_index] = candidate_phase;
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
  const float compound_score = compound_subdivision_score(onset_strength, beats, onset_max);
  if (best_numerator == 6) {
    if (onset_strength.empty() || compound_score >= config_.compound_subdivision_threshold) {
      denominator = 8;
    } else if (config_.candidate_numerators.size() >= 2) {
      // Resolve the 6-vs-(3|4) ambiguity by score, looking candidates up by
      // value rather than positional index so the result is stable regardless
      // of candidate_numerators ordering.
      const auto index_of = [this](int numerator) -> int {
        for (size_t i = 0; i < config_.candidate_numerators.size(); ++i) {
          if (config_.candidate_numerators[i] == numerator) {
            return static_cast<int>(i);
          }
        }
        return -1;
      };
      const auto score_for = [this, &index_of](int numerator) {
        const int index = index_of(numerator);
        return index < 0 ? -std::numeric_limits<float>::infinity()
                         : result_.candidate_scores[static_cast<size_t>(index)];
      };
      const float score_3 = score_for(3);
      const float score_4 = score_for(4);
      // If neither 3 nor 4 is a candidate, fall back to 4.
      best_numerator = score_3 > score_4 ? 3 : 4;
      // Adopt the phase scored for the substituted numerator. Keeping the
      // phase found for 6 can leave it at or past the new numerator, which
      // breaks the documented [0, numerator) range and offsets every downstream
      // bar-position calculation by a beat. When the substitute is not itself a
      // candidate no phase was scored for it, so wrap the old one instead.
      const int substitute_index = index_of(best_numerator);
      best_phase = substitute_index >= 0 ? candidate_phases[static_cast<size_t>(substitute_index)]
                                         : best_phase % best_numerator;
      denominator = config_.denominator;
      confidence = std::max(0.0f, confidence - 0.15f);
    } else {
      // Only {6} is a candidate and the compound evidence is weak: we cannot
      // distinguish a compound 6/8 from a simple meter, so keep the reported
      // signature but lower confidence to reflect the unresolved ambiguity
      // rather than emitting it as if it were well-supported.
      confidence = std::max(0.0f, confidence - 0.15f);
    }
  } else if (best_numerator == 3 && compound_score >= config_.compound_subdivision_threshold) {
    best_numerator = 6;
    denominator = 8;
    confidence = std::max(confidence, 0.55f);
  }

  result_.time_signature = {best_numerator, denominator, confidence};
  result_.downbeat_phase = best_phase;

  // Preserve the scores already calculated by the multi-comb estimator. The
  // normalized values deliberately express relative candidate support; no new
  // meter inference is performed here.
  const float score_sum =
      std::accumulate(result_.candidate_scores.begin(), result_.candidate_scores.end(), 0.0f,
                      [](float sum, float score) { return sum + std::max(0.0f, score); });
  for (size_t i = 0; i < config_.candidate_numerators.size(); ++i) {
    const int numerator = config_.candidate_numerators[i];
    if (numerator <= 1) continue;
    const float score = std::max(0.0f, result_.candidate_scores[i]);
    const int candidate_denominator =
        numerator == 6 &&
                (onset_strength.empty() || compound_score >= config_.compound_subdivision_threshold)
            ? 8
            : config_.denominator;
    result_.candidates.push_back(
        {numerator, candidate_denominator, score_sum > kEpsilon ? score / score_sum : 0.0f});
  }

  std::stable_sort(
      result_.candidates.begin(), result_.candidates.end(),
      [primary = result_.time_signature](const TimeSignature& lhs, const TimeSignature& rhs) {
        const bool lhs_primary =
            lhs.numerator == primary.numerator && lhs.denominator == primary.denominator;
        const bool rhs_primary =
            rhs.numerator == primary.numerator && rhs.denominator == primary.denominator;
        if (lhs_primary != rhs_primary) return lhs_primary;
        return lhs.confidence > rhs.confidence;
      });

  const bool has_primary = std::any_of(
      result_.candidates.begin(), result_.candidates.end(), [this](const TimeSignature& candidate) {
        return candidate.numerator == result_.time_signature.numerator &&
               candidate.denominator == result_.time_signature.denominator;
      });
  if (!has_primary) {
    result_.candidates.insert(result_.candidates.begin(), result_.time_signature);
  }
}

MeterResult estimate_meter(const std::vector<float>& onset_strength, const std::vector<Beat>& beats,
                           const MeterConfig& config) {
  MeterAnalyzer analyzer(onset_strength, beats, config);
  return analyzer.result();
}

void validate_meter_config(const MeterConfig& config) {
  // An empty candidate set makes the estimator return a fixed low-confidence
  // 4/4 rather than searching, which reads as a detection result. Reject it so
  // a caller who cleared the list is told instead of being answered.
  SONARE_CHECK_MSG(!config.candidate_numerators.empty(), ErrorCode::InvalidParameter,
                   "MeterConfig: candidateNumerators must not be empty");
  SONARE_CHECK_MSG(
      config.candidate_numerators.size() <= static_cast<size_t>(kMaxMeterCandidateNumerators),
      ErrorCode::InvalidParameter, "MeterConfig: candidateNumerators must hold at most 16 entries");
  for (int numerator : config.candidate_numerators) {
    SONARE_CHECK_MSG(
        numerator >= kMinMeterCandidateNumerator && numerator <= kMaxMeterCandidateNumerator,
        ErrorCode::InvalidParameter, "MeterConfig: candidateNumerators entries must be in [2, 32]");
  }
  // Only a power of two is a note value, and the estimator reports 8 itself
  // when it resolves a compound meter, so a non-power-of-two could never
  // round-trip through the reported signature.
  SONARE_CHECK_MSG(config.denominator > 0 && config.denominator <= kMaxMeterDenominator &&
                       (config.denominator & (config.denominator - 1)) == 0,
                   ErrorCode::InvalidParameter,
                   "MeterConfig: denominator must be a power of two in [1, 32]");
  SONARE_CHECK_MSG(numeric::finite_non_negative(config.downbeat_weight),
                   ErrorCode::InvalidParameter,
                   "MeterConfig: downbeatWeight must be finite and non-negative");
  SONARE_CHECK_MSG(numeric::finite_non_negative(config.measure_weight), ErrorCode::InvalidParameter,
                   "MeterConfig: measureWeight must be finite and non-negative");
  SONARE_CHECK_MSG(numeric::finite_non_negative(config.subdivision_weight),
                   ErrorCode::InvalidParameter,
                   "MeterConfig: subdivisionWeight must be finite and non-negative");
  SONARE_CHECK_MSG(numeric::finite_non_negative(config.compound_subdivision_threshold),
                   ErrorCode::InvalidParameter,
                   "MeterConfig: compoundSubdivisionThreshold must be finite and non-negative");
}

MeterResult estimate_meter_from_beats(const std::vector<float>& beat_times,
                                      const std::vector<float>& beat_strengths,
                                      const MeterConfig& config) {
  validate_meter_config(config);
  // Mismatched lengths mean the caller paired the wrong two arrays. Scoring the
  // shorter prefix would answer a question nobody asked, so reject instead.
  SONARE_CHECK_MSG(beat_times.size() == beat_strengths.size(), ErrorCode::InvalidParameter,
                   "estimateMeter: beatTimes and beatStrengths must be the same length");
  // No beats is not a short search, it is nothing to search. The estimator
  // would answer with its fixed low-confidence 4/4, which reads as a detection
  // result — the same objection validate_meter_config makes to an empty
  // candidate set, reached through the same code path. A series that holds at
  // least one beat still gets that default, because there the caller did supply
  // something to score and the confidence is the honest report on it.
  SONARE_CHECK_MSG(!beat_times.empty(), ErrorCode::InvalidParameter,
                   "estimateMeter: beatTimes must not be empty");

  std::vector<Beat> beats;
  beats.reserve(beat_times.size());
  for (size_t i = 0; i < beat_times.size(); ++i) {
    SONARE_CHECK_MSG(numeric::finite_non_negative(beat_times[i]), ErrorCode::InvalidParameter,
                     "estimateMeter: beatTimes entries must be finite and non-negative");
    SONARE_CHECK_MSG(i == 0 || beat_times[i] >= beat_times[i - 1], ErrorCode::InvalidParameter,
                     "estimateMeter: beatTimes must be non-decreasing");
    SONARE_CHECK_MSG(numeric::finite(beat_strengths[i]), ErrorCode::InvalidParameter,
                     "estimateMeter: beatStrengths entries must be finite");
    // The frame index is unused while the onset envelope is empty, which is the
    // only mode this entry point runs in; it is filled positionally so a beat
    // still round-trips as an ordered series.
    beats.push_back({beat_times[i], static_cast<int>(i), beat_strengths[i]});
  }

  return estimate_meter({}, beats, config);
}

}  // namespace sonare
