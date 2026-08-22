#include <cmath>

#include "midi/synth/native_synth.h"
#include "util/constants.h"

namespace sonare::midi::synth {

float synth_note_to_hz(float note) noexcept {
  return constants::kA4Hz * std::exp2((note - constants::kMidiA4) / constants::kSemitonesPerOctave);
}

}  // namespace sonare::midi::synth
