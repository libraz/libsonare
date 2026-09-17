#pragma once

/// @file dissonance_analyzer.h
/// @brief Built-in IDissonanceAnalyzer: interval-class roughness against the
///        chord a note lands on.
///
/// A ranking, not a measurement. It scores a note by the worst interval it makes
/// with the chord sounding under it, so the number orders candidates against
/// each other and carries no acoustic claim -- there is no spectrum here, no
/// partials and no critical band.
///
/// A note that IS a chord tone scores 0, and a passage with no chord annotated
/// scores 0 throughout: with nothing stated to clash against, the analyzer
/// declines to invent a clash.
///
/// Control/offline thread only. Stateless and deterministic.

#include <vector>

#include "arrangement/project_view.h"
#include "midi/assist/i_dissonance_analyzer.h"
#include "midi/assist/i_harmony_context.h"

namespace sonare::midi::assist::modules {

class IntervalDissonanceAnalyzer final : public IDissonanceAnalyzer {
 public:
  /// @param harmony Non-owning; must outlive this analyzer. NULL scores
  ///        everything 0, which is what "no harmonic context installed" means.
  explicit IntervalDissonanceAnalyzer(const IHarmonyContext* harmony) noexcept
      : harmony_(harmony) {}

  /// @brief Roughness of @p candidate against the chord at its own PPQ, in
  ///        [0, 1]. 0 for a chord tone and for an unannotated passage.
  float score_note(const arrangement::ProjectView& view,
                   const CandidateNote& candidate) const override;

  /// @brief Roughness of a simultaneity: the worst interval any pair of
  ///        @p notes makes. 0 for fewer than two notes.
  /// @param ppq Unused -- the score is a property of the pitches, not of where
  ///        they sit -- and kept because the seam's signature carries it.
  float score_vertical(const arrangement::ProjectView& view, double ppq,
                       const std::vector<CandidateNote>& notes) const override;

 private:
  const IHarmonyContext* harmony_ = nullptr;
};

}  // namespace sonare::midi::assist::modules
