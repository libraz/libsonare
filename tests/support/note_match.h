#pragma once

/// @file note_match.h
/// @brief One-to-one note matching for transcription evaluation (onset / pitch / offset).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "util/constants.h"

namespace sonare::test {

/// @brief A note in seconds with a (possibly fractional) MIDI pitch.
struct EvalNote {
  double onset_sec = 0.0;
  double offset_sec = 0.0;
  double midi = 0.0;
};

/// @brief Tolerances deciding whether an estimated note matches a reference note.
struct NoteMatchCriteria {
  double onset_tolerance_sec = 0.050;
  double pitch_tolerance_cents = 50.0;  ///< +inf ignores pitch
  bool match_offset = false;
  double offset_ratio = 0.2;  ///< offset tolerance = max(min tolerance, ratio * reference length)
  double offset_min_tolerance_sec = 0.050;
};

/// @brief Matched / unmatched note counts with derived precision, recall and F-measure.
struct NoteMatchCounts {
  size_t true_positive = 0;
  size_t false_positive = 0;
  size_t false_negative = 0;

  /// @brief Precision, 0 when there are no estimates.
  double precision() const {
    const size_t denominator = true_positive + false_positive;
    return denominator == 0 ? 0.0 : static_cast<double>(true_positive) / denominator;
  }

  /// @brief Recall, 0 when there are no reference notes.
  double recall() const {
    const size_t denominator = true_positive + false_negative;
    return denominator == 0 ? 0.0 : static_cast<double>(true_positive) / denominator;
  }

  /// @brief Harmonic mean of precision and recall, 0 when both are 0.
  double f_measure() const {
    const double p = precision();
    const double r = recall();
    return p + r > 0.0 ? 2.0 * p * r / (p + r) : 0.0;
  }
};

/// @brief Converts a frequency in Hz (finite, positive) to a fractional MIDI note number.
inline double midi_from_hz(double hz) {
  return static_cast<double>(::sonare::constants::kMidiA4) +
         static_cast<double>(::sonare::constants::kSemitonesPerOctave) *
             std::log2(hz / static_cast<double>(::sonare::constants::kA4Hz));
}

/// @brief Maximum one-to-one matching of estimates to reference notes (augmenting paths).
inline NoteMatchCounts match_notes(const std::vector<EvalNote>& truth,
                                   const std::vector<EvalNote>& estimates,
                                   const NoteMatchCriteria& criteria) {
  const auto matches = [&](const EvalNote& reference, const EvalNote& estimate) {
    if (std::abs(estimate.onset_sec - reference.onset_sec) > criteria.onset_tolerance_sec) {
      return false;
    }
    const double cents = static_cast<double>(::sonare::constants::kCentsPerSemitone) *
                         std::abs(estimate.midi - reference.midi);
    if (cents > criteria.pitch_tolerance_cents) return false;
    if (!criteria.match_offset) return true;
    const double tolerance =
        std::max(criteria.offset_min_tolerance_sec,
                 criteria.offset_ratio * (reference.offset_sec - reference.onset_sec));
    return std::abs(estimate.offset_sec - reference.offset_sec) <= tolerance;
  };

  std::vector<int> estimate_to_truth(estimates.size(), -1);
  const auto augment = [&](auto&& self, size_t truth_index, std::vector<bool>& visited) -> bool {
    for (size_t estimate_index = 0; estimate_index < estimates.size(); ++estimate_index) {
      if (visited[estimate_index]) continue;
      if (!matches(truth[truth_index], estimates[estimate_index])) continue;
      visited[estimate_index] = true;
      if (estimate_to_truth[estimate_index] < 0 ||
          self(self, static_cast<size_t>(estimate_to_truth[estimate_index]), visited)) {
        estimate_to_truth[estimate_index] = static_cast<int>(truth_index);
        return true;
      }
    }
    return false;
  };

  NoteMatchCounts counts;
  for (size_t truth_index = 0; truth_index < truth.size(); ++truth_index) {
    std::vector<bool> visited(estimates.size(), false);
    if (augment(augment, truth_index, visited)) ++counts.true_positive;
  }
  counts.false_positive = estimates.size() - counts.true_positive;
  counts.false_negative = truth.size() - counts.true_positive;
  return counts;
}

}  // namespace sonare::test
