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

/// @brief Expected maximum of n independent standard normal draws, by numerator.
/// @details A candidate's score is the best of the numerator's own phases, so a
///          numerator with more phases wins more of that maximum from noise
///          alone: on beat series carrying no meter at all, the unadjusted score
///          rises monotonically with the numerator and the widest candidate wins
///          several times as often as the narrowest. Subtracting the expected
///          maximum puts every candidate's score on the same zero, which is what
///          makes a wide candidate list usable. The entries are exact for the
///          standard normal rather than fitted, and the index is the numerator
///          itself, so 0 and 1 are unused padding.
constexpr float kExpectedMaxOfNormals[kMaxMeterCandidateNumerator + 1] = {
    0.000000f, 0.000000f, 0.564190f, 0.846284f, 1.029375f, 1.162964f, 1.267206f,
    1.352178f, 1.423600f, 1.485013f, 1.538753f, 1.586436f, 1.629228f, 1.667990f,
    1.703382f, 1.735913f, 1.765991f, 1.793942f, 1.820032f, 1.844482f, 1.867475f,
    1.889168f, 1.909692f, 1.929162f, 1.947674f, 1.965315f, 1.982158f, 1.998269f,
    2.013707f, 2.028522f, 2.042761f, 2.056464f, 2.069669f,
};

/// @brief Expected maximum of the grouping search, by numerator.
/// @details The counterpart of kExpectedMaxOfNormals for the second search the
///          scorer runs: within one phase it also picks the best of the ways
///          the bar divides into twos and threes, and that maximum is worth
///          something on beats carrying no meter at all. This table cannot be
///          the same one, and not only because it is indexed by numerator
///          rather than by how many alternatives there are. Phases partition
///          the beats into disjoint groups, so their scores are independent and
///          the expected maximum is the textbook one; groupings share accent
///          positions with each other, and the correlation that creates pulls
///          the expected maximum well below the independent value — 13 beats
///          divide 16 ways but behave like about 5. There is no closed form for
///          a maximum over correlated groups, so these are measured under a
///          null of unstructured beats. They depend only on which positions the
///          groupings share, not on the beat strengths: repeating the
///          measurement over uniform, normal, exponential, log-normal and
///          bimodal strengths, and over spans from 24 to 96 beats, moves no
///          entry by more than 0.04.
///
///          Subtracting this and the phase correction separately treats the two
///          searches as one after the other, which holds while the phase is
///          chosen by the downbeat: at subdivision_weight up to about
///          downbeat_weight the corrected scores stay level across numerators,
///          and past that the grouping term starts choosing the phase too and
///          the correction turns into an over-correction. The default weights
///          sit an order of magnitude inside that.
constexpr float kExpectedMaxOfGroupings[kMaxGroupedMeterNumerator + 1] = {
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.573058f,
    0.568201f, 0.689606f, 0.843203f, 0.905015f, 0.997446f, 1.082682f,
    1.154480f, 1.229718f, 1.294572f, 1.354813f, 1.417368f,
};

/// @brief Margin, in units of the score's own noise, at which meter confidence
///        reaches certainty.
/// @details The scores are standardized, so the gap between the winner and the
///          runner-up is measured in standard deviations and needs a scale to
///          become a confidence. A beat series with no meter separates its top
///          two candidates by well under one unit, and a clear detection by
///          several, so this sits above the former and below the latter.
constexpr float kConfidenceMarginScale = 6.0f;

/// @brief Standardized difference between a group of beats and the whole span.
/// @details Reading the group's mean against the span's own spread, scaled by
///          the group size, is what removes the numerator from the score's
///          noise floor: a plain difference of means is noisier for a wide
///          numerator simply because fewer beats fall on its downbeat.
float group_z_score(float group_mean, int group_size, float overall_mean, float spread) {
  if (group_size < 1 || spread <= constants::kEpsilon) return 0.0f;
  return (group_mean - overall_mean) * std::sqrt(static_cast<float>(group_size)) / spread;
}

/// @brief One way a bar divides, as its group sizes and the beats they start on.
struct Grouping {
  std::vector<int> parts;
  /// @brief Bar positions carrying a secondary accent: every group start except
  ///        the downbeat, which is scored separately.
  std::vector<int> accent_positions;
};

