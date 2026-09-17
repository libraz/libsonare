#include "midi/assist/modules/dissonance_analyzer.h"

#include <algorithm>

#include "midi/assist/modules/music_theory.h"

namespace sonare::midi::assist::modules {

float IntervalDissonanceAnalyzer::score_note(const arrangement::ProjectView& view,
                                             const CandidateNote& candidate) const {
  if (harmony_ == nullptr) return 0.0f;
  const arrangement::ChordSymbol chord = harmony_->chord_at(view, candidate.ppq);
  const std::vector<uint8_t> tones = theory::chord_pitch_classes(chord);
  if (tones.empty()) return 0.0f;
  if (theory::admits(tones, candidate.note)) return 0.0f;

  // The worst clash the note makes, not the mildest: a note that is a ninth
  // above one chord tone and a semitone under another is heard as the semitone.
  float worst = 0.0f;
  for (const uint8_t tone : tones) {
    const float roughness =
        theory::interval_class_roughness(theory::interval_class(candidate.note, tone));
    worst = std::max(worst, roughness);
  }
  return worst;
}

float IntervalDissonanceAnalyzer::score_vertical(const arrangement::ProjectView& view, double ppq,
                                                 const std::vector<CandidateNote>& notes) const {
  (void)view;
  (void)ppq;
  if (notes.size() < 2) return 0.0f;
  float worst = 0.0f;
  for (size_t i = 0; i + 1 < notes.size(); ++i) {
    for (size_t j = i + 1; j < notes.size(); ++j) {
      const float roughness =
          theory::interval_class_roughness(theory::interval_class(notes[i].note, notes[j].note));
      worst = std::max(worst, roughness);
    }
  }
  return worst;
}

}  // namespace sonare::midi::assist::modules
