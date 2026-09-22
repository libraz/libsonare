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

#include "midi/synth/channel_param_state.h"
#include "midi/synth/gs_address_table.h"
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
  kSc8850 = 4,         ///< CC#32 = 4: the SC-8850's own map, which the target has.
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
    case 4:
      return GsToneMap::kSc8850;
    default:
      return GsToneMap::kModuleDefault;
  }
}

/// The eighteen per-part receive switches, in address order: sixteen at
/// `40 1x 03`-`12` and two more at `40 1x 23`-`24`, which the map prints apart
/// from the block because they arrived with the bank-select rules rather than
/// with the rest. The two halves are contiguous here.
///
/// A switch says whether the part receives one class of message at all, so one
/// that is off is an absence rather than an attenuation — the part keeps
/// whatever value it already held, and no later message of that class corrects
/// it until the switch comes back on. RX BANK SELECT LSB is the one exception,
/// and the map states it: the LSB is read as 00 rather than not read at all.
enum class GsRxSwitch : uint8_t {
  kPitchBend,        ///< 40 1x 03
  kChannelPressure,  ///< 40 1x 04
  kProgramChange,    ///< 40 1x 05
  kControlChange,    ///< 40 1x 06, the master switch over every controller below
  kPolyPressure,     ///< 40 1x 07
  kNoteMessage,      ///< 40 1x 08
  kRpn,              ///< 40 1x 09
  kNrpn,             ///< 40 1x 0A
  kModulation,       ///< 40 1x 0B, CC1
  kVolume,           ///< 40 1x 0C, CC7
  kPanpot,           ///< 40 1x 0D, CC10
  kExpression,       ///< 40 1x 0E, CC11
  kHold1,            ///< 40 1x 0F, CC64
  kPortamento,       ///< 40 1x 10, CC65
  kSostenuto,        ///< 40 1x 11, CC66
  kSoft,             ///< 40 1x 12, CC67
  kBankSelect,       ///< 40 1x 23, over CC0 and CC32 alike
  kBankSelectLsb,    ///< 40 1x 24, over CC32's value rather than its arrival
  kCount,
};

/// Every switch on, which is what a GS Reset leaves. A GM System On clears
/// three: Rx. NRPN whichever level it names, and the two bank-select switches
/// for GM1 alone (docs/gs.md).
inline constexpr uint32_t kGsRxAllOn = (1u << static_cast<uint8_t>(GsRxSwitch::kCount)) - 1u;

constexpr uint32_t gs_rx_switch_bit(GsRxSwitch which) noexcept {
  return 1u << static_cast<uint8_t>(which);
}

/// The switch bit @p addr writes, for an address inside either receive-switch
/// block. Derived from the address rather than written per row, so a row and its
/// bit cannot drift; the break at 0x23 is the map's own.
constexpr uint32_t gs_rx_switch_bit(uint32_t addr) noexcept {
  const uint32_t low = addr & 0xFFu;
  return low <= 0x12u ? 1u << (low - 0x03u)
                      : gs_rx_switch_bit(GsRxSwitch::kBankSelect) << (low - 0x23u);
}

// The two derivations meet at each block's ends, which is where a member
// inserted into the enumerator would part them.
static_assert(gs_rx_switch_bit(0x401003u) == gs_rx_switch_bit(GsRxSwitch::kPitchBend));
static_assert(gs_rx_switch_bit(0x401012u) == gs_rx_switch_bit(GsRxSwitch::kSoft));
static_assert(gs_rx_switch_bit(0x401023u) == gs_rx_switch_bit(GsRxSwitch::kBankSelect));
static_assert(gs_rx_switch_bit(0x401024u) == gs_rx_switch_bit(GsRxSwitch::kBankSelectLsb));

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

/// @p gs as voice-applicable quantities (GsPartMod, channel_param_state.h).
GsPartMod gs_part_mod(const GsPartParams& gs) noexcept;

/// The six controller sources of the controller-destination block (40 2x xx),
/// in address order: the low byte is the source in its high nibble and the
/// destination in its low one.
enum class GsCtrlSource : uint8_t {
  kModulation = 0,
  kBend,
  kChannelAftertouch,
  kPolyAftertouch,
  kCc1,
  kCc2,
};
inline constexpr size_t kGsCtrlSourceCount = 6;

/// The source half of a controller-destination address.
constexpr uint8_t gs_ctrl_source_index(uint32_t addr) noexcept {
  return static_cast<uint8_t>((addr >> 4) & 0x0Fu);
}

