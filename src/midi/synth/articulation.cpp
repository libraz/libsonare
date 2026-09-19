#include "midi/synth/articulation.h"

#include "midi/synth/native_synth.h"

namespace sonare::midi::synth {

bool accepts_legato(SynthEngineMode mode, uint8_t from_note, uint8_t to_note,
                    float lowest_pitch_mult) noexcept {
  const EngineLegato row = engine_legato(mode);
  if (!row.continues) return false;
  if (row.lowest_hz <= 0.0f) return true;
  // Upward is unconditional: it shortens the delay, and a line already holding
  // the note it started on holds anything above it.
  if (to_note >= from_note) return true;
  const float mult = lowest_pitch_mult > 0.0f ? lowest_pitch_mult : 1.0f;
  return synth_note_to_hz(static_cast<float>(to_note)) * mult >= row.lowest_hz;
}

}  // namespace sonare::midi::synth
