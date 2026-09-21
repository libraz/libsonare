/// @file gs_efx_types_test.cpp
/// @brief Per-type coverage of the GS insertion-effect (EFX) map.
///
/// The SC-88Pro defines 64 EFX types. kEfxTypes below is the enumeration of all
/// of them, and every row says whether the type realises an insert chain or is
/// refused, carrying the refusal's reason in the row. A refusal argued only in
/// a source comment is invisible to anything mechanical and reads as an
/// oversight; a row makes it a reviewed decision that expires when the type is
/// mapped, since mapping one without deleting its reason fails here.
///
/// The cases enforce six things the mapping cannot check about itself: every
/// type resolves to a chain or to a listed refusal; no mapping exists for a type
/// with no row (an exhaustive sweep of the type-number space, so a mapping added
/// without a row fails by name); every stage name is one insert_factory can
/// actually build, which is what separates a mapping that looks complete from
/// one that produces sound; two types realising the identical chain are listed
/// as such with the reason they are indistinguishable; every byte the
/// measurement archive gives a conversion to is either translated onto a control
/// that exists or counted as STATE with the reason no control does; and the set
/// of bytes that move a chain at all is exactly that translated set plus a
/// listed set of reads nothing measured. The last is the one that can see a
/// WRONG slot, which a sweep of the byte a case names never can.
///
/// **Parameter combinations.** The sweep below has five axes: the EFX type (65
/// numbers, the 64 plus the alias), the parameter slot (20), the conversion
/// class (11), the byte value (the boundaries 0, 1, 63, 64, 65, 126, 127), and
/// which table of the class applies (2 rate ranges, 5 delay ladders, 3 frequency
/// columns). Past the three-parameter threshold, so the set is a model rather
/// than a hand-picked list. The constraint that decides the model: the class and
/// the table are FUNCTIONS of (type, slot) — the generated header assigns them —
/// so they are not free axes, and a set generated as if they were would carry
/// cells no wire state can reach. What is left free is (type, slot) x byte, and
/// that product is small enough to run whole: the 85 (type, slot) pairs the
/// header names, each over all 128 byte values, which contains every realisable
/// pair of the five axes rather than a covering subset of them. The shape-only
/// case below takes the seven boundary values across all 65 types x 20 slots for
/// the same reason.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "midi/synth/gs_efx_tables.h"
#include "midi/synth/gs_layer.h"
#include "rt/processor_base.h"

