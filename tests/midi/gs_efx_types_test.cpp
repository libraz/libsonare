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
/// as such with the reason they are indistinguishable; every adjudicated byte
/// does what the binding files say it does; and the set of bytes that move a
/// chain at all is exactly the bound set plus the set the skeleton owns. The
/// last is the one that can see a WRONG slot, which a sweep of the byte a case
/// names never can.
///
/// The adjudication is read from gs_efx_join.h, rendered from the binding files
/// rather than written here. A byte reaches a control, or it is inert and the
/// row says why, or the skeleton converts it under a law of its own; and the
/// three are separated by measurement rather than by the word chosen for them,
/// which is what a hand-written table of the same rows could not do about
/// itself.
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
/// that product is small enough to run whole: every (type, slot) a binding file
/// assigns to a control, each over all 128 byte values, which contains every
/// realisable pair of the five axes rather than a covering subset. The shape-only
/// case below takes the seven boundary values across all 65 types x 20 slots for
/// the same reason.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "midi/gs_efx_join.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_efx_bindings.h"
#include "midi/synth/gs_efx_convert.h"
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
     "a vowel formant filter whose identity is the vowel; the vowel has a "
     "parameter position of its own, so what is missing is the formant filter "
     "to receive it, and a fixed vowel would be a resonant filter chosen at random"},
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
      // Space-D and 3D Chorus were here for the same reason, and are separated
      // now by their own pre-delay bytes and by their output levels. Neither
      // Space-D's unmodulated voicing nor 3D Chorus's binaural stage is
      // modelled; what that no longer does is make the pair indistinguishable.
      {"the 3-tap delay's first two taps and the 3D delay's land on the same two times at "
       "their power-on bytes, and neither the tap counts nor the 3D stage is modelled",
       {0x0152, 0x0157}},
      // The two reverbs were here too, and part company now on their output
      // stage: the gate type powers up on a different high-shelf gain and a
      // different output level. The gate stage is still not modelled.
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

/// The conversion class and table numbers spelled out, so a header that moves a
/// pair to a different class fails here rather than being joined to a control of
/// a unit it no longer carries.
struct ConversionName {
  uint8_t conversion_class;
  uint8_t table;
  const char* name;
};

