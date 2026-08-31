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
///
/// What belongs in a patch and what belongs after it is docs/voicing.md: the
/// voice is the instrument, and a cabinet, an amplifier or a room is a stage
/// the bank binds by default rather than something baked into the patch. The
/// send weights here are the other kind — they multiply a send the file
/// controls, so CC 0 stays dry, which is the test that separates the two.

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

/// The rig a program is heard through by default (docs/voicing.md): the stage
/// after the instrument, which the bank supplies and the host may replace or
/// clear. An electric guitar's voice is the string and the pickup; the amplifier
/// and its cabinet are this.
///
/// `preset` is an amp-preset identifier (mastering::saturation::amp_preset_names)
/// or empty, which is what most programs return — an instrument whose whole
/// sound is its own voice has no rig, and that is the default rather than a gap.
/// The two numbers ride on top of the named preset: `drive` is how hard the amp
/// is pushed and `level_db` the trim that keeps a rigged program at the same
/// level as its unrigged siblings.
struct GmFallbackRig {
  /// Distinguishes one binding from another without comparing strings, so a part
  /// that changes program WITHIN a rig keeps the amplifier it is already
  /// running instead of rebuilding an identical one. 0 is no rig.
  uint8_t id = 0;
  const char* preset = "";
  float drive = 0.5f;
  float level_db = 0.0f;
};

/// Default rig for a melodic (bank, program). Never fails; bank 128 and every
/// program with no binding return `id` 0 and an empty `preset`.
///
/// This is a lookup on bank data, not a routing decision: whether the rig is
/// actually built depends on the part carrying no insert of its own and on the
/// voice being the model's rather than a SoundFont's, which is Sf2Player's to
/// judge. A sampled electric guitar already has an amplifier inside it.
GmFallbackRig gm_fallback_rig(uint16_t bank, uint8_t program) noexcept;

/// The binding an id names, for a caller that carried the id rather than the
/// program — a part publishes which rig it wants as one small value across the
/// thread boundary, and the builder resolves it back here. Ids come from
/// gm_fallback_rig; an unknown one is no rig.
GmFallbackRig gm_rig_binding(uint8_t id) noexcept;

/// Longest amp-envelope release across all fallback patches (ms) — players
/// fold this into their tail accounting when the fallback is enabled.
float gm_fallback_max_release_ms() noexcept;

}  // namespace sonare::midi::synth