namespace {

using sonare::midi::synth::apply_gs_efx_sysex;
using sonare::midi::synth::gs_efx_insert_chain;
using sonare::midi::synth::gs_efx_insert_name;
using sonare::midi::synth::gs_efx_insert_params;
using sonare::midi::synth::gs_efx_type_defaults;
using sonare::midi::synth::GsEfx;
using sonare::midi::synth::GsEfxStage;
using sonare::midi::synth::GsEfxTypeDefaults;

namespace s = sonare::midi::synth;

/// One EFX type: its number, the manual's name, and — when the type is refused
/// — why. A mapped type carries a null reason.
struct EfxType {
  uint16_t type;
  const char* name;
  const char* bypass_reason;  ///< nullptr = mapped.
};

/// Every SC-88Pro EFX type. The five groups are the manual's own: single
/// effects (MSB 01), series-2 composites (02), guitar/bass multis (04), the
/// keyboard multi (05), and the parallel-2 types (11). 0x0300 is not a 65th
/// type — it is the second number the manual prints for Rotary Multi — and is
/// listed separately in kAliasTypes.
constexpr EfxType kEfxTypes[] = {
    // MSB 01 — single effects.
    {0x0100, "Stereo-EQ", nullptr},
    {0x0101, "Spectrum", nullptr},
    {0x0102, "Enhancer", nullptr},
    {0x0103, "Humanizer",
     "a vowel formant filter whose identity is the vowel; no parameter position "
     "for the vowel is transcribed, so any vowel would be picked at random"},
    {0x0110, "Overdrive", nullptr},
    {0x0111, "Distortion", nullptr},
    {0x0120, "Phaser", nullptr},
    {0x0121, "Auto Wah", nullptr},
    {0x0122, "Rotary", nullptr},
    {0x0123, "Stereo Flanger", nullptr},
    {0x0124, "Step Flanger", nullptr},
    {0x0125, "Tremolo", nullptr},
    {0x0126, "Auto Pan", nullptr},
    {0x0130, "Compressor", nullptr},
    {0x0131, "Limiter", nullptr},
    {0x0140, "Hexa Chorus", nullptr},
    {0x0141, "Tremolo Chorus", nullptr},
    {0x0142, "Stereo Chorus", nullptr},
    {0x0143, "Space-D", nullptr},
    {0x0144, "3D Chorus", nullptr},
    {0x0150, "Stereo Delay", nullptr},
    {0x0151, "Modulation Delay", nullptr},
    {0x0152, "3 Tap Delay", nullptr},
    {0x0153, "4 Tap Delay", nullptr},
    {0x0154, "Time Control Delay", nullptr},
    {0x0155, "Reverb", nullptr},
    {0x0156, "Gate Reverb", nullptr},
    {0x0157, "3D Delay", nullptr},
    {0x0160, "2 Voice Pitch Shifter", nullptr},
    {0x0161, "Feedback Pitch Shifter", nullptr},
    {0x0170, "3D Auto",
     "a binaural panner with no stock insert; 3D Chorus and 3D Delay map "
     "because their 3D stage sits on an effect that exists, here it is the "
     "whole effect"},
    {0x0171, "3D Manual",
     "a binaural panner with no stock insert; a static azimuth, so the auto-pan "
     "insert is wrong in kind and in direction alike"},
    {0x0172, "Lo-Fi 1", nullptr},
    {0x0173, "Lo-Fi 2", nullptr},
    // MSB 02 — series-2 composites.
    {0x0200, "OD -> Chorus", nullptr},
    {0x0201, "OD -> Flanger", nullptr},
    {0x0202, "OD -> Delay", nullptr},
    {0x0203, "DS -> Chorus", nullptr},
    {0x0204, "DS -> Flanger", nullptr},
    {0x0205, "DS -> Delay", nullptr},
    {0x0206, "EH -> Chorus", nullptr},
    {0x0207, "EH -> Flanger", nullptr},
    {0x0208, "EH -> Delay", nullptr},
    {0x0209, "Chorus -> Delay", nullptr},
    {0x020A, "Flanger -> Delay", nullptr},
    {0x020B, "Chorus -> Flanger", nullptr},
    {0x020C, "Rotary Multi", nullptr},
    // MSB 04 — guitar and bass multis. These realise onto the existing amp
    // simulation rather than a second amplifier: the hardware separates the amp
    // type from the amp switch too (src/midi/synth/docs/voicing.md).
    {0x0400, "GTR Multi 1", nullptr},
    {0x0401, "GTR Multi 2", nullptr},
    {0x0402, "GTR Multi 3", nullptr},
    {0x0403, "Clean Gt Multi 1", nullptr},
    {0x0404, "Clean Gt Multi 2", nullptr},
    {0x0405, "Bass Multi", nullptr},
    {0x0406, "Rhodes Multi", nullptr},
    // MSB 05 — the keyboard multi.
    {0x0500, "Keyboard Multi", nullptr},
    // MSB 11 — parallel-2. Every one is refused for the same structural reason:
    // GsEfxStage is a series chain the realiser runs in order, so the split and
    // the sum have no representation. Folding one into a series would deliver a
    // different effect under the right type name, and unlike a bypass that is
    // invisible. Mapping them needs a branch in GsEfxStage plus a second chain
    // and a summing buffer in the realiser (Sf2Player::build_realized_efx), and
    // the positional stage/processor alignment in enqueue_efx_param_updates has
    // to become branch-aware.
    {0x1100, "Cho/Delay", "two effects in parallel; the chain shape is a series"},
    {0x1101, "FL/Delay", "two effects in parallel; the chain shape is a series"},
    {0x1102, "Cho/Flanger", "two effects in parallel; the chain shape is a series"},
    {0x1103, "OD1/OD2", "two effects in parallel; the chain shape is a series"},
    {0x1104, "OD/Rotary", "two effects in parallel; the chain shape is a series"},
    {0x1105, "OD/Phaser", "two effects in parallel; the chain shape is a series"},
    {0x1106, "OD/Auto Wah", "two effects in parallel; the chain shape is a series"},
    {0x1107, "PH/Rotary", "two effects in parallel; the chain shape is a series"},
    {0x1108, "PH/Auto Wah", "two effects in parallel; the chain shape is a series"},
};

static_assert(sizeof(kEfxTypes) / sizeof(kEfxTypes[0]) == 64,
              "the SC-88Pro defines 64 EFX types; a row was added or lost");

/// Type numbers that are not one of the 64 but do resolve to a chain. Keeping
/// them out of kEfxTypes stops the 64-count assertion from drifting, and the
/// coverage sweep still expects them to be mapped.
constexpr EfxType kAliasTypes[] = {
    {0x0300, "Rotary Multi (the manual's second number for 0x020C)", nullptr},
};

/// A group of types that realise the identical chain AND identical parameters,
/// with the reason they are currently indistinguishable. Anything the mapping
/// collides that is not listed here is drift.
struct EfxCollision {
  const char* reason;
  std::vector<uint16_t> types;
};

const std::vector<EfxCollision>& collisions() {
  static const std::vector<EfxCollision> kCollisions = {
      // The step flanger and the two delay groups were here while every delay
      // and rate byte read zero. Each is separated now by a byte the archive
      // reaches: the step flanger's own rate byte carries no conversion where
      // the stereo flanger's does, and the delay variants power up on different
      // times. What is not modelled is unchanged; what it no longer does is make
      // the types indistinguishable.
      {"Space-D and 3D Chorus power up on the same chorus rate, and neither "
       "Space-D's unmodulated voicing nor 3D Chorus's binaural stage is modelled",
       {0x0143, 0x0144}},
      {"the 3-tap delay's first two taps and the 3D delay's land on the same two times at "
       "their power-on bytes, and neither the tap counts nor the 3D stage is modelled",
       {0x0152, 0x0157}},
      {"the gate reverb's gate stage is not modelled", {0x0155, 0x0156}},
      // The two pitch shifters were here while every block read zero. They are
      // separable now without the feedback loop being modelled: the two types
      // power up on different effect balances (48 against 64), and the balance
      // is translated. The modelling gap is unchanged; what it no longer does is
      // make the pair indistinguishable.
      {"Lo-Fi 1 and Lo-Fi 2 differ in degradation parameters that are not translated",
       {0x0172, 0x0173}},
      {"one effect the manual prints under two type numbers", {0x020C, 0x0300}},
  };
  return kCollisions;
}

/// A unit holding @p type, in the state selecting it over the wire leaves: the
/// type's own twenty power-on parameters, not a block of zeros. Built directly
/// rather than through apply_gs_efx_sysex, so the round-trip case below still
/// compares two ways of arriving at the state instead of one way twice.
GsEfx make_efx(uint16_t type) {
  GsEfx efx;
  efx.type = type;
  efx.type_msb = static_cast<uint8_t>(type >> 8);
  const GsEfxTypeDefaults* defaults = gs_efx_type_defaults(type);
  if (defaults != nullptr) efx.params = defaults->params;
  efx.assigned = true;
  return efx;
}

/// The chain's identity: every stage name and its parameters, in order. Two
/// types with the same signature are indistinguishable to a listener.
std::string signature(const std::vector<GsEfxStage>& chain) {
  std::string out;
  for (const GsEfxStage& stage : chain) {
    out += stage.name;
    out += '|';
    out += stage.params_json;
    out += '\n';
  }
  return out;
}

std::vector<std::string> stage_names(const std::vector<GsEfxStage>& chain) {
  std::vector<std::string> out;
  out.reserve(chain.size());
  for (const GsEfxStage& stage : chain) out.push_back(stage.name);
  return out;
}

/// Reads a JSON number field out of an insert's params object. Returns false
/// when the key is absent, which is itself an assertable fact (an unset GS
/// parameter deliberately emits no key so the insert keeps its own default).
bool json_number(const std::string& json, const std::string& key, double& out) {
  const std::string needle = "\"" + key + "\":";
  const size_t at = json.find(needle);
  if (at == std::string::npos) return false;
  out = std::strtod(json.c_str() + at + needle.size(), nullptr);
  return true;
}

/// A GS DT1 message writing @p values from EFX block address 40 03 @p offset,
/// with the Roland checksum.
std::vector<uint8_t> efx_sysex(uint8_t offset, const std::vector<uint8_t>& values) {
  std::vector<uint8_t> msg = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, offset};
  unsigned sum = 0x40u + 0x03u + offset;
  for (uint8_t v : values) {
    msg.push_back(v);
    sum += v;
  }
  msg.push_back(static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu));
  msg.push_back(0xF7);
  return msg;
}

std::string hex4(uint16_t type) {
  static const char* kDigits = "0123456789ABCDEF";
  std::string out = "0x";
  for (int shift = 12; shift >= 0; shift -= 4) out += kDigits[(type >> shift) & 0xF];
  return out;
}

/// Where a conversion's physical quantity lands on the insert the type maps to.
/// Written by hand: the generated header names the (type, slot) pairs and their
/// conversion classes, param_field_tables.h and the insert factory name the
/// controls, and nothing in the tree joins the two. A pair is TRANSLATABLE when
/// the chain its type realises carries a control of the same physical unit, and
/// counted as STATE when it does not -- an engine that does not read a byte owes
/// it STATE rather than AUDIBLE (docs/gs.md). A STATE row carries the reason.
struct EfxJoin {
  uint16_t type;
  uint8_t slot;
  const char* conversion;    ///< class.table, as gs_efx_tables.h spells it.
  const char* stage;         ///< The chain stage the control lives on.
  const char* key;           ///< Its JSON key. Null where no control exists.
  const char* state_reason;  ///< Why none does. Null where one does.
};

