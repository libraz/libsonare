#pragma once

/// @file progression_patterns.h
/// @brief Shared chord progression pattern helpers.

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace sonare {

struct ProgressionPattern {
  std::string name;
  /// @brief (semitones above the key root, quality) pairs. The first element is
  ///        a semitone offset and not a scale degree, so the subdominant is 5
  ///        rather than 4. Quality follows @c diatonic_triads: 0 major, 1 minor,
  ///        2 diminished.
  std::vector<std::pair<int, int>> chords;
};

/// @brief Common pop/J-pop/blues progression templates as scale-degree chords.
const std::vector<ProgressionPattern>& known_progression_patterns();

/// @brief Diatonic triads for major or minor keys as (relative root, quality).
std::array<std::pair<int, int>, 7> diatonic_triads(bool minor_key);

/// @brief Soft similarity score between a detected chord and an expected chord.
float chord_similarity_score(int root, int quality, int expected_root, int expected_quality);

}  // namespace sonare
