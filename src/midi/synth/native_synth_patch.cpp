#include "midi/synth/native_synth.h"
#include "midi/synth/pitch.h"

namespace sonare::midi::synth {

float synth_note_to_hz(float note) noexcept { return note_to_hz(note); }

}  // namespace sonare::midi::synth
