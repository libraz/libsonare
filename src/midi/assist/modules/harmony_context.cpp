#include "midi/assist/modules/harmony_context.h"

#include "midi/assist/modules/music_theory.h"

namespace sonare::midi::assist::modules {

arrangement::ChordSymbol TimelineHarmonyContext::chord_at(const arrangement::ProjectView& view,
                                                          double ppq) const {
  if (const arrangement::ChordSymbol* chord = view.harmony().chord_at(ppq)) return *chord;
  return {};
}

arrangement::KeySegment TimelineHarmonyContext::key_at(const arrangement::ProjectView& view,
                                                       double ppq) const {
  if (const arrangement::KeySegment* key = view.harmony().key_at(ppq)) return *key;
  return {};
}

std::vector<uint8_t> TimelineHarmonyContext::scale_pitch_classes(
    const arrangement::ProjectView& view, double ppq) const {
  const arrangement::KeySegment key = key_at(view, ppq);
  return theory::scale_pitch_classes(key.tonic_pc, key.mode);
}

}  // namespace sonare::midi::assist::modules