constexpr const char* kPhaser = "effects.modulation.phaser";
constexpr const char* kFlanger = "effects.modulation.flanger";
constexpr const char* kChorus = "effects.modulation.chorus";
constexpr const char* kRingMod = "effects.modulation.ringModulator";
constexpr const char* kAutoPan = "stereo.autoPan";
constexpr const char* kEnsemble = "effects.modulation.ensemble";
constexpr const char* kDelay = "effects.delay.stereo";
constexpr const char* kEq = "eq.parametric";
constexpr const char* kAmpSim = "saturation.ampSim";
constexpr const char* kRotary = "effects.modulation.rotary";

/// One row per entry of kGsEfxSlotConversions, in the header's own order.
constexpr std::array<EfxJoin, 85> kJoin = {{
    {0x0100, 1, "gain.tone", kEq, "band0.gainDb", nullptr},
    {0x0100, 3, "gain.tone", kEq, "band3.gainDb", nullptr},
    {0x0100, 4, "freq.eq", kEq, "band1.frequencyHz", nullptr},
    {0x0100, 5, "width.section", kEq, "band1.q", nullptr},
    {0x0100, 6, "gain.tone", kEq, "band1.gainDb", nullptr},
    {0x0100, 7, "freq.eq", kEq, "band2.frequencyHz", nullptr},
    {0x0100, 8, "width.section", kEq, "band2.q", nullptr},
    {0x0100, 9, "gain.tone", kEq, "band2.gainDb", nullptr},
    {0x0100, 19, "level.output", nullptr, nullptr, "eq.parametric has no output gain"},
    {0x0101, 18, "pan.output", nullptr, nullptr, "eq.graphic has no pan"},
    {0x0103, 18, "pan.output", nullptr, nullptr,
     "a vowel formant filter has no insert, so the type realises no chain at all"},
    {0x0111, 19, "level.output", kAmpSim, "levelDb", nullptr},
    {0x0120, 1, "rate.wide", kPhaser, "rateHz", nullptr},
    {0x0121, 4, "rate.wide", nullptr, nullptr,
     "the auto-wah insert follows an envelope and carries no LFO rate"},
    {0x0122, 2, "accel.rotor", kRotary, "drumUndershootHz", nullptr},
    {0x0122, 6, "accel.rotor", kRotary, "undershootHz", nullptr},
    {0x0123, 3, "rate.wide", kFlanger, "rateHz", nullptr},
    {0x0125, 0, "wave.modulator", nullptr, nullptr,
     "the ring modulator's carrier is a sine and takes no shape selector"},
    {0x0125, 1, "rate.wide", kRingMod, "carrierHz", nullptr},
    {0x0126, 0, "wave.modulator", nullptr, nullptr,
     "the auto-pan insert's LFO takes no shape selector"},
    {0x0126, 1, "rate.wide", kAutoPan, "rateHz", nullptr},
    {0x0130, 18, "pan.output", nullptr, nullptr, "the compressor insert has no pan"},
    {0x0140, 0, "delay_time.pre_delay", kEnsemble, "centerDelayMs", nullptr},
    {0x0140, 19, "level.output", nullptr, nullptr, "the ensemble insert has no output level"},
    {0x0142, 1, "freq.pre_filter", kChorus, "preFilterHz", nullptr},
    {0x0142, 3, "rate.wide", kChorus, "rateHz", nullptr},
    {0x0142, 16, "gain.tone", nullptr, nullptr, "the chorus insert has no output EQ"},
    {0x0142, 17, "gain.tone", nullptr, nullptr, "the chorus insert has no output EQ"},
    {0x0143, 1, "rate.wide", kChorus, "rateHz", nullptr},
    {0x0144, 1, "rate.wide", kChorus, "rateHz", nullptr},
    {0x0150, 0, "delay_time.time3", kDelay, "delayTimeLMs", nullptr},
    {0x0150, 1, "delay_time.time3", kDelay, "delayTimeRMs", nullptr},
    {0x0150, 7, "freq.damping", kDelay, "dampingHz", nullptr},
    {0x0150, 15, "balance.effect", nullptr, nullptr,
     "the insert's mix is a crossfade, dry = 1 - wet; the measured law is two independent "
     "gains that meet at full in the middle of the byte, which the record calls the opposite "
     "sign to a crossfade"},
    {0x0150, 16, "gain.tone", nullptr, nullptr, "the stereo-delay insert has no output EQ"},
    {0x0150, 17, "gain.tone", nullptr, nullptr, "the stereo-delay insert has no output EQ"},
    {0x0151, 0, "delay_time.time3", kDelay, "delayTimeLMs", nullptr},
    {0x0151, 1, "delay_time.time3", kDelay, "delayTimeRMs", nullptr},
    {0x0151, 4, "rate.wide", nullptr, nullptr, "the stereo-delay insert carries no modulation LFO"},
    {0x0152, 0, "delay_time.time1", kDelay, "delayTimeLMs", nullptr},
    {0x0152, 1, "delay_time.time1", kDelay, "delayTimeRMs", nullptr},
    {0x0152, 2, "delay_time.time1", nullptr, nullptr,
     "the stereo-delay insert has two taps and this is a third"},
    {0x0153, 0, "delay_time.time1", kDelay, "delayTimeLMs", nullptr},
    {0x0153, 1, "delay_time.time1", kDelay, "delayTimeRMs", nullptr},
    {0x0153, 2, "delay_time.time1", nullptr, nullptr,
     "the stereo-delay insert has two taps and this is a third"},
    {0x0153, 3, "delay_time.time1", nullptr, nullptr,
     "the stereo-delay insert has two taps and this is a fourth"},
    {0x0154, 15, "balance.effect", nullptr, nullptr,
     "the insert's mix is a crossfade, dry = 1 - wet; the measured law is two independent "
     "gains that meet at full in the middle of the byte, which the record calls the opposite "
     "sign to a crossfade"},
    {0x0157, 0, "delay_time.time3", kDelay, "delayTimeLMs", nullptr},
    {0x0157, 1, "delay_time.time3", kDelay, "delayTimeRMs", nullptr},
    {0x0157, 2, "delay_time.time3", nullptr, nullptr,
     "the stereo-delay insert has two taps and this is a third"},
    {0x0157, 15, "balance.effect", nullptr, nullptr,
     "the insert's mix is a crossfade, dry = 1 - wet; the measured law is two independent "
     "gains that meet at full in the middle of the byte, which the record calls the opposite "
     "sign to a crossfade"},
    {0x0170, 0, "azimuth.placement", nullptr, nullptr,
     "a binaural panner has no insert, so the type realises no chain at all"},
    {0x0170, 1, "rate.wide", nullptr, nullptr,
     "a binaural panner has no insert, so the type realises no chain at all"},
    {0x0171, 0, "azimuth.placement", nullptr, nullptr,
     "a binaural panner has no insert, so the type realises no chain at all"},
    {0x0200, 6, "rate.wide", kChorus, "rateHz", nullptr},
    {0x0201, 6, "rate.wide", kFlanger, "rateHz", nullptr},
    {0x0203, 6, "rate.wide", kChorus, "rateHz", nullptr},
    {0x0204, 6, "rate.wide", kFlanger, "rateHz", nullptr},
    {0x0206, 6, "rate.wide", kChorus, "rateHz", nullptr},
    {0x0207, 6, "rate.wide", kFlanger, "rateHz", nullptr},
    {0x0208, 5, "delay_time.time3", kDelay, "delayTimeLMs", nullptr},
    {0x0209, 0, "delay_time.pre_delay", kChorus, "centerDelayMs", nullptr},
    {0x0209, 5, "delay_time.time3", kDelay, "delayTimeLMs", nullptr},
    {0x020A, 1, "rate.wide", kFlanger, "rateHz", nullptr},
    {0x0400, 12, "rate.narrow", kChorus, "rateHz", nullptr},
    {0x0400, 16, "delay_time.time4", kDelay, "delayTimeLMs", nullptr},
    {0x0401, 9, "gain.tone", kEq, "band0.gainDb", nullptr},
    {0x0401, 10, "freq.eq", kEq, "band1.frequencyHz", nullptr},
    {0x0401, 11, "width.section", kEq, "band1.q", nullptr},
    {0x0401, 12, "gain.tone", kEq, "band1.gainDb", nullptr},
    {0x0401, 13, "gain.tone", kEq, "band2.gainDb", nullptr},
    {0x0403, 4, "gain.tone", kEq, "band0.gainDb", nullptr},
    {0x0403, 5, "freq.eq", kEq, "band1.frequencyHz", nullptr},
    {0x0403, 6, "width.section", kEq, "band1.q", nullptr},
    {0x0403, 7, "gain.tone", kEq, "band1.gainDb", nullptr},
    {0x0403, 8, "gain.tone", kEq, "band2.gainDb", nullptr},
    {0x0403, 10, "rate.narrow", kChorus, "rateHz", nullptr},
    {0x0403, 16, "freq.damping", kDelay, "dampingHz", nullptr},
    {0x0406, 15, "wave.modulator", nullptr, nullptr,
     "the auto-pan insert's LFO takes no shape selector"},
    {0x0500, 12, "rate.narrow", kPhaser, "rateHz", nullptr},
    {0x1100, 0, "delay_time.pre_delay", nullptr, nullptr,
     "a parallel-2 type realises no chain at all"},
    {0x1100, 5, "delay_time.time3", nullptr, nullptr, "a parallel-2 type realises no chain at all"},
    {0x1101, 0, "delay_time.pre_delay", nullptr, nullptr,
     "a parallel-2 type realises no chain at all"},
    {0x1101, 5, "delay_time.time3", nullptr, nullptr, "a parallel-2 type realises no chain at all"},
    {0x1105, 6, "rate.wide", nullptr, nullptr, "a parallel-2 type realises no chain at all"},
}};