void collect_groupings(int remaining, std::vector<int>& parts, std::vector<std::vector<int>>& out) {
  if (remaining == 0) {
    out.push_back(parts);
    return;
  }
  // Twos before threes, so a numerator whose groupings tie -- which is what an
  // unaccented bar produces -- reports the most subdivided of them. That keeps
  // the compound reading something the beats have to argue for.
  for (const int part : {2, 3}) {
    if (part > remaining) continue;
    parts.push_back(part);
    collect_groupings(remaining - part, parts, out);
    parts.pop_back();
  }
}

/// @brief Every way @p numerator beats divide into groups of two and three.
/// @details A numerator too wide to search gets the single undivided group,
///          which scores exactly as the ungrouped path did: no accent positions
///          means no secondary term.
std::vector<Grouping> meter_groupings(int numerator) {
  std::vector<Grouping> groupings;
  if (numerator >= kMinMeterCandidateNumerator && numerator <= kMaxGroupedMeterNumerator) {
    std::vector<std::vector<int>> partitions;
    std::vector<int> parts;
    collect_groupings(numerator, parts, partitions);
    groupings.reserve(partitions.size());
    for (auto& partition : partitions) {
      Grouping grouping;
      int position = 0;
      for (size_t i = 0; i + 1 < partition.size(); ++i) {
        position += partition[i];
        grouping.accent_positions.push_back(position);
      }
      grouping.parts = std::move(partition);
      groupings.push_back(std::move(grouping));
    }
  }
  if (groupings.empty()) {
    groupings.push_back({{numerator}, {}});
  }
  return groupings;
}

float grouping_search_bias(int numerator) {
  return numerator >= 0 && numerator <= kMaxGroupedMeterNumerator
             ? kExpectedMaxOfGroupings[numerator]
             : 0.0f;
}

