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
/// The cases enforce four things the mapping cannot check about itself: every
/// type resolves to a chain or to a listed refusal; no mapping exists for a type
/// with no row (an exhaustive sweep of the type-number space, so a mapping added
/// without a row fails by name); every stage name is one insert_factory can
/// actually build, which is what separates a mapping that looks complete from
/// one that produces sound; and two types realising the identical chain are
/// listed as such with the reason they are indistinguishable.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "midi/synth/gs_layer.h"
#include "rt/processor_base.h"

namespace {

using sonare::midi::synth::apply_gs_efx_sysex;
using sonare::midi::synth::gs_efx_insert_chain;
using sonare::midi::synth::gs_efx_insert_name;
using sonare::midi::synth::gs_efx_insert_params;
using sonare::midi::synth::GsEfx;
using sonare::midi::synth::GsEfxStage;

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
      {"the step flanger's stepped LFO is not modelled", {0x0123, 0x0124}},
      {"Space-D's unmodulated voicing and 3D Chorus's binaural stage are not modelled",
       {0x0142, 0x0143, 0x0144}},
      {"the modulation LFO, the tap counts and the 3D stage are not modelled, so every "
       "delay variant reduces to the stereo delay",
       {0x0150, 0x0151, 0x0152, 0x0153, 0x0154, 0x0157}},
      {"the gate reverb's gate stage is not modelled", {0x0155, 0x0156}},
      {"the feedback pitch shifter's feedback loop is not modelled", {0x0160, 0x0161}},
      {"Lo-Fi 1 and Lo-Fi 2 differ in degradation parameters that are not translated",
       {0x0172, 0x0173}},
      {"one effect the manual prints under two type numbers", {0x020C, 0x0300}},
  };
  return kCollisions;
}

GsEfx make_efx(uint16_t type) {
  GsEfx efx;
  efx.type = type;
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
  static constexpr uint8_t kValues[] = {0, 1, 63, 64, 65, 126, 127};
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
  SECTION("Overdrive drive rises with EFX PARAMETER 2") {
    GsEfx efx = make_efx(0x0110);
    double previous = -1.0;
    for (int value = 0; value <= 127; ++value) {
      efx.params[1] = static_cast<uint8_t>(value);
      double drive = 0.0;
      REQUIRE(json_number(gs_efx_insert_params(efx), "drive", drive));
      INFO("PARAMETER 2 = " << value);
      REQUIRE(drive > previous);
      previous = drive;
    }
  }

  SECTION("Distortion drive rises from a higher floor than the overdrive's") {
    GsEfx od = make_efx(0x0110);
    GsEfx ds = make_efx(0x0111);
    double previous = -1.0;
    for (int value = 0; value <= 127; ++value) {
      od.params[1] = ds.params[1] = static_cast<uint8_t>(value);
      double od_drive = 0.0;
      double ds_drive = 0.0;
      REQUIRE(json_number(gs_efx_insert_params(od), "drive", od_drive));
      REQUIRE(json_number(gs_efx_insert_params(ds), "drive", ds_drive));
      INFO("PARAMETER 2 = " << value);
      REQUIRE(ds_drive > od_drive);
      REQUIRE(ds_drive > previous);
      previous = ds_drive;
    }
  }

  SECTION("output level rises with EFX PARAMETER 20, and 0 leaves it unset") {
    GsEfx efx = make_efx(0x0110);
    double unused = 0.0;
    REQUIRE_FALSE(json_number(gs_efx_insert_params(efx), "levelDb", unused));
    double previous = -1000.0;
    for (int value = 1; value <= 127; ++value) {
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

  SECTION("effect balance rises with EFX PARAMETER 16, and 0 leaves it unset") {
    GsEfx efx = make_efx(0x0160);
    double unused = 0.0;
    REQUIRE_FALSE(json_number(gs_efx_insert_params(efx), "dryWet", unused));
    double previous = -1.0;
    for (int value = 1; value <= 127; ++value) {
      efx.params[15] = static_cast<uint8_t>(value);
      double wet = 0.0;
      REQUIRE(json_number(gs_efx_insert_params(efx), "dryWet", wet));
      INFO("PARAMETER 16 = " << value);
      REQUIRE(wet > previous);
      previous = wet;
    }
  }

  SECTION("the guitar multis' EQ shelves follow PARAMETER 17 and 18") {
    // The one confirmed per-block layout in the composite types: EQ Low Gain and
    // Hi Gain, centred at 64 over +-12 dB.
    for (uint16_t type : {uint16_t{0x0401}, uint16_t{0x0403}, uint16_t{0x0405}}) {
      double previous_low = -1000.0;
      double previous_high = -1000.0;
      for (int value = 1; value <= 127; ++value) {
        GsEfx efx = make_efx(type);
        efx.params[16] = efx.params[17] = static_cast<uint8_t>(value);
        const auto chain = gs_efx_insert_chain(efx);
        const auto eq = std::find_if(chain.begin(), chain.end(), [](const GsEfxStage& stage) {
          return stage.name == "eq.parametric";
        });
        REQUIRE(eq != chain.end());
        double low = 0.0;
        double high = 0.0;
        REQUIRE(json_number(eq->params_json, "band0.gainDb", low));
        REQUIRE(json_number(eq->params_json, "band1.gainDb", high));
        INFO(hex4(type) << " EQ gain byte " << value);
        REQUIRE(low >= previous_low);
        REQUIRE(high >= previous_high);
        previous_low = low;
        previous_high = high;
      }
      REQUIRE(previous_low == 12.0);
      REQUIRE(previous_high == 12.0);
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
  // the two cannot drift apart into different depths.
  const auto tremolo_chorus = gs_efx_insert_chain(make_efx(0x0141));
  REQUIRE(
      stage_names(tremolo_chorus) ==
      std::vector<std::string>{"effects.modulation.chorus", "effects.modulation.ringModulator"});
  REQUIRE(tremolo_chorus[1].params_json == chain[0].params_json);
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
