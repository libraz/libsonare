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
///
/// The compatibility contract this implements is docs/gs.md: the target
/// device, the level every address carries, the parameters reachable from
/// more than one direction, and the extensions on top. It is a specification
/// rather than a guide.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "midi/synth/sf2_voice.h"

namespace sonare::midi::synth {

/// The GS tone map a part addresses, selected by Bank Select LSB (CC#32) on an
/// SC-88Pro-class module. The map picks WHICH generation's tone set a variation
/// number reaches; the variation number itself always lives in Bank Select MSB
/// (CC#0), and the capital tone stays the Program Change. SC-55 and SC-88
/// modules predate the map select and always sound their own map, which is why
/// an unset CC#32 means "the map this module is configured for" rather than any
/// particular generation.
enum class GsToneMap : uint8_t {
  kModuleDefault = 0,  ///< CC#32 = 0: whichever map the module is set to.
  kSc55 = 1,           ///< CC#32 = 1: the SC-55 tone map.
  kSc88 = 2,           ///< CC#32 = 2: the SC-88 tone map.
  kSc88Pro = 3,        ///< CC#32 = 3: the SC-88Pro tone map.
};

/// The tone map a Bank Select LSB value selects. Values outside the defined
/// maps read as kModuleDefault: a module that never saw the message is already
/// playing its default map, so an unrecognised map must sound that rather than
/// nothing.
///
/// This decodes CC#32 only for a GS part. GM2 puts its melodic variation number
/// in CC#32 instead, and is distinguished by its bank MSB (0x79) before the LSB
/// is ever read — see gs_effective_bank in gm_fallback_map.h.
constexpr GsToneMap gs_tone_map_from_lsb(uint8_t bank_lsb) noexcept {
  switch (bank_lsb & 0x7Fu) {
    case 1:
      return GsToneMap::kSc55;
    case 2:
      return GsToneMap::kSc88;
    case 3:
      return GsToneMap::kSc88Pro;
    default:
      return GsToneMap::kModuleDefault;
  }
}

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

/// The eight part edits reduced to quantities a voice can apply directly, so
/// both voice banks share one reading of the offsets rather than each making
/// its own. Defaults are no-ops.
struct GsPartMod {
  float cutoff_cents = 0.0f;     ///< added to the voice's filter cutoff offset.
  float resonance_gain = 1.0f;   ///< multiplies filter Q (floored at 0.5 by the caller).
  float attack_scale = 1.0f;     ///< multiplies the amplitude envelope's attack.
  float decay_scale = 1.0f;      ///< ... its decay.
  float release_scale = 1.0f;    ///< ... its release.
  float vib_rate_scale = 1.0f;   ///< multiplies the vibrato LFO frequency.
  float vib_depth_cents = 0.0f;  ///< added to the LFO's pitch depth (floored at 0).
  float vib_delay_scale = 1.0f;  ///< feeds gs_vib_delay_seconds, which is not a plain scale.