/// The conversion class and table numbers spelled out, so a header that moves a
/// pair to a different class fails here rather than being joined to a control of
/// a unit it no longer carries.
struct ConversionName {
  uint8_t conversion_class;
  uint8_t table;
  const char* name;
};

constexpr std::array<ConversionName, 18> kConversionNames = {{
    {s::kGsEfxClassRate, 0, "rate.narrow"},
    {s::kGsEfxClassRate, 1, "rate.wide"},
    {s::kGsEfxClassDelayTime, 0, "delay_time.pre_delay"},
    {s::kGsEfxClassDelayTime, 1, "delay_time.time1"},
    {s::kGsEfxClassDelayTime, 2, "delay_time.time2"},
    {s::kGsEfxClassDelayTime, 3, "delay_time.time3"},
    {s::kGsEfxClassDelayTime, 4, "delay_time.time4"},
    {s::kGsEfxClassFreq, 0, "freq.eq"},
    {s::kGsEfxClassFreq, 1, "freq.pre_filter"},
    {s::kGsEfxClassFreq, 2, "freq.damping"},
    {s::kGsEfxClassGain, 0, "gain.tone"},
    {s::kGsEfxClassLevel, 0, "level.output"},
    {s::kGsEfxClassWidth, 0, "width.section"},
    {s::kGsEfxClassWave, 0, "wave.modulator"},
    {s::kGsEfxClassPan, 0, "pan.output"},
    {s::kGsEfxClassBalance, 0, "balance.effect"},
    {s::kGsEfxClassAzimuth, 0, "azimuth.placement"},
    {s::kGsEfxClassAccel, 0, "accel.rotor"},
}};

/// A (type, slot) the translation reads although the archive gives it no
/// conversion. Every one is a reviewed exception with its reason: the list is
/// what separates "reads a byte nothing measured" from "reads the wrong byte",
/// and the sweep below fails on a read that is on neither list.
struct UnmeasuredRead {
  uint16_t type;
  uint8_t slot;
  const char* reason;
};

constexpr std::array<UnmeasuredRead, 8> kUnmeasuredReads = {{
    {0x0110, 0,
     "Drive, measured as a gain in front of one fixed curve but with no byte-to-dB "
     "conversion derived; the amp-sim drive knob takes the fraction"},
    {0x0110, 19,
     "output level; the level reading is unit-scoped at this address and names three "
     "types, of which this is not one"},
    {0x0111, 0, "Drive, as for the overdrive above"},
    {0x0142, 0,
     "the pre-filter's shape selector, measured as an enumeration rather than a "
     "conversion, so the derivation gives it no class"},
    {0x0160, 0, "Coarse Pitch, a 64-centred semitone offset the archive does not reach"},
    {0x0160, 15,
     "Effect Balance; the measured two-ramp law names three delay types and not "
     "this one, so what stands here is the older linear reading"},
    {0x0161, 0, "Coarse Pitch, as for the 2-voice shifter above"},
    {0x0161, 15, "Effect Balance, as for the 2-voice shifter above"},
}};

/// The EQ block's gain slots for one type, taken from the header rather than
/// written here.
struct EqSlots {
  uint16_t type;
  int low;
  int high;
  int count;
};

/// Every composite type the header gives gain slots to, with the first and last
/// of them: a composite's EQ is a three-gain block whose shelves bracket one
/// peaking section, and the archive lists the three in that order. `count` is
/// carried so a type that stopped being a three-gain block fails rather than
/// being read as one. The single-effect types are excluded because their gain
/// pair sits on an output EQ their insert does not have.
std::vector<EqSlots> gain_slots_by_type() {
  std::map<uint16_t, std::vector<int>> slots;
  for (const s::GsEfxSlotConversion& entry : s::kGsEfxSlotConversions) {
    if (entry.conversion_class != s::kGsEfxClassGain) continue;
    if ((entry.type >> 8) < 0x02) continue;
    slots[entry.type].push_back(entry.parameter);
  }
  std::vector<EqSlots> out;
  for (const auto& entry : slots) {
    out.push_back({entry.first, entry.second.front(), entry.second.back(),
                   static_cast<int>(entry.second.size())});
  }
  return out;
}

/// The floor the translated count may not fall below, and the ceiling the
/// untranslated count may not rise above. Both hand-written from the run that
/// first measured them, and hand-written on purpose: the population comes from
/// the generated header, so a derivation that dropped a class would shrink it
/// and a one-sided ratchet would go green on the loss.
constexpr int kGsEfxTranslatedFloor = 56;
constexpr int kGsEfxStateCeiling = 29;

