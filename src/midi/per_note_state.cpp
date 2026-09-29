#include "midi/per_note_state.h"

#include "util/constants.h"

namespace sonare::midi {

using sonare::constants::kCentsPerSemitone;

namespace {
constexpr uint32_t kBendCenter = 0x80000000u;
}  // namespace

double per_note_bend_units(Bend32 bend) noexcept {
  if (bend.raw >= kBendCenter) {
    return static_cast<double>(bend.raw - kBendCenter) /
           static_cast<double>(0xFFFFFFFFu - kBendCenter);
  }
  return -static_cast<double>(kBendCenter - bend.raw) / static_cast<double>(kBendCenter);
}

float channel_bend_cents(Bend32 bend, float range_cents) noexcept {
  return (bend.f14() - 8192.0f) / 8192.0f * range_cents;
}

ComposedPitch compose_note_pitch(const NotePitchRequest& req) noexcept {
  const double note = static_cast<double>(req.note);
  double base = note;
  bool absolute = false;
  if (req.has_attribute_pitch) {
    base = attribute_pitch_semitones(req.attribute_pitch_q7_9);
    absolute = true;
  } else if (req.per_note.pitch_7_25_set) {
    base = req.per_note.pitch_7_25.q7_25();
    absolute = true;
  }
  const double per_note_bend = req.per_note.bend_set ? per_note_bend_units(req.per_note.bend) *
                                                           req.per_note_bend_sensitivity.q7_25()
                                                     : 0.0;
  const double channel_bend =
      static_cast<double>(channel_bend_cents(req.channel_bend, req.channel_bend_range_cents)) /
      static_cast<double>(kCentsPerSemitone);

  ComposedPitch out;
  out.absolute = absolute;
  out.per_note_semitones = (base - note) + per_note_bend;
  out.pitch_semitones = base + req.coarse_tune_semitones + req.fine_tune_semitones + channel_bend +
                        per_note_bend + req.mpe_bend_semitones;
  return out;
}

}  // namespace sonare::midi
