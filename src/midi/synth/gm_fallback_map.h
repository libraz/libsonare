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

#include "midi/program_map.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/native_synth.h"

namespace sonare::midi::synth {

/// Rhythm-part bank: the SF2 convention for a drum kit, and the bank every
/// drum channel resolves to.
constexpr uint16_t kDrumBank = 128;

/// The GM/GS/GM2 bank a channel plays from, given its two bank-select bytes
/// and whether the part is a rhythm part (channel 10 by default, or assigned
/// by the GS "use for rhythm part" SysEx). This is the single source of truth
/// for bank resolution: every instrument resolves programs through it, so the
/// same MIDI stream selects the same timbre everywhere.
constexpr uint16_t gs_effective_bank(uint8_t bank_msb, uint8_t bank_lsb, bool drums) noexcept {
  if (drums || bank_msb == static_cast<uint8_t>(Gm2Bank::kPercussion)) return kDrumBank;
  if (bank_msb == static_cast<uint8_t>(Gm2Bank::kMelodic)) return bank_lsb;
  return bank_msb;
}

/// The GS tone map a channel's bank-select bytes select. GM2 addresses its
/// melodic and percussion banks through the MSB and gives the LSB a different
/// meaning there (a variation number, a percussion set), so the LSB is read as a
/// map select only when the MSB is neither of GM2's two bank numbers.
constexpr GsToneMap gs_effective_tone_map(uint8_t bank_msb, uint8_t bank_lsb) noexcept {
  if (bank_msb == static_cast<uint8_t>(Gm2Bank::kMelodic) ||
      bank_msb == static_cast<uint8_t>(Gm2Bank::kPercussion)) {
    return GsToneMap::kModuleDefault;
  }
  return gs_tone_map_from_lsb(bank_lsb);
}

/// Fallback patch for a melodic (bank, program) within @p map. Never fails —
/// a variation the map does not define, and an unknown program, both resolve
/// through the capital tone and its GM family.
const NativeSynthPatch& gm_fallback_patch(uint16_t bank, uint8_t program,
                                          GsToneMap map = GsToneMap::kModuleDefault) noexcept;

/// True when a synth engine is a dedicated physical / resonator model whose
/// data-free voice matches or exceeds a general-purpose SoundFont sample for
/// its instrument family (piano, plucked/bowed/reed/brass/flute waveguides,
/// modal mallets, membrane percussion, free reed). The signal-synthesis
/// engines (subtractive / FM / additive drawbar) and the source-filter vocal
/// engine are excluded: for those families a loaded SF2 sample stays the
/// reference, so they are not model-first candidates. This is the single
/// source of truth for that classification.
bool is_dedicated_model_engine(SynthEngineMode mode) noexcept;

/// True when the built-in GM fallback resolves this melodic (bank, program) to
/// a dedicated model engine (see is_dedicated_model_engine). Derived from
/// gm_fallback_patch so it cannot drift from the actual routing. Drums
/// (bank 128) are out of scope: they always play the percussion model via
/// gm_fallback_drum_patch on their own path.
///
/// This is a classification query, not a routing switch: the SF2-vs-model
/// decision is made where a voice is instantiated (a loaded SoundFont still
/// takes precedence when present). Use it to report or group the model-first
/// program set, not to gate playback.
bool gm_program_has_dedicated_model(uint16_t bank, uint8_t program) noexcept;

/// Fallback patch for a GM drum note (rhythm parts / bank 128). Always the
/// Standard kit; GS kit variations are applied per voice by apply_gs_drum_kit.
const NativeSynthPatch& gm_fallback_drum_patch(uint8_t note) noexcept;

/// GS drum-kit index for a rhythm-part program within @p map (mirrors
/// gs_drum_kit_name; the numbering is kGsDrumKits, which is the single source of
/// truth). A program the selected map defines no kit for maps to Standard,
/// which is what a module plays for a kit it does not have.
uint8_t gm_fallback_drum_kit(uint8_t program, GsToneMap map = GsToneMap::kModuleDefault) noexcept;

/// Applies a GS kit variation to a resolved Standard drum patch's percussion +
/// amp-envelope params (in place, note-aware — Power lowers the shells, TR-808
/// sine-ifies them, Brush swishes the snare, etc.) and returns a gain
/// multiplier. kit 0 (Standard) is a no-op returning 1.0. Applied per voice at
/// note-on so a single Standard drum table covers every kit (no per-kit table).
float apply_gs_drum_kit(PercussionPatchParams& perc, DahdsrConfig& amp, uint8_t kit,
                        uint8_t note) noexcept;

/// Per-program ambience weighting for fallback voices: multipliers on the
/// channel's CC91/CC93-derived sends (clamped to 1 after scaling). SF2 zones
/// carry their own send generators, so this weighting exists only for the
/// data-free fallback path — a cathedral organ carries more room than a
/// close-miked bass at the same controller value. CC 0 stays fully dry.
struct GmFallbackSends {
  float reverb_scale = 1.0f;
  float chorus_scale = 1.0f;
};

/// Ambience weighting for a melodic (bank, program); bank 128 resolves the
/// drum weighting. Never fails — unknown programs resolve through their
/// GM family.
GmFallbackSends gm_fallback_sends(uint16_t bank, uint8_t program) noexcept;

/// Longest amp-envelope release across all fallback patches (ms) — players
/// fold this into their tail accounting when the fallback is enabled.
float gm_fallback_max_release_ms() noexcept;

}  // namespace sonare::midi::synth