std::string conversion_name(uint8_t conversion_class, uint8_t table) {
  for (const ConversionName& row : kConversionNames) {
    if (row.conversion_class == conversion_class && row.table == table) return row.name;
  }
  return "unknown";
}

/// The params of the one stage named @p name, or an empty string when the chain
/// carries no such stage or more than one (which would make the lookup silently
/// pick a side).
std::string stage_params(const std::vector<GsEfxStage>& chain, const std::string& name) {
  const std::string* found = nullptr;
  for (const GsEfxStage& stage : chain) {
    if (stage.name != name) continue;
    if (found != nullptr) return {};
    found = &stage.params_json;
  }
  return found != nullptr ? *found : std::string{};
}

/// Counts what it checked, so a run that stopped reaching the population reports
/// as a smaller number rather than as a clean one.
class Tally {
 public:
  void same(bool held, const std::string& what) {
    ++count_;
    INFO(what);
    CHECK(held);
  }

  int count() const { return count_; }

 private:
  int count_ = 0;
};

/// The byte values the boundary axis takes: the two ends, the centre and its
/// neighbours, which is where every conversion in the archive has a knot.
constexpr uint8_t kValues[] = {0, 1, 63, 64, 65, 126, 127};

/// Every type the table declares, mapped and refused alike.
std::vector<EfxType> all_rows() {
  std::vector<EfxType> rows(std::begin(kEfxTypes), std::end(kEfxTypes));
  rows.insert(rows.end(), std::begin(kAliasTypes), std::end(kAliasTypes));
  return rows;
}

}  // namespace

TEST_CASE("every GS EFX type resolves to a chain or to a listed refusal", "[midi][sf2][gs]") {
  for (const EfxType& row : all_rows()) {
    DYNAMIC_SECTION(hex4(row.type) << " " << row.name) {
      const auto chain = gs_efx_insert_chain(make_efx(row.type));
      if (row.bypass_reason == nullptr) {
        // A mapped type must produce stages; an empty chain is a silent bypass
        // wearing a mapping.
        REQUIRE_FALSE(chain.empty());
        for (const GsEfxStage& stage : chain) REQUIRE_FALSE(stage.name.empty());
      } else {
        // A refused type bypasses (and the caller logs). The reason is the row.
        INFO("refused because " << row.bypass_reason);
        REQUIRE(chain.empty());
        REQUIRE(gs_efx_insert_name(row.type).empty());
      }
    }
  }
}

TEST_CASE("no EFX type is mapped without a table row", "[midi][sf2][gs]") {
  // Exhaustive over the type-number space the GS wire can carry for the defined
  // category MSBs: a mapping added to gs_layer without a row here fails by name
  // rather than passing unnoticed.
  std::set<uint16_t> declared;
  for (const EfxType& row : all_rows()) declared.insert(row.type);

  std::vector<uint16_t> undeclared_but_mapped;
  for (unsigned msb = 0x00; msb <= 0x11; ++msb) {
    for (unsigned lsb = 0x00; lsb <= 0x7F; ++lsb) {
      const auto type = static_cast<uint16_t>((msb << 8) | lsb);
      if (gs_efx_insert_chain(make_efx(type)).empty()) continue;
      if (declared.count(type) == 0) undeclared_but_mapped.push_back(type);
    }
  }
  std::string names;
  for (uint16_t type : undeclared_but_mapped) names += hex4(type) + " ";
  INFO("mapped with no row in kEfxTypes/kAliasTypes: " << names);
  REQUIRE(undeclared_but_mapped.empty());

  // Thru (type 0) is the power-on state, not an effect: it must never realise.
  REQUIRE(gs_efx_insert_chain(make_efx(0x0000)).empty());
}

TEST_CASE("gs_efx_insert_name covers exactly the mapped single-effect types", "[midi][sf2][gs]") {
  // The single effects are the MSB-01 group; composites have no single name and
  // are read through the chain. A refused type must have no name either, or the
  // caller would build a one-stage chain from it.
  for (const EfxType& row : kEfxTypes) {
    if ((row.type >> 8) != 0x01) continue;
    DYNAMIC_SECTION(hex4(row.type) << " " << row.name) {
      if (row.bypass_reason == nullptr) {
        REQUIRE_FALSE(gs_efx_insert_name(row.type).empty());
      } else {
        REQUIRE(gs_efx_insert_name(row.type).empty());
      }
    }
  }
}

#if defined(SONARE_WITH_FX) && defined(SONARE_WITH_MASTERING)

TEST_CASE("every EFX chain stage names a processor the insert factory builds", "[midi][sf2][gs]") {
  // The failure this catches: a mapping that looks complete and produces
  // nothing, because make_insert returns null for a name that does not exist.
  // Building also parses the stage's params JSON, so a malformed object throws.
  const auto names = sonare::mastering::api::insert_factory_names();
  for (const EfxType& row : all_rows()) {
    if (row.bypass_reason != nullptr) continue;
    DYNAMIC_SECTION(hex4(row.type) << " " << row.name) {
      for (const GsEfxStage& stage : gs_efx_insert_chain(make_efx(row.type))) {
        INFO("stage " << stage.name << " params " << stage.params_json);
        REQUIRE(std::find(names.begin(), names.end(), stage.name) != names.end());
        REQUIRE(sonare::mastering::api::make_insert(stage.name, stage.params_json) != nullptr);
      }
    }
  }
}

TEST_CASE("every translated EFX parameter key is one its insert reads", "[midi][sf2][gs]") {
  // A key the processor does not read is silently ignored, so a translation
  // aimed at a misspelled key is a no-op that no audible test would catch.
  for (const EfxType& row : all_rows()) {
    if (row.bypass_reason != nullptr) continue;
    GsEfx efx = make_efx(row.type);
    efx.params.fill(100);  // every parameter written, so every translation fires
    DYNAMIC_SECTION(hex4(row.type) << " " << row.name) {
      for (const GsEfxStage& stage : gs_efx_insert_chain(efx)) {
        std::vector<std::string> unknown;
        auto processor =
            sonare::mastering::api::make_insert(stage.name, stage.params_json, &unknown);
        REQUIRE(processor != nullptr);
        std::string joined;
        for (const std::string& key : unknown) joined += key + " ";
        INFO("stage " << stage.name << " ignored: " << joined);
        REQUIRE(unknown.empty());
      }
    }
  }
}

#endif  // SONARE_WITH_FX && SONARE_WITH_MASTERING

