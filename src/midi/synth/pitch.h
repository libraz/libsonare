#pragma once

/// @file pitch.h
/// @brief Shared equal-temperament note-to-frequency conversion for synth voices.

#include <cmath>
#include <cstdint>

#include "util/constants.h"

namespace sonare::midi::synth {

/// @brief Convert a (possibly fractional) MIDI note number to frequency in Hz.
/// @details Equal temperament, A4 (note 69) = 440 Hz. Uses `std::exp2` to match
///   the physical-model voices' historical arithmetic bit-for-bit; this is the
///   single source of truth for the voice pitch mapping and is deliberately not
///   routed through @ref sonare::midi_to_hz (which uses `std::pow`).
inline float note_to_hz(float note) noexcept {
  return constants::kA4Hz * std::exp2((note - constants::kMidiA4) / constants::kSemitonesPerOctave);
}

/// @brief MIDI note-number overload; masks the note to its low 7 bits first.
inline float note_to_hz(uint8_t note) noexcept {
  return note_to_hz(static_cast<float>(note & 0x7Fu));
}

}  // namespace sonare::midi::synth
