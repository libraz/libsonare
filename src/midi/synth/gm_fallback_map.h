#pragma once

/// @file gm_fallback_map.h
/// @brief GM program / drum note -> NativeSynth fallback patch — the
///        data-free floor. When no SoundFont is loaded (or the loaded one
///        does not cover a program) Sf2Player resolves the note through this
///        table instead of dropping it, so every GM program and GM drum note
///        stays audible with zero data.
///
/// Patches are grouped by GM family (16 families x 8 programs) with the
/// subtractive engine of this phase; later phases swap families to their
/// dedicated modes (FM e-pianos, KS guitars, modal mallets, ...) by changing
/// table entries only. The returned references point at static const data —
/// safe to keep in a voice for its whole life and on the audio thread.

#include <cstdint>

#include "midi/synth/native_synth.h"

namespace sonare::midi::synth {

/// Fallback patch for a melodic (bank, program). Never fails — unknown
/// programs resolve through their GM family.
const NativeSynthPatch& gm_fallback_patch(uint16_t bank, uint8_t program) noexcept;

/// Fallback patch for a GM drum note (rhythm parts / bank 128).
const NativeSynthPatch& gm_fallback_drum_patch(uint8_t note) noexcept;

/// Longest amp-envelope release across all fallback patches (ms) — players
/// fold this into their tail accounting when the fallback is enabled.
float gm_fallback_max_release_ms() noexcept;

}  // namespace sonare::midi::synth
