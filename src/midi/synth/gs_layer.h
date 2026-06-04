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

#include <cstddef>
#include <cstdint>
#include <string_view>

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
  kGmReset,       ///< GM System On (F0 7E 7F 09 01 F7)
  kGsReset,       ///< GS Reset (F0 41 dd 42 12 40 00 7F 00 41 F7)
  kUseForRhythm,  ///< GS part rhythm assignment (40 1x 15 mm)
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