TEST_CASE("two EFX types realise the same chain only where that is documented", "[midi][sf2][gs]") {
  std::map<std::string, std::vector<uint16_t>> by_signature;
  for (const EfxType& row : all_rows()) {
    if (row.bypass_reason != nullptr) continue;
    by_signature[signature(gs_efx_insert_chain(make_efx(row.type)))].push_back(row.type);
  }

  std::set<std::vector<uint16_t>> declared;
  for (const EfxCollision& group : collisions()) {
    std::vector<uint16_t> sorted = group.types;
    std::sort(sorted.begin(), sorted.end());
    declared.insert(sorted);
  }

  for (const auto& entry : by_signature) {
    if (entry.second.size() < 2) continue;
    std::vector<uint16_t> sorted = entry.second;
    std::sort(sorted.begin(), sorted.end());
    std::string names;
    for (uint16_t type : sorted) names += hex4(type) + " ";
    INFO("undocumented collision: " << names << "-> " << entry.first);
    REQUIRE(declared.count(sorted) == 1);
  }

  // The reverse: a documented collision that no longer collides is a stale
  // blessing, and the next type to take that shape would inherit it unexamined.
  for (const EfxCollision& group : collisions()) {
    std::vector<uint16_t> sorted = group.types;
    std::sort(sorted.begin(), sorted.end());
    std::string names;
    for (uint16_t type : sorted) names += hex4(type) + " ";
    INFO("declared collision no longer collides: " << names << " (" << group.reason << ")");
    const std::string first = signature(gs_efx_insert_chain(make_efx(sorted.front())));
    for (uint16_t type : sorted) {
      REQUIRE(signature(gs_efx_insert_chain(make_efx(type))) == first);
    }
    REQUIRE(by_signature[first].size() == sorted.size());
  }
}

TEST_CASE("a parameter-only edit never changes an EFX chain's shape", "[midi][sf2][gs]") {
  // Exhaustive over the three independent factors of the surface — type,
  // parameter index, parameter value — rather than a sampled combination, since
  // no pairwise generator is available here and the space is small enough to
  // enumerate whole. Structure comes from the type alone: parameters voice the
  // stages, they never add or remove one.
  for (const EfxType& row : all_rows()) {
    const std::vector<std::string> shape = stage_names(gs_efx_insert_chain(make_efx(row.type)));
    for (size_t index = 0; index < GsEfx{}.params.size(); ++index) {
      for (uint8_t value : kValues) {
        GsEfx efx = make_efx(row.type);
        efx.params[index] = value;
        INFO(hex4(row.type) << " " << row.name << " param " << (index + 1) << " = "
                            << static_cast<int>(value));
        REQUIRE(stage_names(gs_efx_insert_chain(efx)) == shape);
      }
    }
  }
}

TEST_CASE("EFX parameter translations move their insert control monotonically", "[midi][sf2][gs]") {
  // Only the confirmed parameter positions are translated, so only they are
  // swept. Each sweep asserts the direction the manual gives, over every byte
  // value rather than a sampled few.
  SECTION("Overdrive drive rises with EFX PARAMETER 1") {
    // PARAMETER 1 is the Drive byte and PARAMETER 2 the amp selector, which is
    // the way round the archive read them at these two types (40 03 03 answers
    // a gain in front of one fixed curve; 40 03 04 picks one of four curves).
    GsEfx efx = make_efx(0x0110);
    double previous = -1.0;
    for (int value = 0; value <= 127; ++value) {
      efx.params[0] = static_cast<uint8_t>(value);
      double drive = 0.0;
      REQUIRE(json_number(gs_efx_insert_params(efx), "drive", drive));
      INFO("PARAMETER 1 = " << value);
      REQUIRE(drive > previous);
      previous = drive;
    }
  }

  SECTION("Distortion drive rises from a higher floor than the overdrive's") {
    GsEfx od = make_efx(0x0110);
    GsEfx ds = make_efx(0x0111);
    double previous = -1.0;
    for (int value = 0; value <= 127; ++value) {
      od.params[0] = ds.params[0] = static_cast<uint8_t>(value);
      double od_drive = 0.0;
      double ds_drive = 0.0;
      REQUIRE(json_number(gs_efx_insert_params(od), "drive", od_drive));
      REQUIRE(json_number(gs_efx_insert_params(ds), "drive", ds_drive));
      INFO("PARAMETER 1 = " << value);
      REQUIRE(ds_drive > od_drive);
      REQUIRE(ds_drive > previous);
      previous = ds_drive;
    }
  }

  SECTION("output level rises with EFX PARAMETER 20, and 0 is the value zero") {
    GsEfx efx = make_efx(0x0110);
    // Swept from 0, because 0 is a level a file can ask for rather than an
    // absence: selecting the type loads this slot's own default, so no state of
    // the block means "unset".
    double previous = -1000.0;
    for (int value = 0; value <= 127; ++value) {
      efx.params[19] = static_cast<uint8_t>(value);
      double level = 0.0;
      REQUIRE(json_number(gs_efx_insert_params(efx), "levelDb", level));
      INFO("PARAMETER 20 = " << value);
      REQUIRE(level >= previous);
      previous = level;
    }
    REQUIRE(previous == 0.0);  // unity (127) is 0 dB
  }

  SECTION("pitch shift rises with EFX PARAMETER 1 and centres on 64") {
    GsEfx efx = make_efx(0x0160);
    double previous = -1000.0;
    for (int value = 1; value <= 127; ++value) {
      efx.params[0] = static_cast<uint8_t>(value);
      double semitones = 0.0;
      REQUIRE(json_number(gs_efx_insert_params(efx), "semitones", semitones));
      INFO("PARAMETER 1 = " << value);
      REQUIRE(semitones >= previous);
      previous = semitones;
    }
    efx.params[0] = 64;
    double centre = 1.0;
    REQUIRE(json_number(gs_efx_insert_params(efx), "semitones", centre));
    REQUIRE(centre == 0.0);
  }

  SECTION("effect balance rises with EFX PARAMETER 16, and 0 is the value zero") {
    GsEfx efx = make_efx(0x0160);
    // Swept from 0, for the reason the output level is: a balance of 0 is all
    // direct signal, which is a setting rather than a silence.
    double previous = -1.0;
    for (int value = 0; value <= 127; ++value) {
      efx.params[15] = static_cast<uint8_t>(value);
      double wet = 0.0;
      REQUIRE(json_number(gs_efx_insert_params(efx), "dryWet", wet));
      INFO("PARAMETER 16 = " << value);
      REQUIRE(wet > previous);
      previous = wet;
    }
  }

  SECTION("the guitar multis' EQ shelves follow the slots their own type uses") {
    // A composite's parameter block is laid out per type, and the slots come
    // from the generated header rather than from numbers written here. Sweeping
    // a byte this case names is a check that the code reads the byte the case
    // reads, which is true of every layout; taking the slots from the archive's
    // own classification is what makes the case able to see a wrong one. Bass
    // Multi has no gain slot in the header at all, so it drops out by itself.
    for (const EqSlots& row : gain_slots_by_type()) {
      double previous_low = -1000.0;
      double previous_high = -1000.0;
      for (int value = 0; value <= 127; ++value) {
        GsEfx efx = make_efx(row.type);
        efx.params[static_cast<size_t>(row.low)] = static_cast<uint8_t>(value);
        efx.params[static_cast<size_t>(row.high)] = static_cast<uint8_t>(value);
        const auto chain = gs_efx_insert_chain(efx);
        const auto eq = std::find_if(chain.begin(), chain.end(), [](const GsEfxStage& stage) {
          return stage.name == "eq.parametric";
        });
        REQUIRE(eq != chain.end());
        double low = 0.0;
        double high = 0.0;
        REQUIRE(json_number(eq->params_json, "band0.gainDb", low));
        REQUIRE(json_number(eq->params_json, "band2.gainDb", high));
        INFO(hex4(row.type) << " EQ gain byte " << value);
        REQUIRE(low >= previous_low);
        REQUIRE(high >= previous_high);
        previous_low = low;
        previous_high = high;
      }
      // The window's ends, which the byte reaches at 0x34 and 0x4C and holds
      // outside: a byte of 0 is the cut and not an absence.
      REQUIRE(previous_low == 12.0);
      REQUIRE(previous_high == 12.0);
      GsEfx zeroed = make_efx(row.type);
      zeroed.params[static_cast<size_t>(row.low)] = 0;
      double floor_db = 0.0;
      REQUIRE(json_number(stage_params(gs_efx_insert_chain(zeroed), "eq.parametric"),
                          "band0.gainDb", floor_db));
      REQUIRE(floor_db == -12.0);
    }
  }
}