bool is_compound_pair(const std::vector<int>& parts) {
  return parts.size() == 2 && parts[0] == 3 && parts[1] == 3;
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
    // No search ran, so the bar is reported undivided rather than carrying the
    // 2+2 that a four would have been given had anything been scored, and
    // searched stays false so a caller can tell this fixed answer from a
    // measured one instead of having to recognize the fallback by its values.
    result_.grouping = {result_.time_signature.numerator};
    result_.candidates = {result_.time_signature};
    return;
  }
  result_.searched = true;

  // Compute the global onset-strength maximum once and reuse it for every
  // normalization rather than rescanning the envelope on each lookup.
  const float onset_max = onset_strength.empty()
                              ? 0.0f
                              : *std::max_element(onset_strength.begin(), onset_strength.end());

  // Without an envelope the per-beat strengths carried on the beats are the only
  // accent source, and they need the same treatment the envelope gets: division
  // by the maximum of whatever series is being read. Clamping them instead
  // saturates every beat above 1 at exactly 1, which erases the accent contrast
  // the whole score is built on -- and the stream this path documents as its
  // input, AnalysisResult::beat_observations.onset_strength, is a raw windowed
  // aggregate in the envelope's own units, so it exceeds 1 on ordinary material.
  const bool use_envelope = onset_max > constants::kEpsilon;
  float beat_strength_max = 0.0f;
  if (!use_envelope) {
    for (const auto& beat : beats) {
      beat_strength_max = std::max(beat_strength_max, beat.strength);
    }
  }

  std::vector<float> beat_strengths;
  beat_strengths.reserve(beats.size());
  for (const auto& beat : beats) {
    float strength = beat.strength;
    if (use_envelope) {
      strength = normalized_strength(onset_strength, beat.frame, onset_max);
    } else if (beat_strength_max > constants::kEpsilon) {
      strength = beat.strength / beat_strength_max;
    }
    beat_strengths.push_back(std::clamp(strength, 0.0f, 1.0f));
  }

  // The span's own centre and spread, which every candidate's accent groups are
  // read against. Computing them once keeps every candidate on one scale.
  const float overall_mean =
      mean_or_zero(std::accumulate(beat_strengths.begin(), beat_strengths.end(), 0.0f),
                   static_cast<int>(beat_strengths.size()));
  float variance = 0.0f;
  for (float strength : beat_strengths) {
    const float deviation = strength - overall_mean;
    variance += deviation * deviation;
  }
  const float spread = std::sqrt(mean_or_zero(variance, static_cast<int>(beat_strengths.size())));

  float best_score = -std::numeric_limits<float>::infinity();
  int best_numerator = 4;
  int best_phase = 0;
  std::vector<int> best_grouping{best_numerator};
  // The phase and grouping that scored best for each candidate, kept alongside
  // the scores so the compound-meter fallback below can adopt the ones
  // belonging to the numerator it substitutes rather than the one it rejected.
  std::vector<int> candidate_phases(config_.candidate_numerators.size(), 0);
  std::vector<std::vector<int>> candidate_groupings(config_.candidate_numerators.size());

  // Scratch reused across candidates: one accumulator per bar position, filled
  // once per phase so each grouping only has to add up the positions it accents.
  std::vector<float> position_sums;
  std::vector<int> position_counts;

  for (size_t candidate_index = 0; candidate_index < config_.candidate_numerators.size();
       ++candidate_index) {
    const int numerator = config_.candidate_numerators[candidate_index];
    if (numerator <= 1) continue;
    const std::vector<Grouping> groupings = meter_groupings(numerator);

    float candidate_best = -std::numeric_limits<float>::infinity();
    int candidate_phase = 0;
    const std::vector<int>* candidate_grouping = &groupings.front().parts;
    for (int phase = 0; phase < numerator; ++phase) {
      position_sums.assign(static_cast<size_t>(numerator), 0.0f);
      position_counts.assign(static_cast<size_t>(numerator), 0);
      for (size_t i = 0; i < beat_strengths.size(); ++i) {
        const size_t position =
            static_cast<size_t>((static_cast<int>(i) - phase + numerator) % numerator);
        position_sums[position] += beat_strengths[i];
        ++position_counts[position];
      }

      const float downbeat = mean_or_zero(position_sums[0], position_counts[0]);
      // Each accent group is read against the whole span rather than against the
      // remaining beats, and scaled by its own size, so the score does not get
      // noisier as the numerator widens and leaves fewer beats on the downbeat.
      // That is the half of the numerator bias the phase search does not cause,
      // and the sign is kept rather than clipped at zero because the correction
      // subtracted below is calibrated on a signed score.
      const float downbeat_contrast =
          group_z_score(downbeat, position_counts[0], overall_mean, spread) *
          config_.downbeat_weight;

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
      const float phase_base = downbeat_contrast + config_.measure_weight * measure_consistency;

      // The downbeat and the bar-to-bar consistency are the same whichever way
      // the bar divides, so only the secondary-accent term separates the
      // groupings from each other.
      for (const Grouping& grouping : groupings) {
        float strong_sum = 0.0f;
        int strong_count = 0;
        for (const int position : grouping.accent_positions) {
          strong_sum += position_sums[static_cast<size_t>(position)];
          strong_count += position_counts[static_cast<size_t>(position)];
        }
        const float strong = mean_or_zero(strong_sum, strong_count);
        const float score = phase_base + group_z_score(strong, strong_count, overall_mean, spread) *
                                             config_.subdivision_weight;
        if (score > candidate_best) {
          candidate_best = score;
          candidate_phase = phase;
          candidate_grouping = &grouping.parts;
        }
      }
    }

    // The candidate keeps the best of both its searches, so it also keeps
    // whatever those two maxima won from noise. Removing the expected maximum of
    // each leaves every candidate measured from the same zero regardless of how
    // many phases and how many groupings it had to choose from. The two are
    // subtracted separately because they are searched at different weights, and
    // each carries the weight of the term it inflated.
    const float phase_search_bias =
        kExpectedMaxOfNormals[std::clamp(numerator, 0, kMaxMeterCandidateNumerator)] *
        config_.downbeat_weight;
    result_.candidate_scores[candidate_index] =
        candidate_best - phase_search_bias -
        grouping_search_bias(numerator) * config_.subdivision_weight;
    candidate_phases[candidate_index] = candidate_phase;
    candidate_groupings[candidate_index] = *candidate_grouping;
    if (result_.candidate_scores[candidate_index] > best_score) {
      best_score = result_.candidate_scores[candidate_index];
      best_numerator = numerator;
      best_phase = candidate_phase;
      best_grouping = *candidate_grouping;
    }
  }

  // The scores are standardized, so the gap to the runner-up is a number of
  // standard deviations and is divided by the scale at which that gap counts as
  // certainty. A single candidate was never compared against anything, so it
  // reports no margin rather than treating its own score as one.
  // Only the top two scores are read, so this is a linear scan rather than a
  // descending sort -- which would also be a second std::sort instantiation over
  // float, for the sake of two elements.
  float top_score = -std::numeric_limits<float>::infinity();
  float second_score = -std::numeric_limits<float>::infinity();
  for (const float score : result_.candidate_scores) {
    if (score > top_score) {
      second_score = top_score;
      top_score = score;
    } else if (score > second_score) {
      second_score = score;
    }
  }
  const float margin =
      result_.candidate_scores.size() > 1 ? std::max(0.0f, top_score - second_score) : 0.0f;
  float confidence = std::clamp(0.45f + margin / kConfidenceMarginScale, 0.0f, 1.0f);

  int denominator = config_.denominator;
  const float compound_score = compound_subdivision_score(onset_strength, beats, onset_max);
  // Whether the question "does a beat divide into three?" can be asked at all.
  // It is answered from energy between the beats, so without an envelope there
  // is no evidence either way -- neither for the promotion to a compound
  // denominator nor for the demotion that the absence of subdivision would
  // otherwise justify.
  const bool subdivision_measurable = !onset_strength.empty();
  // A six only reaches the compound question if that is how its own beats
  // divide. Six grouped as 2+2+2 is a simple meter, and used to be forced
  // through this branch and out the other side as a three or a four; it now
  // keeps the numerator its accents support.
  if (best_numerator == 6 && is_compound_pair(best_grouping)) {
    if (!subdivision_measurable) {
      // Nothing to decide on: report the six the accents support, with the
      // denominator the caller asked for and the grouping {3, 3} that says how
      // its bar divides. Promoting it to 6/8 here would put a beat unit nothing
      // measured into a field callers read as a measurement, and demoting it to
      // a three or a four would discard a numerator the accents did support.
    } else if (compound_score >= config_.compound_subdivision_threshold) {
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
      best_grouping = substitute_index >= 0
                          ? candidate_groupings[static_cast<size_t>(substitute_index)]
                          : std::vector<int>{best_numerator};
      denominator = config_.denominator;
      confidence = std::max(0.0f, confidence - 0.15f);
    } else {
      // Only {6} is a candidate and the compound evidence is weak: we cannot
      // distinguish a compound 6/8 from a simple meter, so keep the reported
      // signature but lower confidence to reflect the unresolved ambiguity
      // rather than emitting it as if it were well-supported.
      confidence = std::max(0.0f, confidence - 0.15f);
    }
  } else if (best_numerator == 3 && subdivision_measurable &&
             compound_score >= config_.compound_subdivision_threshold) {
    // Promoting a three to a compound six pairs its bars, so the grouping is
    // the pair of threes that promotion is defined as; the scored grouping
    // belonged to a numerator that no longer describes the result.
    best_numerator = 6;
    denominator = 8;
    best_grouping = {3, 3};
    confidence = std::max(confidence, 0.55f);
  }

  result_.time_signature = {best_numerator, denominator, confidence};
  result_.downbeat_phase = best_phase;
  result_.grouping = std::move(best_grouping);

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
    // A six is listed as a compound one on the same terms the primary result
    // reaches that reading, its own grouping and the measurability of the
    // subdivision included, so the two cannot disagree about the beat unit of
    // the same numerator.
    const int candidate_denominator =
        numerator == 6 && is_compound_pair(candidate_groupings[i]) && subdivision_measurable &&
                compound_score >= config_.compound_subdivision_threshold
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
