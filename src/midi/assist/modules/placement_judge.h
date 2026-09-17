#pragma once

/// @file placement_judge.h
/// @brief Built-in INotePlacementJudge: voice range, scale membership, then
///        dissonance -- in that order, with the reason it acted on.
///
/// Two of the three rules ADJUST and one REFUSES, and which is which is the
/// whole design. A note outside the voice's range is a register mistake, so it
/// is folded by octaves and kept; a note outside the key is usually a passing
/// spelling, so it is moved to the nearest note the key admits. A note that
/// still clashes after both is a harmonic decision the judge will not make on
/// the caller's behalf, so it is refused and the reason names the score.
///
/// Every verdict carries a human-readable reason naming the rule that acted and
/// the values it acted on. An accepted note that was never touched says so too,
/// so a caller reading the reasons can tell "passed" from "passed after being
/// moved a minor third".
///
/// Control/offline thread only. Stateless and deterministic.

#include "arrangement/project_view.h"
#include "midi/assist/i_dissonance_analyzer.h"
#include "midi/assist/i_harmony_context.h"
#include "midi/assist/i_note_placement_judge.h"

namespace sonare::midi::assist::modules {

struct RangeScaleJudgeConfig {
  /// A note scoring strictly above this after every adjustment is refused.
  /// 0.7 sits between the second/ninth at 0.6 and the tritone at 0.8, so sixths
  /// and ninths pass while the avoid notes -- a semitone off a chord tone, or a
  /// tritone -- do not. Deliberately not 0.6: that is a scored value, and a
  /// limit equal to one leans on exact float equality to admit it.
  float max_dissonance = 0.7f;
  /// Move an out-of-key note to the nearest one the key admits. Off leaves the
  /// note where it is and lets the dissonance rule decide.
  bool snap_to_scale = true;
};

class RangeScaleJudge final : public INotePlacementJudge {
 public:
  /// @param harmony Non-owning; NULL states no key, so the scale rule is skipped.
  /// @param dissonance Non-owning; NULL states no score, so nothing is refused
  ///        for clashing.
  RangeScaleJudge(const IHarmonyContext* harmony, const IDissonanceAnalyzer* dissonance,
                  RangeScaleJudgeConfig config = {}) noexcept
      : harmony_(harmony), dissonance_(dissonance), config_(config) {}

  PlacementVerdict judge(const arrangement::ProjectView& view, const VoiceModel& voice,
                         const CandidateNote& candidate) const override;

 private:
  const IHarmonyContext* harmony_ = nullptr;
  const IDissonanceAnalyzer* dissonance_ = nullptr;
  RangeScaleJudgeConfig config_{};
};

}  // namespace sonare::midi::assist::modules