/// One source's half of the block: per destination, what that source at FULL is
/// worth. What it is worth now is this scaled by the source's own position, and
/// a destination takes the sum over sources (docs/gs.md) — which is why the
/// values are held per source rather than pre-summed, and why the set is an
/// array rather than a field per source-destination pair.
struct GsDestinationSet {
  float pitch_cents = 0.0f;      ///< +00 PITCH CONTROL
  float cutoff_cents = 0.0f;     ///< +01 TVF CUTOFF CONTROL
  float amp_fraction = 0.0f;     ///< +02 AMPLITUDE CONTROL
  float vib_depth_cents = 0.0f;  ///< +04 LFO1 PITCH DEPTH
  float tvf_lfo_cents = 0.0f;    ///< +05 LFO1 TVF DEPTH
  float tva_depth = 0.0f;        ///< +06 LFO1 TVA DEPTH
  /// +03 LFO1 RATE CONTROL, held as its written byte where the rest are held
  /// converted: the sources meet in this one's exponent rather than in a
  /// full-scale amount, so the conversion happens after they are summed.
  uint8_t lfo_rate = 0x40;
};

/// The GS power-on position of every MIDI controller a part tracks. Only the
/// three that do not power on at zero are named; everything else starts where
/// a controller nobody has moved sits.
constexpr std::array<uint8_t, 128> gs_default_cc_positions() noexcept {
  std::array<uint8_t, 128> p{};
  p[7] = 100;   // CC7 volume
  p[10] = 64;   // CC10 pan, centred
  p[11] = 127;  // CC11 expression
  return p;
}

/// Whether any source names a TVF destination, which is what a note-on has to
/// settle: the offset and the swing both arrive per sample from a controller,
/// so what the note-on decides is only whether there is a filter to reach.
constexpr bool gs_part_has_filter_destination(
    const std::array<GsDestinationSet, kGsCtrlSourceCount>& dest) noexcept {
  for (const GsDestinationSet& d : dest) {
    if (d.cutoff_cents != 0.0f || d.tvf_lfo_cents != 0.0f) return true;
  }
  return false;
}

/// The reset state of a part's whole block. Every source is inert but the
/// modulation wheel, whose LFO1 PITCH DEPTH powers on at 0A where every other
/// source's powers on at 00.
constexpr std::array<GsDestinationSet, kGsCtrlSourceCount> gs_default_destinations() noexcept {
  std::array<GsDestinationSet, kGsCtrlSourceCount> d{};
  d[static_cast<size_t>(GsCtrlSource::kModulation)].vib_depth_cents =
      gs_mod_depth_cents(kGsModDepthDefault);
  return d;
}

/// GS SCALE TUNING (40 1x 40-4B): one byte per pitch class from C, 40 = in tune
/// and one cent a step. A temperament, so it is indexed by the key that was
/// struck rather than by any note a GS parameter substituted for it.
using GsScaleTuning = std::array<uint8_t, 12>;

/// The twelve bytes that detune nothing, which is the reset state.
inline constexpr GsScaleTuning kGsScaleTuningEqual{
    {0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40}};

/// The cents @p scale offsets @p note by. Both voice banks take the offset from
/// here and apply it in their own idiom — a sample increment on one, a base
/// frequency on the other — so neither can read the table its own way.
constexpr float gs_scale_tuning_cents(const GsScaleTuning& scale, uint8_t note) noexcept {
  return static_cast<float>(static_cast<int>(scale[(note & 0x7Fu) % 12u]) - 0x40);
}

/// PITCH OFFSET FINE (40 1x 17-18) as the cents it offsets @p note by. The two
/// nibbles make one 08-F8 byte centred on 80 in 0.1-Hz steps, and the parameter
/// is a fixed FREQUENCY shift rather than a fixed interval — the manual's point
/// in printing it beside RPN 00 01 — so the interval it works out to belongs to
/// the note. Both voice banks take it from here for the reason scale tuning is
/// shared. Zero at the centre, which leaves an untuned render bit-identical.
float gs_pitch_offset_fine_cents(uint8_t value, uint8_t note) noexcept;

