#include "midi/assist/modules/placement_judge.h"

#include <string>

#include "midi/assist/modules/music_theory.h"

namespace sonare::midi::assist::modules {

namespace {

constexpr int kMaxMidiNote = 127;
constexpr int kOctave = 12;

std::string note_text(int note) { return std::to_string(note); }

/// Rounds a score to two decimals for the reason text, so a reason is stable
/// enough to assert on without pinning the scorer's exact arithmetic.
std::string score_text(float score) {
  const int hundredths = static_cast<int>(score * 100.0f + 0.5f);
  return std::to_string(hundredths / 100) + "." + (hundredths % 100 < 10 ? "0" : "") +
         std::to_string(hundredths % 100);
}

PlacementVerdict refuse(std::string reason) {
  PlacementVerdict verdict;
  verdict.accepted = false;
  verdict.reason = std::move(reason);
  return verdict;
}

}  // namespace

PlacementVerdict RangeScaleJudge::judge(const arrangement::ProjectView& view,
                                        const VoiceModel& voice,
                                        const CandidateNote& candidate) const {
  if (candidate.note > kMaxMidiNote) {
    return refuse("note " + note_text(candidate.note) + " is outside the MIDI range");
  }
  if (candidate.velocity == 0 || candidate.velocity > kMaxMidiNote) {
    return refuse("velocity " + note_text(candidate.velocity) + " is outside 1..127");
  }
  if (voice.low_note > voice.high_note) {
    return refuse("voice range " + note_text(voice.low_note) + ".." +
                  note_text(voice.high_note) + " is empty");
  }

  CandidateNote adjusted = candidate;
  std::string reason;

  // 1. Register. An octave fold is a correction, not a decision -- the line
  //    keeps its pitch class and only changes where it sits.
  int note = adjusted.note;
  while (note < voice.low_note && note + kOctave <= kMaxMidiNote) note += kOctave;
  while (note > voice.high_note && note - kOctave >= 0) note -= kOctave;
  if (note < voice.low_note || note > voice.high_note) {
    return refuse("no octave of note " + note_text(candidate.note) + " fits the voice range " +
                  note_text(voice.low_note) + ".." + note_text(voice.high_note));
  }
  if (note != adjusted.note) {
    reason = "folded " + note_text(candidate.note) + " to " + note_text(note) +
             " into the voice range " + note_text(voice.low_note) + ".." +
             note_text(voice.high_note);
    adjusted.note = static_cast<uint8_t>(note);
  }

  // 2. Key. Skipped entirely when no key is stated, so an unannotated project
  //    is not silently forced into C major.
  if (config_.snap_to_scale && harmony_ != nullptr) {
    const std::vector<uint8_t> scale = harmony_->scale_pitch_classes(view, adjusted.ppq);
    if (!scale.empty() && !theory::admits(scale, adjusted.note)) {
      const int snapped = theory::nearest_admitted(scale, adjusted.note);
      if (snapped < voice.low_note || snapped > voice.high_note) {
        return refuse("note " + note_text(adjusted.note) +
                      " is outside the key and its nearest in-key note leaves the voice range");
      }
      if (!reason.empty()) reason += "; ";
      reason += "snapped " + note_text(adjusted.note) + " to " + note_text(snapped) +
                " to stay in key";
      adjusted.note = static_cast<uint8_t>(snapped);
    }
  }

  // 3. Harmony. The one rule that refuses: after register and key have been
  //    corrected, a remaining clash is a choice the judge does not make.
  if (dissonance_ != nullptr) {
    const float score = dissonance_->score_note(view, adjusted);
    if (score > config_.max_dissonance) {
      return refuse("note " + note_text(adjusted.note) + " scores " + score_text(score) +
                    " against the chord, over the " + score_text(config_.max_dissonance) +
                    " limit");
    }
    if (!reason.empty()) reason += "; ";
    reason += "accepted at dissonance " + score_text(score);
  } else if (reason.empty()) {
    reason = "accepted unchanged";
  }

  PlacementVerdict verdict;
  verdict.accepted = true;
  verdict.adjusted = adjusted;
  verdict.reason = std::move(reason);
  return verdict;
}

}  // namespace sonare::midi::assist::modules
