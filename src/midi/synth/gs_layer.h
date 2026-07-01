#pragma once

/// @file gs_layer.h
/// @brief GS architecture layer for the SF2 player: NRPN part parameters
///        (TVF cutoff/resonance, TVA envelope, vibrato), per-note drum-kit
///        NRPNs, and the GS/GM SysEx surface (GM System On, GS Reset,
///        "use for rhythm part").
///
/// Most of GS comes from the SoundFont itself (variation banks and bank-128
/// drum kits are SF2 (bank, preset) addresses); this layer adds what the SF2
/// modulator model does not carry: GS NRPN part edits applied as RELATIVE
/// offsets on top of the resolved SoundFont generators, and the GS reset /
/// rhythm-part protocol plumbing.
///
/// Scaling note: Roland documents the SC-88 ranges (e.g. TVF cutoff
/// +-9600 cents over the 64-step NRPN range) but not exact per-step curves;
/// the constants here are documented approximations chosen to match the
/// documented end-to-end ranges. Tests assert direction and monotonicity,
/// not absolute Roland-hardware values.
///
/// RT contract: everything here is POD + pure functions — usable from the
/// audio thread without allocation.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "midi/synth/sf2_voice.h"

namespace sonare::midi::synth {

/// GS NRPN part parameters, stored as signed offsets from centre (data - 64).
/// All-zero means "no edit" (the SoundFont patch plays unmodified).
struct GsPartParams {
  int8_t vibrato_rate = 0;   // NRPN 01 08
  int8_t vibrato_depth = 0;  // NRPN 01 09
  int8_t vibrato_delay = 0;  // NRPN 01 0A
  int8_t tvf_cutoff = 0;     // NRPN 01 20
  int8_t tvf_resonance = 0;  // NRPN 01 21
  int8_t eg_attack = 0;      // NRPN 01 63
  int8_t eg_decay = 0;       // NRPN 01 64
  int8_t eg_release = 0;     // NRPN 01 66

  bool any() const noexcept {
    return vibrato_rate != 0 || vibrato_depth != 0 || vibrato_delay != 0 || tvf_cutoff != 0 ||
           tvf_resonance != 0 || eg_attack != 0 || eg_decay != 0 || eg_release != 0;
  }
};

/// Per-note drum overrides (GS NRPN msb 18/1A/1C/1D/1E, lsb = drum note).
struct GsDrumNoteParams {
  enum Flag : uint8_t {
    kPitch = 1u << 0,
    kLevel = 1u << 1,
    kPan = 1u << 2,
    kReverb = 1u << 3,
    kChorus = 1u << 4,
  };
  uint8_t flags = 0;
  int8_t pitch_coarse = 0;  // semitones (data - 64)
  uint8_t level = 127;      // absolute TVA level (data)
  uint8_t pan = 64;         // absolute pan (data; 64 = centre)
  uint8_t reverb = 0;       // absolute per-note reverb send (data)
  uint8_t chorus = 0;       // absolute per-note chorus send (data)

  bool any() const noexcept { return flags != 0; }
};

/// GS insertion effect (EFX) state, stored as the RAW GS wire so any adapter
/// can interpret it without a typed per-effect struct. The SC-55/88 EFX is a
/// single insertion unit whose type is a 14-bit number (two 7-bit SysEx bytes)
/// and whose 20 parameters are raw 0..127 bytes; each adapter reads only the
/// parameters it uses. Keeping the wire raw means a new EFX algorithm is a new
/// adapter over the same bytes — no parser, struct or ABI change.
struct GsEfx {
  /// EFX type number: (MSB << 8) | LSB, matching the two-byte GS notation
  /// (e.g. 0x0110). 0 = the power-on default (Thru / no insertion effect).
  uint16_t type = 0;
  /// EFX PARAMETER 1..20 (GS address 40 03 03..16), raw 0..127.
  std::array<uint8_t, 20> params{};
  uint8_t send_reverb = 0;  ///< EFX -> reverb send (40 03 17).
  uint8_t send_chorus = 0;  ///< EFX -> chorus send (40 03 18).
  uint8_t send_delay = 0;   ///< EFX -> delay send (40 03 19).
  /// True once any EFX-block write has arrived (so an all-zero Thru that was
  /// explicitly set is distinguished from the never-touched power-on state).
  bool assigned = false;