  /// True when the filter stage has been edited, which engages it: the manual
  /// gives no way to ask for a filter and then not hear it.
  bool filter_edited = false;
};

/// @p gs as voice-applicable quantities.
GsPartMod gs_part_mod(const GsPartParams& gs) noexcept;

/// The LFO onset delay after a vibrato-delay edit of @p scale on a voice whose
/// own delay is @p base_s. Not a plain multiply: a base of zero would stay zero,
/// so a positive edit gives it an onset instead of nothing.
float gs_vib_delay_seconds(float base_s, float scale) noexcept;

/// The GS system parameters at 40 00 xx that are not the effect block. Every
/// field holds its GS power-on value, so a default-constructed instance is the
/// reset state and a render that never saw one of these writes is untouched.
struct GsMasterParams {
  /// MASTER TUNE (40 00 00-03) as its four nibbles; 00 04 00 00 is 0 cents.
  std::array<uint8_t, 4> tune{{0x00, 0x04, 0x00, 0x00}};
  uint8_t volume = 0x7F;     ///< MASTER VOLUME (40 00 04).
  uint8_t key_shift = 0x40;  ///< MASTER KEY-SHIFT (40 00 05), 28-58 semitones.
  uint8_t pan = 0x40;        ///< MASTER PAN (40 00 06).
};

/// MASTER TUNE as a pitch offset in cents. The four nibbles make one 0018-07E8
/// word centred on 0400, in 0.1-cent steps, so the range is -100 to +100.
float gs_master_tune_cents(const GsMasterParams& master) noexcept;

/// MASTER VOLUME as a linear gain, on the same square law CC7, velocity and the
/// drum-note level already use.
float gs_master_volume_gain(uint8_t value) noexcept;

/// A key-shift byte as a pitch offset in cents: 28-58 around 40, one semitone
/// per step. Shared by MASTER KEY-SHIFT and the part's own PITCH KEY SHIFT,
/// which the manual gives the same range and the same centre.
float gs_key_shift_cents(uint8_t value) noexcept;

/// MASTER PAN as the two output-leg gains. It is a balance on the finished mix
/// rather than a re-pan — the legs already carry each part's own position — so
/// the centre 40 leaves both at 1 and a hard side silences the other.
void gs_master_pan_gains(uint8_t value, float* left, float* right) noexcept;

/// ASSIGN MODE (GS address 40 1x 14) SINGLE. 01 LIMITED-MULTI and 02 FULL-MULTI
/// have no constant because they are one behaviour here (docs/gs.md).
inline constexpr uint8_t kGsAssignModeSingle = 0;

/// MONO/POLY MODE (GS address 40 1x 13) Mono; 01 is Poly and is the default.
inline constexpr uint8_t kGsMonoPolyMono = 0;
inline constexpr uint8_t kGsMonoPolyPoly = 1;

/// USE FOR RHYTHM PART (GS address 40 1x 15): 00 melodic, 01/02 drum map 1/2.
/// The drum setup address 41 mn rr addresses a map and not a part, so per-note
/// drum edits are stored per map and two parts on one map share them.
inline constexpr uint8_t kGsDrumMapNone = 0;
inline constexpr uint8_t kGsDrumMap1 = 1;
inline constexpr uint8_t kGsDrumMapCount = 2;

/// Number of TONE MODIFY parameters (GS address 40 1x 30-37), which is also the
/// number of GsPartParams fields: the two are the same set reached from two
/// directions, so a field added to one without the other is a defect.
inline constexpr uint8_t kGsToneModifyCount = 8;

/// TONE MODIFY @p index (0-7, GS address 40 1x 30-37) onto the part parameter it
/// shares with its NRPN — the single storage location the alias table promises
/// (docs/gs.md). @p value is the raw 0-127 byte, centred on 64. An index past
/// the block is ignored.
void gs_apply_tone_modify(GsPartParams& gs, uint8_t index, uint8_t value) noexcept;

/// Per-note drum overrides (GS NRPN msb 18/1A/1C/1D/1E/1F with the drum note as
/// the lsb, and the drum setup addresses 41 m2/m4/m5/m6/m9 rr they alias).
///
/// Every field holds the value at which the parameter changes nothing. The
/// manual gives these no power-on value because a drum set change re-initialises
/// them to what the kit itself specifies, so an unwritten parameter has to mean
/// "the kit's" — which is also why each field is read only behind its flag.
struct GsDrumNoteParams {
  enum Flag : uint8_t {
    kPitch = 1u << 0,
    kLevel = 1u << 1,
    kPan = 1u << 2,
    kReverb = 1u << 3,
    kChorus = 1u << 4,
    kDelay = 1u << 5,
  };
  uint8_t flags = 0;
  int8_t pitch_coarse = 0;  // semitones (data - 64)
  uint8_t level = 127;      // absolute TVA level (data)
  uint8_t pan = 64;         // absolute pan (data; 64 = centre)
  uint8_t reverb = 127;     // reverb-send multiplicand (data)
  uint8_t chorus = 127;     // chorus-send multiplicand (data)
  uint8_t delay = 127;      // delay-send multiplicand (data)

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
  /// EFX -> reverb send (40 03 17). Its reset default is 40, not 0: the address
  /// table's row carries the same value, and a file that selects a type without
  /// writing the sends is entitled to it (docs/gs.md, reset defaults).
  uint8_t send_reverb = 40;
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
/// the payload with or without F0/F7 framing. Never crashes.
///
/// Returns true when at least one byte reached a GsEfx field. A write landing
/// entirely on the block's IGNORE rows — the two control-source assignments and
/// the send EQ switch (docs/gs.md) — addresses the block and still returns
/// false, because nothing was applied and there is nothing to rebuild for.
///
/// @param out_type_changed  Optional out-flag: set to true when the write
///   changed the EFX TYPE (address 40 03 00/01), false when it touched only
///   parameter/send bytes. A parameter/send-only change lets the caller update
///   the already-built insert processors in place (preserving their DSP state)
///   instead of rebuilding the whole chain, while a type change restructures it.
bool apply_gs_efx_sysex(GsEfx& efx, const uint8_t* data, size_t size,
                        bool* out_type_changed = nullptr) noexcept;

/// Insertion-effect adapter name for a GS EFX @p type: the `insert_factory`
/// processor name an adapter drives, or an empty view for a type this layer
/// does not map (the caller bypasses it and logs — no silent drop). Covers the
/// single-effect types only; a composite type has no single name and is read
/// through gs_efx_insert_chain, which is the authority on what a type realises.
/// A promotion adds an entry here (and the matching DSP) without touching the
/// parser, ABI or bindings.
std::string_view gs_efx_insert_name(uint16_t type) noexcept;

/// JSON param object (for `insert_factory` / make_insert) translating the raw
/// GS EFX parameters of @p efx into the mapped insert's parameters. The
/// Overdrive/Distortion families translate EFX PARAMETER 2 as the drive amount
/// and PARAMETER 20 as the output level (the SC-88Pro OD/Dist parameter map;
/// PARAMETER 1 is the OD/Dist selector, redundant with the EFX type). The basic
/// OD/Dist has no tone/EQ parameters — the tone comes from the amp voicing. The
/// pitch-shifter families (2-voice / feedback) translate EFX PARAMETER 1 as the
/// coarse semitone shift and PARAMETER 16 as the dry/effect balance. Tremolo
/// returns a fixed rate/depth voicing rather than a translation. Every other
/// type (mapped or not) returns "{}" so the insert plays its own defaults.
///
/// A type is translated only where the parameter's position in the block is
/// confirmed. The manual's Effect list names all 20 parameters of every type,
/// but it is not transcribed in this repository, so for the remaining types the
/// block STRUCTURE is honoured and the parameter voicing is a stated default —
/// never a guessed position.
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
///
/// The chain is a SERIES: the realiser runs the stages in order. The GS
/// parallel-2 types (0x1100–0x1108) split the signal into two effects and sum
/// them, which this shape cannot express, so they stay unmapped rather than
/// being folded into a series that would sound like a different effect under
/// the right type name. tests/midi/gs_efx_types_test.cpp enumerates all 64 types
/// and carries the reason for each one left unmapped, so a refusal is a table
/// row rather than a claim in a comment.
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
/// The three sends land on the *_send_scale fields rather than on the zone's own
/// send values: each multiplies everything the note sends into that unit rather
/// than adding to it (docs/gs.md).
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

/// One GS drum-kit variation: the rhythm-part program that selects it, its
/// zero-based kit index, its name, and the tone map that introduced it. This
/// table is the single source of truth for the kit numbering —
/// gs_drum_kit_name() and gm_fallback_drum_kit() both derive from it, so the
/// name a host reports and the variation a voice plays cannot disagree.
///
/// The kit INDEX is what a voice is voiced by, and it is deliberately neither
/// the program number nor in program order: the kits the SC-55 map defines keep
/// indices 0-9, and every kit a later map added is appended after them. A kit
/// inserted into the middle of the program range therefore cannot renumber the
/// kits already voiced.
struct GsDrumKit {
  uint8_t program;
  uint8_t index;
  std::string_view name;
  GsToneMap since;  ///< Earliest map defining this kit (never kModuleDefault).
};

/// Every GS rhythm set, across all three tone maps. Program numbers are
/// zero-based, so the owner's manual's one-based "PC 26 TR-808" appears here as
/// program 25. Where the maps disagree about a name the newest one wins: the
/// SC-55 map's "STANDARD" is the SC-88 map's "STANDARD 1", and the SC-88 map's
/// combined "TR-808/909" is split into two sets by the SC-88Pro map.
inline constexpr std::array<GsDrumKit, 26> kGsDrumKits = {{
    // The SC-55 map's sets — the ones every GS module has, whichever map a file
    // selects.
    {0, 0, "Standard", GsToneMap::kSc55},
    {8, 1, "Room", GsToneMap::kSc55},
    {16, 2, "Power", GsToneMap::kSc55},
    {24, 3, "Electronic", GsToneMap::kSc55},
    {25, 4, "TR-808", GsToneMap::kSc55},
    {32, 5, "Jazz", GsToneMap::kSc55},
    {40, 6, "Brush", GsToneMap::kSc55},
    {48, 7, "Orchestra", GsToneMap::kSc55},
    {56, 8, "SFX", GsToneMap::kSc55},
    {127, 9, "CM-64/32L", GsToneMap::kSc55},
    // Added by the SC-88 map.
    {1, 10, "Standard 2", GsToneMap::kSc88},
    {26, 11, "Dance", GsToneMap::kSc88},
    {49, 12, "Ethnic", GsToneMap::kSc88},
    {50, 13, "Kick & Snare", GsToneMap::kSc88},
    {57, 14, "Rhythm FX", GsToneMap::kSc88},
    // Added by the SC-88Pro map — the drum-machine sets and the production kits.
    {2, 15, "Standard 3", GsToneMap::kSc88Pro},
    {9, 16, "Hip Hop", GsToneMap::kSc88Pro},
    {10, 17, "Jungle", GsToneMap::kSc88Pro},
    {11, 18, "Techno", GsToneMap::kSc88Pro},
    {27, 19, "CR-78", GsToneMap::kSc88Pro},
    {28, 20, "TR-606", GsToneMap::kSc88Pro},
    {29, 21, "TR-707", GsToneMap::kSc88Pro},
    {30, 22, "TR-909", GsToneMap::kSc88Pro},
    {52, 23, "Asia", GsToneMap::kSc88Pro},
    {53, 24, "Cymbal & Claps", GsToneMap::kSc88Pro},
    {58, 25, "Rhythm FX 2", GsToneMap::kSc88Pro},
}};

/// True when @p map reaches a kit or tone introduced by @p since.
/// kModuleDefault is the newest map: a module nobody sent a map select to is
/// set to its own, and its own is the one it was built for.
constexpr bool gs_map_reaches(GsToneMap map, GsToneMap since) noexcept {
  if (map == GsToneMap::kModuleDefault) return true;
  return static_cast<uint8_t>(since) <= static_cast<uint8_t>(map);
}

/// Table entry for a rhythm-part program within @p map, or nullptr when that map
/// defines no kit at the program. Callers decide what that means: a name query
/// reports "not a kit", a voice query falls back to Standard — which is what a
/// module plays for a kit its selected map does not have.
inline const GsDrumKit* gs_drum_kit_entry(uint8_t program,
                                          GsToneMap map = GsToneMap::kModuleDefault) noexcept {
  for (const GsDrumKit& kit : kGsDrumKits) {
    if (kit.program == program) return gs_map_reaches(map, kit.since) ? &kit : nullptr;
  }
  return nullptr;
}

/// GS drum-kit name for a rhythm-part program within @p map (see kGsDrumKits).
/// A program the map defines no kit for returns an empty view.
std::string_view gs_drum_kit_name(uint8_t program,
                                  GsToneMap map = GsToneMap::kModuleDefault) noexcept;

}  // namespace sonare::midi::synth