constexpr std::array<ConversionName, 19> kConversionNames = {{
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
    {s::kGsEfxClassRatio, 0, "ratio.percent"},
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

/// The floors the measured counts may not fall below, hand-written from the run
/// that first measured them and hand-written on purpose: every other number in
/// the case below is rendered from the binding files, so a file that lost rows
/// would shrink both the claim and the check together and read as clean.
///
/// There is deliberately no ceiling on the documented-state count. Parameters
/// nobody has adjudicated yet mostly become states as they are looked at, so a
/// ceiling would go red on the lane finishing its own work; what a downgrade of
/// a translation would have to get past is the translated floor.
constexpr int kGsEfxTranslatedFloor = 231;
constexpr int kGsEfxAdjudicatedFloor = 598;

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

TEST_CASE("every GS EFX type resolves to a chain or to a listed refusal",
          "[midi][sf2][gs][efxtypes]") {
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

TEST_CASE("no EFX type is mapped without a table row", "[midi][sf2][gs][efxtypes]") {
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

TEST_CASE("gs_efx_insert_name covers exactly the mapped single-effect types",
          "[midi][sf2][gs][efxtypes]") {
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

TEST_CASE("every EFX chain stage names a processor the insert factory builds",
          "[midi][sf2][gs][efxtypes]") {
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

TEST_CASE("every translated EFX parameter key is one its insert reads",
          "[midi][sf2][gs][efxtypes]") {
  // A key the processor does not read is silently ignored, so a translation
  // aimed at a misspelled key is a no-op that no audible test would catch.
  //
  // Swept over the boundary bytes rather than taken at one filling: a key a
  // translation only emits for part of the byte's range -- a mode selector
  // written below a threshold, say -- is absent from a single reading and so
  // never checked at all. Filling every slot with the same value keeps that
  // cheap, since what is under test is the key's spelling and not its value.
  for (uint8_t value : kValues) {
    for (const EfxType& row : all_rows()) {
      if (row.bypass_reason != nullptr) continue;
      GsEfx efx = make_efx(row.type);
      efx.params.fill(value);  // every parameter written, so every translation fires
      DYNAMIC_SECTION(hex4(row.type) << " " << row.name << " at " << static_cast<int>(value)) {
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
}

TEST_CASE("a documented state's missing control is one its insert really lacks",
          "[midi][sf2][gs][efxtypes]") {
  // A state row says the byte reaches nothing, and most of them say why in
  // prose nothing reads. Where the reason is that the insert has no such
  // control, the row names it, and the claim is checked against the insert
  // rather than believed: the failure it exists for is an insert growing the
  // control years later and the parameter staying unbound because the note
  // explaining why went stale in a file nobody rereads.
  //
  // A row whose missing control has no established spelling anywhere carries
  // prose alone, deliberately -- a claim naming a key no insert would ever use
  // is one that can never go red.
  Tally tally;
  for (const s::GsEfxJoinRow& row : s::kGsEfxJoin) {
    if (row.absent_stage == s::kGsEfxJoinNoName) continue;
    const std::string stage(s::kGsEfxJoinStages[row.absent_stage]);
    const std::string key(s::kGsEfxJoinKeys[row.absent_key]);
    const std::string label = hex4(row.type) + " slot " + std::to_string(row.slot);

    int named = 0;
    for (const GsEfxStage& entry : gs_efx_insert_chain(make_efx(row.type))) {
      if (entry.name == stage) ++named;
    }
    tally.same(named == 1, label + " names " + stage + ", which its chain does not carry once");

    const std::vector<std::string> reads = sonare::mastering::api::insert_param_names(stage);
    tally.same(std::find(reads.begin(), reads.end(), key) == reads.end(),
               label + " is a documented state because " + stage + " has no " + key + ", and " +
                   stage + " reads one now");
  }
  WARN("comparisons: " << tally.count());
  REQUIRE(tally.count() >= 12);
}

#endif  // SONARE_WITH_FX && SONARE_WITH_MASTERING

TEST_CASE("two EFX types realise the same chain only where that is documented",
          "[midi][sf2][gs][efxtypes]") {
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

TEST_CASE("a parameter-only edit never changes an EFX chain's shape", "[midi][sf2][gs][efxtypes]") {
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

TEST_CASE("EFX parameter translations move their insert control monotonically",
          "[midi][sf2][gs][efxtypes]") {
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
      // Read off the output stage: the level is the unit's, carried at the same
      // slot for every type, so it is not the drive block's parameter to hold.
      REQUIRE(
          json_number(stage_params(gs_efx_insert_chain(efx), "utility.gain"), "levelDb", level));
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

TEST_CASE("Tremolo realises as amplitude modulation, not as a ring modulator",
          "[midi][sf2][gs][efxtypes]") {
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
  REQUIRE_FALSE(chain.empty());
  REQUIRE(chain[0].name == "effects.modulation.ringModulator");
  const std::string modulator = stage_params(chain, "effects.modulation.ringModulator");
  double carrier = 0.0;
  double wet = 0.0;
  REQUIRE(json_number(modulator, "carrierHz", carrier));
  REQUIRE(json_number(modulator, "dryWet", wet));
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
  // The effect's own stages come first and the unit's output stage follows, so
  // the two modulation blocks are the head of the chain rather than all of it.
  REQUIRE(stage_names(tremolo_chorus).size() >= 2);
  REQUIRE(stage_names(tremolo_chorus)[0] == "effects.modulation.chorus");
  REQUIRE(stage_names(tremolo_chorus)[1] == "effects.modulation.ringModulator");
  const std::string chain_modulator =
      stage_params(tremolo_chorus, "effects.modulation.ringModulator");
  double chain_wet = 0.0;
  REQUIRE(json_number(chain_modulator, "dryWet", chain_wet));
  REQUIRE(chain_wet == wet);
  double chain_carrier = 0.0;
  REQUIRE(json_number(chain_modulator, "carrierHz", chain_carrier));
  REQUIRE(chain_carrier > 0.0);
  REQUIRE(chain_carrier < 20.0);
}

#if defined(SONARE_WITH_FX) && defined(SONARE_WITH_MASTERING)

TEST_CASE("the Tremolo voicing never inverts the phase it modulates", "[midi][sf2][gs][efxtypes]") {
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

TEST_CASE("an EFX type set over the wire reads back the same chain", "[midi][sf2][gs][efxtypes]") {
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

namespace {

/// The five forms named, so a failure says which classification was claimed
/// rather than which integer stands in the generated row.
std::string form_name(uint8_t form) {
  switch (form) {
    case s::kGsEfxJoinAssigned:
      return "assigned";
    case s::kGsEfxJoinState:
      return "a documented state";
    case s::kGsEfxJoinUnmapped:
      return "unmapped";
    case s::kGsEfxJoinBuilder:
      return "the skeleton's own";
    case s::kGsEfxJoinUnreadable:
      return "unreadable";
    default:
      return "an unknown form";
  }
}

}  // namespace

TEST_CASE("every adjudicated EFX byte does what its binding file says",
          "[midi][sf2][gs][efxtypes]") {
  Tally tally;

  // The binding table the chain walks and the join table read here are two
  // renderings of the same files by two generators, and nothing else compares
  // them. A pair in one and not the other is a row that reaches a control
  // nobody adjudicated, or an adjudication that reaches nothing.
  std::map<std::pair<uint16_t, int>, std::vector<const s::GsEfxBinding*>> bound;
  for (const s::GsEfxBinding& row : s::kGsEfxBindings) {
    bound[{row.type, static_cast<int>(row.slot)}].push_back(&row);
  }

  // What the archive measured a law for, so a row that assigns a law to a pair
  // the archive already read can be required to assign the one it read.
  std::map<std::pair<uint16_t, int>, const s::GsEfxSlotConversion*> measured;
  for (const s::GsEfxSlotConversion& entry : s::kGsEfxSlotConversions) {
    measured[{entry.type, static_cast<int>(entry.parameter)}] = &entry;
  }

  std::map<uint8_t, int> rows_by_form;
  std::map<uint16_t, int> state_by_type;
  int declared_keys = 0;
  int translated = 0;
  int against_measured = 0;

  for (const s::GsEfxJoinRow& row : s::kGsEfxJoin) {
    ++rows_by_form[row.form];
    const std::string label = hex4(row.type) + " slot " + std::to_string(row.slot);
    const std::pair<uint16_t, int> address = {row.type, static_cast<int>(row.slot)};
    const auto chain = gs_efx_insert_chain(make_efx(row.type));

    if (row.form != s::kGsEfxJoinAssigned) {
      tally.same(bound.count(address) == 0,
                 label + " is counted as " + form_name(row.form) +
                     " and yet the binding table drives a control from it");

      std::set<std::string> shapes;
      for (uint8_t value : kValues) {
        GsEfx efx = make_efx(row.type);
        efx.params[row.slot] = value;
        shapes.insert(signature(gs_efx_insert_chain(efx)));
      }

      // The skeleton's own rows are the one unassigned form whose byte moves:
      // the chain builder reads it and writes a control under a law of its own,
      // where the archive measured none for a binding row to name. One that
      // moved nothing would be a note about code that has gone away.
      if (row.form == s::kGsEfxJoinBuilder) {
        tally.same(shapes.size() > 1,
                   label + " is counted as the skeleton's own and moves nothing");
        continue;
      }

      // Not a note: the byte has to be inert. Were it reaching a control after
      // all, the row would be stale and the count wrong in the direction that
      // flatters it.
      tally.same(shapes.size() == 1,
                 label + " is counted as " + form_name(row.form) + " and yet moves its chain");

      // Inertness alone cannot separate the four unassigned forms, and without
      // that separation everything could be filed as whichever one is cheapest
      // to defend. What separates them is the chain: a type counted as unmapped
      // realises nothing at all, where a documented state is a byte one that
      // does play does not read.
      if (row.form == s::kGsEfxJoinUnmapped) {
        tally.same(chain.empty(), label + " is counted as unmapped and its type realises a chain");
      } else if (row.form == s::kGsEfxJoinState) {
        ++state_by_type[row.type];
        tally.same(!chain.empty(),
                   label + " is counted as a documented state and its type realises no chain");
      }
      continue;
    }

    const auto found = bound.find(address);
    tally.same(found != bound.end(),
               label + " is assigned and the binding table drives no control from it");
    if (found == bound.end()) continue;

    for (const s::GsEfxBinding* binding : found->second) {
      ++declared_keys;
      const std::string stage(s::kGsEfxBindingStages[binding->stage]);
      const std::string key(s::kGsEfxBindingKeys[binding->key]);
      const std::string what = label + " (" + stage + "." + key + ")";

      // The stage has to be in the chain exactly once, or reading a key off
      // "the" stage of that name is reading whichever one came first.
      int named = 0;
      for (const GsEfxStage& entry : chain) {
        if (entry.name == stage) ++named;
      }
      tally.same(named == 1, what + " does not name exactly one stage of the chain");

      // Translated is MEASURED: sweep the byte over its whole domain and
      // require the emitted value to be there every time and to move. A key
      // written at a constant is a key the wire cannot reach, and it reads
      // exactly like a translation to anything that greps for the key.
      std::set<double> emitted;
      bool always_present = true;
      for (int value = 0; value <= 127; ++value) {
        GsEfx efx = make_efx(row.type);
        efx.params[row.slot] = static_cast<uint8_t>(value);
        double number = 0.0;
        if (!json_number(stage_params(gs_efx_insert_chain(efx), stage), key, number)) {
          always_present = false;
          break;
        }
        emitted.insert(number);
      }
      tally.same(always_present, what + " does not emit its key at every byte value");
      tally.same(emitted.size() >= 2,
                 what + " emits its key at a constant, which is not a translation");
      if (always_present && emitted.size() >= 2) ++translated;

      // Where the archive read this pair itself, the law the row assigns has to
      // be the law the archive read. A row is free to name a law for a pair
      // nothing measured -- that is most of them -- but not to name a different
      // one for a pair that was measured. Rotary Multi is filed under the other
      // of its two type numbers in the archive, so both are looked up.
      auto reading = measured.find(address);
      if (reading == measured.end()) {
        reading = measured.find({s::gs_efx_alias_type(row.type), static_cast<int>(row.slot)});
      }
      if (reading == measured.end()) continue;
      ++against_measured;
      tally.same(reading->second->conversion_class == binding->conversion_class &&
                     reading->second->table == binding->table,
                 what + " assigns " + conversion_name(binding->conversion_class, binding->table) +
                     " where the archive measured " +
                     conversion_name(reading->second->conversion_class, reading->second->table));
    }
  }

  // The counts the files declare against the counts this run reached. Rendered
  // and measured are separate readings: a generator that dropped rows would
  // otherwise shrink both at once and report as a smaller clean run.
  tally.same(rows_by_form[s::kGsEfxJoinAssigned] == s::kGsEfxJoinAssignedRows,
             "the rendered assigned rows are not the count the header declares");
  tally.same(rows_by_form[s::kGsEfxJoinState] == s::kGsEfxJoinStateRows,
             "the rendered documented-state rows are not the count the header declares");
  tally.same(rows_by_form[s::kGsEfxJoinUnmapped] == s::kGsEfxJoinUnmappedRows,
             "the rendered unmapped rows are not the count the header declares");
  tally.same(declared_keys == s::kGsEfxJoinDeclaredKeys,
             "the binding table drives a different number of controls than the files declare");
  tally.same(static_cast<int>(s::kGsEfxBindings.size()) == s::kGsEfxJoinDeclaredKeys,
             "the two generators read a different number of controls out of the same files");

  tally.same(rows_by_form[s::kGsEfxJoinBuilder] == s::kGsEfxJoinBuilderRows,
             "the rendered skeleton-owned rows are not the count the header declares");

  // Nothing is filed as unreadable today. The form stays in the vocabulary
  // because retiring one is how a row with nowhere to go ends up filed as
  // something it is not; what is asserted is that it is not in use.
  tally.same(s::kGsEfxJoinUnreadableRows == 0, "a parameter is filed as unreadable");

  // Every pair the archive measured a law for is adjudicated. This is the one
  // direction the binding files cannot state about themselves: they enumerate
  // what someone looked at, not what there was to look at.
  for (const s::GsEfxSlotConversion& entry : s::kGsEfxSlotConversions) {
    const std::pair<uint16_t, int> address = {entry.type, static_cast<int>(entry.parameter)};
    const bool adjudicated =
        std::any_of(s::kGsEfxJoin.begin(), s::kGsEfxJoin.end(), [&](const s::GsEfxJoinRow& row) {
          return (row.type == address.first || s::gs_efx_alias_type(row.type) == address.first) &&
                 row.slot == address.second;
        });
    tally.same(adjudicated, hex4(entry.type) + " slot " + std::to_string(entry.parameter) +
                                " has a measured law and no binding file looked at it");
  }

  tally.same(translated == declared_keys, "a declared control is not reached by its byte");
  tally.same(translated >= kGsEfxTranslatedFloor, "the translated count fell below its floor");
  tally.same(static_cast<int>(s::kGsEfxJoin.size()) >= kGsEfxAdjudicatedFloor,
             "the adjudicated count fell below its floor");

  std::string breakdown;
  for (const auto& entry : state_by_type) {
    breakdown += hex4(entry.first) + ":" + std::to_string(entry.second) + " ";
  }
  WARN("adjudicated: " << s::kGsEfxJoin.size() << "  translated: " << translated
                       << "  state: " << rows_by_form[s::kGsEfxJoinState]
                       << "  unmapped: " << rows_by_form[s::kGsEfxJoinUnmapped]
                       << "  laws checked against the archive: " << against_measured);
  WARN("state by type: " << breakdown);
  WARN("comparisons: " << tally.count());
  REQUIRE(tally.count() >= 900);
}

TEST_CASE("the translation reads exactly the bytes the archive named",
          "[midi][sf2][gs][efxtypes]") {
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
  // Every read the binding files account for. A slot the archive measured no
  // table for still moves the chain where a binding row gives it one of the
  // measured laws -- the shared output stage is most of them -- so the binding
  // table rather than the archive's own reach is what this is drawn from.
  std::set<std::pair<uint16_t, int>> named;
  for (const s::GsEfxBinding& row : s::kGsEfxBindings) {
    named.insert({row.type, row.slot});
    // Rotary Multi answers to two type numbers and the binding files carry one
    // of them, so the other is named here rather than reading as an unexplained
    // set of reads.
    named.insert({s::gs_efx_alias_type(row.type), row.slot});
  }
  // The reads the chain skeleton owns: a byte it converts under a law of its
  // own, the archive having measured none for a binding row to name. Each is a
  // reviewed row with its reason, and together with the named set they are what
  // separates "reads a byte nothing measured" from "reads the wrong byte".
  std::set<std::pair<uint16_t, int>> excepted;
  for (const s::GsEfxJoinRow& row : s::kGsEfxJoin) {
    if (row.form == s::kGsEfxJoinBuilder) excepted.insert({row.type, row.slot});
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
  for (const auto& pair : excepted) {
    tally.same(moved.count(pair) == 1, hex4(pair.first) + " slot " + std::to_string(pair.second) +
                                           " is excused as the skeleton's own and is not read");
  }

  WARN("slots that move a chain: " << moved.size() << "  bound to a control: " << named.size()
                                   << "  owned by the skeleton: " << excepted.size());
  WARN("comparisons: " << tally.count());
  REQUIRE(tally.count() >= 120);
}

namespace {

/// The quantity a binding row's law reads out of one byte, derived here from
/// the generated table rather than from the chain builder.
///
/// This is a second reader on purpose. The chain builder writes a control only
/// where the hand-written translation for the type has not already written it,
/// so a binding row naming a different law from the branch beside it would be
/// silently outranked -- the control would carry the branch's value and the
/// table would be a claim nothing tested. Both sides are read here and required
/// to agree, which is also what makes the branches safe to retire.
bool law_reads(const s::GsEfxBinding& row, uint8_t byte, const std::string& key, double& out) {
  using s::GsFreqColumn;
  using s::GsRateRange;
  using s::GsTimeLadder;
  switch (row.conversion_class) {
    case s::kGsEfxClassRate:
      out = s::gs_efx_rate_hz(byte, row.table == 1 ? GsRateRange::kWide : GsRateRange::kNarrow);
      return true;
    case s::kGsEfxClassDelayTime: {
      const std::array<GsTimeLadder, 5> ladders = {GsTimeLadder::kLadder0, GsTimeLadder::kLadder1,
                                                   GsTimeLadder::kLadder2, GsTimeLadder::kLadder3,
                                                   GsTimeLadder::kLadder4};
      if (row.table >= ladders.size()) return false;
      out = s::gs_efx_delay_ms(byte, ladders[row.table]);
      return true;
    }
    case s::kGsEfxClassFreq: {
      const std::array<GsFreqColumn, 3> columns = {GsFreqColumn::kColumn0, GsFreqColumn::kColumn1,
                                                   GsFreqColumn::kColumn2};
      if (row.table >= columns.size()) return false;
      out = s::gs_efx_freq_hz(byte, columns[row.table]);
      return true;
    }
    case s::kGsEfxClassGain:
      out = s::gs_efx_gain_db(byte);
      return true;
    case s::kGsEfxClassLevel:
      // The dB wrapper the chain carries, over the measured multiplier's floor.
      out = std::max(-24.0, 20.0 * std::log10(static_cast<double>(s::gs_efx_level_mul(byte))));
      return true;
    case s::kGsEfxClassWidth:
      out = s::gs_efx_width_q(byte);
      return true;
    case s::kGsEfxClassAccel: {
      const bool hertz = key.size() > 2 && key.compare(key.size() - 2, 2, "Hz") == 0;
      out = hertz ? s::gs_efx_accel_undershoot_hz(byte) : s::gs_efx_accel_tau_s(byte);
      return true;
    }
    case s::kGsEfxClassRatio: {
      if (row.range >= s::kGsEfxBindingRanges.size()) return false;
      const s::GsEfxBindingRange& ends = s::kGsEfxBindingRanges[row.range];
      float percent = 0.0f;
      if (!s::gs_efx_ratio(byte, ends.lo_byte, ends.hi_byte, ends.lo_unit, ends.hi_unit,
                           &percent)) {
        return false;
      }
      out = static_cast<double>(percent) / 100.0;
      return true;
    }
    default:
      return false;
  }
}

}  // namespace

TEST_CASE("every binding row reaches its control carrying its own law's reading",
          "[midi][sf2][gs][efxtypes]") {
  Tally tally;
  int unread = 0;
  for (const s::GsEfxBinding& row : s::kGsEfxBindings) {
    const std::string stage(s::kGsEfxBindingStages[row.stage]);
    const std::string key(s::kGsEfxBindingKeys[row.key]);
    const std::string label =
        hex4(row.type) + " slot " + std::to_string(row.slot) + " -> " + stage + "." + key;
    for (uint8_t value : kValues) {
      GsEfx efx = make_efx(row.type);
      efx.params[row.slot] = value;
      const std::string params = stage_params(gs_efx_insert_chain(efx), stage);
      double carried = 0.0;
      if (!json_number(params, key, carried)) {
        tally.same(false, label + " names a control the chain does not carry");
        continue;
      }
      double expected = 0.0;
      if (!law_reads(row, value, key, expected)) {
        ++unread;
        continue;
      }
      // Rendered through std::to_string, so six decimal places is the width of
      // the comparison rather than a tolerance chosen for the quantity.
      tally.same(std::fabs(carried - expected) <= 5e-7 * std::max(1.0, std::fabs(expected)),
                 label + " at byte " + std::to_string(static_cast<int>(value)) + " carries " +
                     std::to_string(carried) + " where its law reads " + std::to_string(expected));
    }
  }
  tally.same(unread == 0, "every binding row's class has a reader here");
  WARN("binding rows checked: " << s::kGsEfxBindings.size() << "  comparisons: " << tally.count());
  REQUIRE(tally.count() >= static_cast<int>(s::kGsEfxBindings.size()));
}