TEST_CASE("Tremolo realises as amplitude modulation, not as a ring modulator", "[midi][sf2][gs]") {
  // Tremolo is UNIPOLAR amplitude modulation and ring modulation is bipolar, so
  // the mapping is only honest if the modulator never crosses zero. It does not,
  // and the control that holds it positive is dryWet: the insert's dry and wet
  // terms multiply the SAME input, so they collapse to one gain envelope
  //   out = dry*x + wet*x*sin = x*((1 - wet) + wet*sin)
  // whose minimum is 1 - 2*wet. (1 - wet) is the DC bias and wet is the depth,
  // so the envelope stays non-negative for every wet <= 0.5 and inverts above
  // it. The voicing's 0.35 gives a 0.30..1.00 envelope: a ~10 dB tremolo whose
  // peak is unity. The case below asserts that on the OUTPUT, not on the
  // algebra, and checks that 0.5 is a real edge rather than a claimed one.
  const auto chain = gs_efx_insert_chain(make_efx(0x0125));
  REQUIRE(chain.size() == 1);
  REQUIRE(chain[0].name == "effects.modulation.ringModulator");
  double carrier = 0.0;
  double wet = 0.0;
  REQUIRE(json_number(chain[0].params_json, "carrierHz", carrier));
  REQUIRE(json_number(chain[0].params_json, "dryWet", wet));
  REQUIRE(carrier > 0.0);
  REQUIRE(carrier < 20.0);
  REQUIRE(wet > 0.0);
  REQUIRE(wet < 0.5);

  // Tremolo Chorus is the chorus with that same modulation on its output, so
  // the two cannot drift apart into different DEPTHS. Their rates do part
  // company and that is the wire speaking: the standalone type's rate byte
  // carries a conversion and the chain's does not, so the chain keeps a voicing
  // where the standalone type reads a setting.
  const auto tremolo_chorus = gs_efx_insert_chain(make_efx(0x0141));
  REQUIRE(
      stage_names(tremolo_chorus) ==
      std::vector<std::string>{"effects.modulation.chorus", "effects.modulation.ringModulator"});
  double chain_wet = 0.0;
  REQUIRE(json_number(tremolo_chorus[1].params_json, "dryWet", chain_wet));
  REQUIRE(chain_wet == wet);
  double chain_carrier = 0.0;
  REQUIRE(json_number(tremolo_chorus[1].params_json, "carrierHz", chain_carrier));
  REQUIRE(chain_carrier > 0.0);
  REQUIRE(chain_carrier < 20.0);
}

#if defined(SONARE_WITH_FX) && defined(SONARE_WITH_MASTERING)

TEST_CASE("the Tremolo voicing never inverts the phase it modulates", "[midi][sf2][gs]") {
  // A constant +1 input makes the output BE the gain envelope, so any sample at
  // or below zero is a sign flip and nothing else. Half a second covers several
  // periods of the sub-audio carrier, troughs included.
  constexpr double kSampleRate = 48000.0;
  constexpr int kBlock = 512;
  constexpr int kFrames = 24000;

  auto envelope = [&](const std::string& params) {
    auto processor =
        sonare::mastering::api::make_insert("effects.modulation.ringModulator", params);
    REQUIRE(processor != nullptr);
    processor->prepare(kSampleRate, kBlock);
    std::vector<float> buffer(kFrames, 1.0f);
    for (int at = 0; at < kFrames; at += kBlock) {
      const int n = std::min(kBlock, kFrames - at);
      float* channel = buffer.data() + at;
      float* channels[] = {channel};
      processor->process(channels, 1, n);
    }
    return buffer;
  };

  const auto tremolo = envelope(gs_efx_insert_chain(make_efx(0x0125))[0].params_json);
  const float low = *std::min_element(tremolo.begin(), tremolo.end());
  const float high = *std::max_element(tremolo.begin(), tremolo.end());
  INFO("envelope spans " << low << " .. " << high);
  REQUIRE(low > 0.0f);              // unipolar: the trough closes the gate, never inverts
  REQUIRE(high <= 1.0f + 1.0e-6f);  // and the peak is unity, so the type adds no gain
  REQUIRE(high - low > 0.25f);      // and it is a real tremolo, not a near-flat gain

  // The bound is measured, not asserted: the same insert one step past dryWet
  // 0.5 does invert, which is what makes "under 0.5" a boundary rather than a
  // number chosen to make the case pass.
  const auto bipolar = envelope("{\"carrierHz\":5.0,\"dryWet\":0.60}");
  REQUIRE(*std::min_element(bipolar.begin(), bipolar.end()) < 0.0f);
}

#endif  // SONARE_WITH_FX && SONARE_WITH_MASTERING

TEST_CASE("an EFX type set over the wire reads back the same chain", "[midi][sf2][gs]") {
  for (const EfxType& row : all_rows()) {
    DYNAMIC_SECTION(hex4(row.type) << " " << row.name) {
      GsEfx efx;
      const auto msb = static_cast<uint8_t>((row.type >> 8) & 0x7Fu);
      const auto lsb = static_cast<uint8_t>(row.type & 0x7Fu);
      const auto write = efx_sysex(0x00, {msb, lsb});
      bool type_changed = false;
      REQUIRE(apply_gs_efx_sysex(efx, write.data(), write.size(), &type_changed));
      REQUIRE(efx.type == row.type);
      REQUIRE(type_changed);  // every row is a real type, never the Thru default

      const std::string expected = signature(gs_efx_insert_chain(make_efx(row.type)));
      REQUIRE(signature(gs_efx_insert_chain(efx)) == expected);
      // Reading the chain does not consume the state: a second read is equal.
      REQUIRE(signature(gs_efx_insert_chain(efx)) == expected);

      // A parameter write after the type write reports no type change and keeps
      // the chain's shape, which is the condition the realiser uses to update
      // the live processors in place instead of rebuilding them.
      const std::vector<std::string> shape = stage_names(gs_efx_insert_chain(efx));
      const auto param = efx_sysex(0x04, {0x50});  // EFX PARAMETER 2 = 80
      bool param_changed_type = true;
      REQUIRE(apply_gs_efx_sysex(efx, param.data(), param.size(), &param_changed_type));
      REQUIRE_FALSE(param_changed_type);
      REQUIRE(efx.params[1] == 0x50);
      REQUIRE(stage_names(gs_efx_insert_chain(efx)) == shape);
    }
  }
}