/// The velocity a part sounds @p velocity at, under VELOCITY SENSE DEPTH
/// (40 1x 1A) and VELOCITY SENSE OFFSET (40 1x 1B), both centred on 40.
///
/// The curve is libsonare's: the manual gives the two a range and a default and
/// no mapping at all, as it does MASTER VOLUME. Depth is the SLOPE and pivots on
/// the centre of the velocity axis rather than on zero, which is what makes the
/// parameter's name true — a depth of 0 sounds every key alike instead of
/// silencing them, and the part stops answering velocity rather than stopping.
/// Offset then moves the whole curve. At the power-on 40/40 the result is the
/// written velocity exactly, so an untouched part renders bit-identically.
///
/// The result is clamped to 1-127: a shaped 0 is a note-off on the wire and this
/// is a note that was struck.
constexpr uint8_t gs_velocity_sense(uint8_t depth, uint8_t offset, uint8_t velocity) noexcept {
  if (depth == 0x40 && offset == 0x40) return velocity;
  const int shaped =
      static_cast<int>((static_cast<float>(velocity) - 64.0f) * static_cast<float>(depth) / 64.0f) +
      static_cast<int>(offset);
  return static_cast<uint8_t>(shaped < 1 ? 1 : (shaped > 127 ? 127 : shaped));
}

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
/// the lsb, and the drum setup addresses 41 m1/m2/m3/m4/m5/m6/m9 rr — PLAY NOTE
/// NUMBER and ASSIGN GROUP reachable from the address alone, the rest from
/// either side).
///
/// Every field holds the value at which the parameter changes nothing. The
/// manual gives these no power-on value because a drum set change re-initialises
/// them to what the kit itself specifies, so an unwritten parameter has to mean
/// "the kit's" — which is also why each field is read only behind its flag. PLAY
/// NOTE NUMBER has no such value at all, its identity being the struck note, so
/// the flag is the whole of what an unwritten one means.
struct GsDrumNoteParams {
  enum Flag : uint16_t {
    kPitch = 1u << 0,
    kLevel = 1u << 1,
    kPan = 1u << 2,
    kReverb = 1u << 3,
    kChorus = 1u << 4,
    kDelay = 1u << 5,
    kPlayNote = 1u << 6,
    kAssignGroup = 1u << 7,
    kRxNoteOn = 1u << 8,
  };
  uint16_t flags = 0;
  int8_t pitch_coarse = 0;   // semitones (data - 64)
  uint8_t level = 127;       // absolute TVA level (data)
  uint8_t pan = 64;          // absolute pan (data; 64 = centre)
  uint8_t reverb = 127;      // reverb-send multiplicand (data)
  uint8_t chorus = 127;      // chorus-send multiplicand (data)
  uint8_t delay = 127;       // delay-send multiplicand (data)
  uint8_t play_note = 0;     // the note whose sound is played (data)
  uint8_t assign_group = 0;  // exclusive/mute group, 0 = none (data)
  uint8_t rx_note_on = 1;    // 0 = the note is not sounded at all (data)

  bool any() const noexcept { return flags != 0; }
};

/// Rhythm-part programs selecting the two user drum sets at 21 dn rr: 64 is set
/// 1 and 65 is set 2. They sit outside kGsDrumKits, which is what leaves them
/// free to mean this.
inline constexpr uint8_t kGsUserDrumSetProgram = 64;
inline constexpr uint8_t kGsUserDrumSetCount = 2;

/// The user drum set @p program selects on a rhythm part, or -1 for a program
/// that selects a preset kit.
constexpr int gs_user_drum_set(uint8_t program) noexcept {
  const int index = static_cast<int>(program & 0x7Fu) - kGsUserDrumSetProgram;
  return index >= 0 && index < kGsUserDrumSetCount ? index : -1;
}

/// Where one note of a user drum set takes its sound from (GS addresses
/// 21 dB/dC rr): a rhythm program, and the note within that kit. The rest of a
/// user drum note is GsDrumNoteParams, which the set shares with the drum setup
/// block; 21 dA rr, the generation the program is read in, is held nowhere
/// (docs/gs.md).
///
/// The program holds the value that changes nothing — the Standard kit a rhythm
/// part already falls back to — so a set nobody wrote sounds as the part would
/// without it. The source NOTE has no such value, its identity being the struck
/// note, so it is read behind its flag.
struct GsUserDrumSource {
  enum Flag : uint8_t {
    kSourceNote = 1u << 0,
  };
  uint8_t flags = 0;
  uint8_t program = 0;      // rhythm-part program of the source kit (data)
  uint8_t source_note = 0;  // the note within that kit (data)
};

/// The note @p src sounds for a strike on @p struck — the struck note itself
/// where the set redirects nothing, and for @p src null, which is a part playing
/// a preset kit.
constexpr uint8_t gs_user_drum_sound_note(const GsUserDrumSource* src, uint8_t struck) noexcept {
  if (src != nullptr && (src->flags & GsUserDrumSource::kSourceNote) != 0) {
    return src->source_note & 0x7Fu;
  }
  return struck & 0x7Fu;
}

