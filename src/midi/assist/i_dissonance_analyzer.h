#pragma once

/// @file i_dissonance_analyzer.h
/// @brief assist seam: dissonance-scoring interface (frozen abstract).
///
/// An IDissonanceAnalyzer scores how dissonant a candidate note (or a vertical
/// set of notes) is against the prevailing harmony at a PPQ. A generator /
/// placement judge consults it to prefer consonant choices. Same-build C++
/// abstract class. Control/offline thread only.

#include <cstdint>
#include <vector>

#include "arrangement/project_view.h"

namespace sonare::midi::assist {

/// @brief A candidate pitch placed at a musical position (for scoring).
struct CandidateNote {
  double ppq = 0.0;         ///< Onset position (PPQ).
  double length_ppq = 0.0;  ///< Duration (PPQ); 0 = point/grace.
  uint8_t note = 60;        ///< MIDI note number 0..127.
  uint8_t velocity = 96;    ///< MIDI velocity 1..127.
};

/// @brief Read-only dissonance oracle.
class IDissonanceAnalyzer {
 public:
  virtual ~IDissonanceAnalyzer() = default;

  /// @brief Scores a single candidate note against the harmony at its position.
  /// @return dissonance in [0, 1] (0 = fully consonant, 1 = maximally dissonant).
  virtual float score_note(const arrangement::ProjectView& view,
                           const CandidateNote& candidate) const = 0;

  /// @brief Scores a vertical set of simultaneous candidate notes (a chord/voice
  ///        stack) at `ppq`. @return dissonance in [0, 1].
  virtual float score_vertical(const arrangement::ProjectView& view, double ppq,
                               const std::vector<CandidateNote>& notes) const = 0;
};

}  // namespace sonare::midi::assist