TEST_CASE("every reached EFX byte is translated or counted as STATE", "[midi][sf2][gs]") {
  Tally tally;

  // The join table and the generated header have to name the same pairs in the
  // same order. A header row with no join row is a byte nobody classified; a
  // join row with no header row excuses nothing and would keep the arithmetic
  // looking whole after the derivation stopped producing the pair.
  REQUIRE(kJoin.size() == s::kGsEfxSlotConversions.size());
  for (size_t i = 0; i < kJoin.size(); ++i) {
    const EfxJoin& row = kJoin[i];
    const s::GsEfxSlotConversion& entry = s::kGsEfxSlotConversions[i];
    const std::string label = hex4(row.type) + " slot " + std::to_string(row.slot);
    tally.same(row.type == entry.type && row.slot == entry.parameter,
               label + " is not the header's entry at this position");
    tally.same(conversion_name(entry.conversion_class, entry.table) == row.conversion,
               label + " has moved to another conversion class or table");
    tally.same((row.key == nullptr) != (row.state_reason == nullptr),
               label + " is neither translated onto a control nor given a reason none exists");
  }

  int translatable = 0;
  int translated = 0;
  int state = 0;
  std::map<uint16_t, int> state_by_type;
  for (const EfxJoin& row : kJoin) {
    const std::string label =
        hex4(row.type) + " slot " + std::to_string(row.slot) + " (" + row.conversion + ")";
    const auto chain = gs_efx_insert_chain(make_efx(row.type));

    if (row.key == nullptr) {
      ++state;
      ++state_by_type[row.type];
      // A STATE row is not a note: the byte has to be inert. Were it reaching a
      // control after all, the row would be stale and the count wrong in the
      // direction that flatters it.
      std::set<std::string> shapes;
      for (uint8_t value : kValues) {
        GsEfx efx = make_efx(row.type);
        efx.params[row.slot] = value;
        shapes.insert(signature(gs_efx_insert_chain(efx)));
      }
      tally.same(shapes.size() == 1, label + " is counted as STATE and yet moves its chain");
      continue;
    }

    ++translatable;
    // The stage has to be in the chain exactly once, or reading a key off "the"
    // stage of that name is reading whichever one came first.
    int named = 0;
    for (const GsEfxStage& stage : chain) {
      if (stage.name == row.stage) ++named;
    }
    tally.same(named == 1, label + " does not name exactly one stage of the chain");

    // Translated is MEASURED: sweep the byte over its whole domain and require
    // the emitted value to be there every time and to move. A key written at a
    // constant is a key the wire cannot reach, and it reads exactly like a
    // translation to anything that greps for the key.
    std::set<double> emitted;
    bool always_present = true;
    for (int value = 0; value <= 127; ++value) {
      GsEfx efx = make_efx(row.type);
      efx.params[row.slot] = static_cast<uint8_t>(value);
      double number = 0.0;
      if (!json_number(stage_params(gs_efx_insert_chain(efx), row.stage), row.key, number)) {
        always_present = false;
        break;
      }
      emitted.insert(number);
    }
    tally.same(always_present, label + " does not emit " + row.key + " at every byte value");
    tally.same(emitted.size() >= 2,
               label + " emits " + row.key + " at a constant, which is not a translation");
    if (always_present && emitted.size() >= 2) ++translated;
  }

  // Success condition 1. The first is the one that says the wiring is complete;
  // the two after it are the ratchet, pinned from both sides because the
  // population is generated and a one-sided ratchet goes green when it shrinks.
  tally.same(translatable - translated == 0, "a translatable byte is not translated");
  tally.same(translated >= kGsEfxTranslatedFloor, "the translated count fell below its floor");
  tally.same(state <= kGsEfxStateCeiling, "the untranslated count rose above its ceiling");
  tally.same(translatable + state == s::kGsEfxReached,
             "translatable and STATE do not account for every reached pair");

  std::string breakdown;
  for (const auto& entry : state_by_type) {
    breakdown += hex4(entry.first) + ":" + std::to_string(entry.second) + " ";
  }
  WARN("reached: " << s::kGsEfxReached << "  translatable: " << translatable
                   << "  translated: " << translated << "  STATE: " << state);
  WARN("STATE by type: " << breakdown);
  WARN("comparisons: " << tally.count());
  REQUIRE(tally.count() >= 300);
}

TEST_CASE("the translation reads exactly the bytes the archive named", "[midi][sf2][gs]") {
  // The inverse of the case above, and the one that can see a wrong slot. A
  // sweep of the byte a test names only shows that the code reads the byte the
  // test reads, which is true of every layout: what separates a right layout
  // from a wrong one is WHICH bytes move the chain. So every slot of every type
  // is swept, and each one that moves anything has to be either a translatable
  // pair the archive named or a listed exception with its reason.
  //
  // This is what the zeroed block hid. While every params byte read 0, a slot
  // the code read and a slot it did not read produced the same output, so no
  // check of this shape could have been written; the defaults pour is what made
  // the two separable.
  Tally tally;
  std::set<std::pair<uint16_t, int>> named;
  for (const EfxJoin& row : kJoin) {
    if (row.key != nullptr) named.insert({row.type, row.slot});
  }
  std::set<std::pair<uint16_t, int>> excepted;
  for (const UnmeasuredRead& row : kUnmeasuredReads) {
    excepted.insert({row.type, row.slot});
  }

  std::set<std::pair<uint16_t, int>> moved;
  for (const EfxType& type_row : all_rows()) {
    const std::string base = signature(gs_efx_insert_chain(make_efx(type_row.type)));
    for (size_t slot = 0; slot < GsEfx{}.params.size(); ++slot) {
      bool moves = false;
      for (uint8_t value : kValues) {
        GsEfx efx = make_efx(type_row.type);
        efx.params[slot] = value;
        if (signature(gs_efx_insert_chain(efx)) != base) moves = true;
      }
      if (!moves) continue;
      moved.insert({type_row.type, static_cast<int>(slot)});
      const std::string label = hex4(type_row.type) + " slot " + std::to_string(slot);
      tally.same(named.count({type_row.type, static_cast<int>(slot)}) == 1 ||
                     excepted.count({type_row.type, static_cast<int>(slot)}) == 1,
                 label + " moves the chain and the archive gives it no conversion");
    }
  }

  // Both directions. A translatable pair that moves nothing is a translation
  // aimed at a byte the type does not carry; an exception that moves nothing is
  // a note about code that has gone away.
  for (const auto& pair : named) {
    tally.same(moved.count(pair) == 1, hex4(pair.first) + " slot " + std::to_string(pair.second) +
                                           " is counted as translated and moves nothing");
  }
  for (const UnmeasuredRead& row : kUnmeasuredReads) {
    tally.same(moved.count({row.type, row.slot}) == 1,
               hex4(row.type) + " slot " + std::to_string(row.slot) +
                   " is excused as an unmeasured read and is not read at all");
  }

  WARN("slots that move a chain: " << moved.size() << "  named by the archive: " << named.size()
                                   << "  listed exceptions: " << kUnmeasuredReads.size());
  WARN("comparisons: " << tally.count());
  REQUIRE(tally.count() >= 120);
}