  bool any() const noexcept { return assigned; }
};

/// Applies a GS DT1 write to the EFX block (address 40 03 xx) onto @p efx,
/// handling a run of consecutive data bytes from the start address (a single
/// parameter write or a full-block dump). Bytes addressing reserved/unknown
/// offsets are preserved by being ignored, never dropping the message. Accepts
/// the payload with or without F0/F7 framing. Returns true if the message
/// addressed the EFX block (even if some bytes were ignored); false otherwise.
/// Never crashes.
bool apply_gs_efx_sysex(GsEfx& efx, const uint8_t* data, size_t size) noexcept;

/// Insertion-effect adapter name for a GS EFX @p type: the `insert_factory`
/// processor name an adapter drives, or an empty view for a type this layer
/// does not yet map (the caller bypasses it and logs — no silent drop). The
/// mapping is intentionally partial; layer-3 promotion adds entries here (and
/// the matching DSP) without touching the parser, ABI or bindings.
std::string_view gs_efx_insert_name(uint16_t type) noexcept;

/// JSON param object (for `insert_factory` / make_insert) translating the raw
/// GS EFX parameters of @p efx into the mapped insert's parameters. The
/// Overdrive/Distortion families translate EFX PARAMETER 2 as the drive amount
/// and PARAMETER 20 as the output level (the SC-88Pro OD/Dist parameter map;
/// PARAMETER 1 is the OD/Dist selector, redundant with the EFX type). The basic
/// OD/Dist has no tone/EQ parameters — the tone comes from the amp voicing. Every
/// other type (mapped or not) returns "{}" so the insert plays its own defaults
/// until a per-type translation lands (a layer-3 refinement — the type is
/// already honoured, only its parameter voicing is approximate).
std::string gs_efx_insert_params(const GsEfx& efx);

/// One stage of a realised EFX chain: an `insert_factory` processor name and
/// its JSON params.
struct GsEfxStage {
  std::string name;         ///< insert-factory processor name.
  std::string params_json;  ///< JSON params for make_insert ("{}" = defaults).
};

/// The ordered insert chain that realises @p efx, in signal-flow order. A
/// single-effect type yields a one-stage chain (the `gs_efx_insert_name` /
/// `gs_efx_insert_params` mapping); a composite/multi type (e.g. SC-88Pro GTR
/// Multi = Cmp-OD-EQ-CF) yields its block chain so a whole guitar rig — with a
/// real tone/EQ stage — realises from one EFX unit. An empty vector means the
/// type is unmapped (bypass + log). Stages whose factory build returns null
/// (e.g. an FX-suite stage in a no-FX build) are skipped at realise time, so a
/// partial chain still runs. The block STRUCTURE of the composite types is
/// faithful to the hardware; per-block parameter voicing is translated where
/// the parameter positions are confirmed (the EQ Low/Hi Gain) and left at the
/// insert defaults otherwise.
std::vector<GsEfxStage> gs_efx_insert_chain(const GsEfx& efx);

// --- NRPN offset scalings (documented approximations, see file header) ---

/// TVF cutoff: ~150 cents per step (+-9600 over the full range).
float gs_cutoff_offset_cents(int8_t offset) noexcept;
/// TVF resonance: ~3 cB per step -> linear Q multiplier.
float gs_resonance_gain(int8_t offset) noexcept;
/// TVA envelope time: ~75 timecents per step -> time multiplier.
float gs_time_scale(int8_t offset) noexcept;
/// Vibrato rate: ~25 cents of LFO frequency per step -> frequency multiplier.
float gs_vib_rate_scale(int8_t offset) noexcept;
/// Vibrato depth: ~3 cents of added pitch depth per step.
float gs_vib_depth_cents(int8_t offset) noexcept;

/// Applies the melodic part offsets onto resolved voice parameters.
void apply_gs_part_params(Sf2VoiceParams& params, const GsPartParams& gs) noexcept;

/// Applies the per-note drum overrides onto resolved voice parameters.
/// The reverb/chorus sends are additive contributions in [0, 0.2] (the same
/// depth scale as the CC send default modulators).
void apply_gs_drum_params(Sf2VoiceParams& params, const GsDrumNoteParams& drum) noexcept;

// --- SysEx surface ---

enum class GsSysExKind : uint8_t {
  kNone = 0,
  kGmReset,        ///< GM System On (F0 7E 7F 09 01 F7)
  kGsReset,        ///< GS Reset (F0 41 dd 42 12 40 00 7F 00 41 F7)
  kUseForRhythm,   ///< GS part rhythm assignment (40 1x 15 mm)
  kEfxPartSwitch,  ///< GS per-part EFX on/off (40 4x 22 mm): routes the part
                   ///< through the single insertion effect (value 1 = on).
};

struct GsSysEx {
  GsSysExKind kind = GsSysExKind::kNone;
  /// kUseForRhythm: zero-based target channel index.
  uint8_t channel = 0;
  /// kUseForRhythm: 0 = melodic, 1/2 = drum map 1/2.
  uint8_t value = 0;
};

/// Recognises the GS/GM SysEx messages this layer implements. Accepts the
/// payload with or without the surrounding F0/F7 framing bytes. Unknown or
/// malformed messages return kind == kNone (never crash).
GsSysEx parse_gs_sysex(const uint8_t* data, size_t size) noexcept;

/// GS drum-kit name for a bank-128 program number (SC-55/88 kit numbering:
/// 0 Standard, 8 Room, 16 Power, 24 Electronic, 25 TR-808, 32 Jazz, 40 Brush,
/// 48 Orchestra, 56 SFX). Unknown programs return an empty view.
std::string_view gs_drum_kit_name(uint8_t program) noexcept;

}  // namespace sonare::midi::synth