/// @p stored with @p live over it: every field @p live wrote wins, and the rest
/// stay as @p stored left them. A user drum set is the kit, the drum setup block
/// at 41 mn rr is the edit on it, and layering them is what lets both hold the
/// value that changes nothing (docs/gs.md).
GsDrumNoteParams gs_layer_drum_note_params(const GsDrumNoteParams& stored,
                                           const GsDrumNoteParams& live) noexcept;

/// GS insertion effect (EFX) state, stored as the RAW GS wire so any adapter
/// can interpret it without a typed per-effect struct. The SC-55/88 EFX is a
/// single insertion unit whose type is a 14-bit number (two 7-bit SysEx bytes)
/// and whose 20 parameters are raw 0..127 bytes; each adapter reads only the
/// parameters it uses. Keeping the wire raw means a new EFX algorithm is a new
/// adapter over the same bytes — no parser, struct or ABI change.
struct GsEfx {
  /// EFX type number: (MSB << 8) | LSB, matching the two-byte GS notation
  /// (e.g. 0x0110). 0 = the power-on default (Thru / no insertion effect). This
  /// is the RESOLVED type: a type resolves when its LSB arrives, so between an
  /// MSB-only write and its LSB this still holds the previous type.
  uint16_t type = 0;
  /// The TYPE MSB (40 03 00) as last written, which is not yet part of `type`
  /// when it arrived on its own. Held apart so an MSB-only write cannot realise
  /// the effect under (new MSB, old LSB) — a type number that does not exist.
  uint8_t type_msb = 0;
  /// EFX PARAMETER 1..20 (GS address 40 03 03..16), raw 0..127. Selecting a type
  /// loads that type's twenty power-on bytes over these, so 0 is the value zero
  /// and never "unset": almost none of the machine's defaults is zero. Initially
  /// the Thru type's own, which is what the block powers on holding.
  std::array<uint8_t, 20> params = gs_efx_power_on_params();
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

/// The number of insertion units: the spec unit plus the extension's fifteen.
inline constexpr size_t kGsEfxUnitCount = 16;

/// The insertion unit a 40 4x 22 PART EFX ASSIGN @p value selects, or -1 for
/// BYPASS. `00` bypasses, `01` is the spec unit 0, and `02`-`10` are the
/// libsonare extension's units 1-15 (docs/gs.md). A value the row does not
/// accept is ignored at the parse layer and so should not reach here; it answers
/// bypass rather than a unit anyway, since the result indexes an array.
constexpr int gs_efx_assign_unit(uint8_t value) noexcept {
  return value == 0 || value > kGsEfxUnitCount ? -1 : static_cast<int>(value) - 1;
}

/// The insertion unit an EFX-block write addresses, or -1 for a write outside
/// every EFX block. Unit 0 is the spec block at 40 03 xx and is also reachable
/// at 40 30 xx, where the extension's uniform layout puts it; units 1-15 are at
/// 40 31 xx - 40 3F xx (docs/gs.md). Accepts the payload with or without F0/F7
/// framing. Never crashes.
int gs_efx_addressed_unit(const uint8_t* data, size_t size) noexcept;

/// Applies a GS DT1 write to an EFX block (address 40 03 xx or 40 3u xx) onto @p efx,
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
/// A TYPE resolves on its LSB (40 03 01), pairing the arriving byte with the
/// stored MSB, and selecting a type loads that type's twenty power-on
/// parameters. Three consequences, all the machine's:
/// - A bulk DT1 applies bytes in address order, so the parameters that follow
///   the type in the same message survive the load rather than being overwritten
///   by it.
/// - An MSB-only write changes nothing: resolving there would load the defaults
///   of (new MSB, old LSB), a type that does not exist.
/// - A file that writes a parameter BEFORE its type loses that byte.
/// A type the archive behind gs_efx_tables.h never measured loads nothing and
/// leaves the block as it stood — a measurement gap rather than a rule.
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

/// The skeleton's own JSON params for a single-effect type's insert (for
/// `insert_factory` / make_insert): band shapes and fixed corners, mode
/// selectors, and the bytes the skeleton reads under a law of its own (the
/// Overdrive/Distortion drive, the pitch shifter's balance). Every byte a
/// measured law reaches is written by the binding table instead, which
/// gs_efx_insert_chain applies, so this object alone is not the type's realised
/// parameters. A type with nothing of the skeleton's returns "{}".
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
/// faithful to the hardware; each bound byte reaches the stage its binding row
/// names, and a control no row reaches keeps the insert's default.
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

// --- controller destinations (40 2x xx) ---
//
// The `mod_` in these names is the destination block rather than the modulation
// wheel: every source that reaches a destination reaches it through the same
// conversion, and the sources sum on the way in (docs/gs.md). What each takes is
// one source's byte; what a source is worth is that scaled by its own position.

/// PITCH CONTROL (40 2x 00 / 20) as the pitch offset a controller at full adds,
/// in cents. Whole semitones above 40 over the row's own 28-58 range, so the
/// full swing is a two-octave bend either way; it lands on the same field the
/// pitch wheel does, and like MASTER TUNE and RPN 00 01 the two add.
float gs_mod_pitch_cents(uint8_t value) noexcept;

/// TVF CUTOFF CONTROL (40 2x 01 / 21) as the cutoff offset a controller at full
/// adds, in cents. The byte is centred on 40 and buys the same step
/// gs_cutoff_offset_cents already gives the TONE MODIFY cutoff, so a controller
/// reaches one quantity rather than a second that happens to resemble it. A
/// controller below full scales the result down; 40 is the range's no-op.
float gs_mod_cutoff_cents(uint8_t value) noexcept;

/// LFO1 RATE CONTROL (40 2x 03 / 23) as the cents of LFO frequency a controller
/// at full is worth. The byte buys the same 25 cents a step that the TONE MODIFY
/// vibrato rate does, so a controller reaches that rate rather than a second
/// one. Returned in cents rather than as a multiplier because the quantity is
/// exponential and two sources reach it: each source's contribution is scaled by
/// its own position and the sum goes into one exponent, which is what makes a
/// controller at rest worth exactly 1 instead of something near it.
float gs_mod_lfo_rate_cents(uint8_t value) noexcept;

/// AMPLITUDE CONTROL (40 2x 02 / 22) as the fraction of its own level a
/// controller at full adds to the part, over the manual's -100..+100 %. The byte
/// is centred on 40 and the sum of every source's contribution multiplies the
/// part's linear gain as 1 + sum, floored at zero: two sources each asking for
/// -100 % cannot take a gain below silence and back up inverted.
float gs_mod_amp_fraction(uint8_t value) noexcept;

/// LFO1 TVA DEPTH (40 2x 06 / 26) as the fraction of a part's amplitude a
/// controller at full swings away, 0 for no tremolo and 1 for a swing to
/// silence. A controller scales it linearly and the sources sum, clamped at 1
/// for the reason the amplitude is floored at 0. The LFO is the same one
/// 40 2x 03 retunes and 40 2x 04 gives its pitch depth: LFO1 is one oscillator
/// with four destinations, not four of them.
float gs_mod_tva_depth(uint8_t value) noexcept;

/// Applies the melodic part offsets onto resolved voice parameters.
void apply_gs_part_params(Sf2VoiceParams& params, const GsPartParams& gs) noexcept;

/// Applies the per-note drum overrides onto resolved voice parameters.
/// The three sends land on the *_send_scale fields rather than on the zone's own
/// send values: each multiplies everything the note sends into that unit rather
/// than adding to it (docs/gs.md).
void apply_gs_drum_params(Sf2VoiceParams& params, const GsDrumNoteParams& drum) noexcept;

// --- SysEx surface ---

/// Which General MIDI level a System On names. The two resets are not
/// interchangeable: GM1 leaves the bank-select switches off and GM2 leaves them
/// on, so a GM2 file can still reach a variation where a GM1 file cannot
/// (docs/gs.md).
enum class GmLevel : uint8_t {
  kGeneralMidi1,
  kGeneralMidi2,
};

enum class GsSysExKind : uint8_t {
  kNone = 0,
  kGm1Reset,       ///< GM1 System On (F0 7E 7F 09 01 F7)
  kGm2Reset,       ///< GM2 System On (F0 7E 7F 09 03 F7)
  kGsReset,        ///< GS Reset (F0 41 dd 42 12 40 00 7F 00 41 F7)
  kUseForRhythm,   ///< GS part rhythm assignment (40 1x 15 mm)
  kEfxPartSwitch,  ///< GS per-part EFX on/off (40 4x 22 mm): routes the part
                   ///< through the single insertion effect (value 1 = on).
};

/// Whether @p kind re-initialises the module. A named predicate rather than a
/// comparison written out at each site, so a reset kind added later reaches
/// every branch that resets rather than the ones someone remembered.
constexpr bool gs_sysex_resets(GsSysExKind kind) noexcept {
  return kind == GsSysExKind::kGsReset || kind == GsSysExKind::kGm1Reset ||
         kind == GsSysExKind::kGm2Reset;
}

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
