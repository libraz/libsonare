#pragma once

/// @file i_note_placement_judge.h
/// @brief assist seam: note-placement accept/reject interface (frozen).
///
/// An INotePlacementJudge decides whether a candidate note is acceptable in
/// context (range, dissonance, voice-leading, density). A generator proposes
/// candidates and a judge filters them. Same-build C++ abstract class.
/// Control/offline thread only.

#include "arrangement/project_view.h"
#include "midi/assist/assist_request.h"
#include "midi/assist/i_dissonance_analyzer.h"

namespace sonare::midi::assist {

/// @brief Outcome of judging a candidate note.
struct PlacementVerdict {
  bool accepted = false;
  /// Optional adjusted note (e.g. snapped to a scale tone). Valid when accepted.
  CandidateNote adjusted;
  /// Optional human-readable rejection / adjustment reason (owned).
  std::string reason;
};

/// @brief Accept / reject (and optionally adjust) candidate note placements.
class INotePlacementJudge {
 public:
  virtual ~INotePlacementJudge() = default;

  /// @brief Judges `candidate` in the context of `view` for the active voice.
  /// @param voice the voice the candidate belongs to (range / role constraints).
  virtual PlacementVerdict judge(const arrangement::ProjectView& view, const VoiceModel& voice,
                                 const CandidateNote& candidate) const = 0;
};

}  // namespace sonare::midi::assist
