/// @file gs_layer_skeleton_test.cpp
/// @brief The EFX chain skeleton: which stages each type places, the branch and
///        ordinal every stage carries, how enable rows set a stage's flag, how a
///        binding row finds its stage, and the unit's CONTROL SOURCE storage.
///
/// Rows are hand-written and handed in through the GsEfxRowView overload, so
/// the skeleton is checked against rows that exist only here rather than
/// against whatever the binding files hold today. Stage and key names still
/// index the generated name tables, which is the one thing a row cannot carry.

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_efx_bindings.h"
#include "midi/synth/gs_efx_convert.h"
#include "midi/synth/gs_layer.h"

namespace {

using sonare::midi::synth::apply_gs_efx_sysex;
using sonare::midi::synth::gs_efx_binding_value;
using sonare::midi::synth::gs_efx_insert_chain;
using sonare::midi::synth::gs_efx_type_defaults;
using sonare::midi::synth::GsAddressEntry;
using sonare::midi::synth::GsEfx;
using sonare::midi::synth::GsEfxBindingRow;
using sonare::midi::synth::GsEfxDesignedLaw;
using sonare::midi::synth::GsEfxEnable;
using sonare::midi::synth::GsEfxOut;
using sonare::midi::synth::GsEfxRowView;
using sonare::midi::synth::GsEfxStage;
using sonare::midi::synth::GsLevel;
using sonare::midi::synth::kGsAddressTable;
using sonare::midi::synth::kGsEfxBranchBack;
using sonare::midi::synth::kGsEfxBranchFront;
using sonare::midi::synth::kGsEfxBranchHalfA;
using sonare::midi::synth::kGsEfxBranchHalfB;
using sonare::midi::synth::kGsEfxEnableSelect;
using sonare::midi::synth::kGsEfxEnableStages;
using sonare::midi::synth::kGsEfxFormLinear;
using sonare::midi::synth::kGsEfxFormNone;
using sonare::midi::synth::kGsEfxRowDesigned;
using sonare::midi::synth::kGsEfxRowKeys;
using sonare::midi::synth::kGsEfxRowStages;
using sonare::midi::synth::kGsEfxRowTranslated;

using Chain = std::vector<GsEfxStage>;

constexpr GsEfxRowView kNoRows{nullptr, 0, nullptr, 0};

/// Index of @p name in the generated stage-name table the rows point into.
uint16_t stage_id(std::string_view name) {
  for (size_t i = 0; i < kGsEfxRowStages.size(); ++i) {
    if (kGsEfxRowStages[i] == name) return static_cast<uint16_t>(i);
  }
  FAIL("stage " << name << " is not in kGsEfxRowStages");
  return 0;
}

/// Index of @p name in the generated key-name table the rows point into.
uint16_t key_id(std::string_view name) {
  for (size_t i = 0; i < kGsEfxRowKeys.size(); ++i) {
    if (kGsEfxRowKeys[i] == name) return static_cast<uint16_t>(i);
  }
  FAIL("key " << name << " is not in kGsEfxRowKeys");
  return 0;
}

/// A unit holding @p type with that type's power-on bytes, as selecting it leaves it.
GsEfx efx_of(uint16_t type) {
  GsEfx efx;
  efx.type = type;
  efx.type_msb = static_cast<uint8_t>(type >> 8);
  const auto* defaults = gs_efx_type_defaults(type);
  if (defaults != nullptr) efx.params = defaults->params;
  efx.assigned = true;
  return efx;
}

std::vector<std::string> names_of(const Chain& chain) {
  std::vector<std::string> out;
  for (const GsEfxStage& stage : chain) out.push_back(stage.name);
  return out;
}

/// The stage named @p name at @p ordinal, or null.
const GsEfxStage* find_stage(const Chain& chain, std::string_view name, uint8_t ordinal) {
  for (const GsEfxStage& stage : chain) {
    if (stage.name == name && stage.ordinal == ordinal) return &stage;
  }
  return nullptr;
}

bool carries(const GsEfxStage& stage, std::string_view fragment) {
  return stage.params_json.find(fragment) != std::string::npos;
}

/// The number a params object holds under @p key; fails where it holds none.
float number_at(const GsEfxStage& stage, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\":";
  const size_t at = stage.params_json.find(needle);
  REQUIRE(at != std::string::npos);
  return std::stof(stage.params_json.substr(at + needle.size()));
}

/// A designed row with a linear law over the whole byte range.
GsEfxBindingRow linear_row(uint16_t type, uint8_t slot, std::string_view stage, uint8_t ordinal,
                           std::string_view key, float lo, float hi) {
  GsEfxBindingRow row{};
  row.type = type;
  row.slot = slot;
  row.kind = kGsEfxRowDesigned;
  row.law = GsEfxDesignedLaw{kGsEfxFormLinear, lo, hi, 0};
  row.byte_lo = 0x00;
  row.byte_hi = 0x7F;
  row.out = GsEfxOut::kValue;
  row.stage = stage_id(stage);
  row.ordinal = ordinal;
  row.key = key_id(key);
  return row;
}

/// A switch row: the stages come on at byte 1 only.
GsEfxEnable switch_row(uint16_t type, uint8_t slot,
                       std::initializer_list<std::pair<std::string_view, uint8_t>> stages) {
  GsEfxEnable enable{};
  enable.type = type;
  enable.slot = slot;
  enable.mode = kGsEfxEnableStages;
  for (const auto& [name, ordinal] : stages) {
    enable.stages[enable.n_stages] = stage_id(name);
    enable.ordinals[enable.n_stages] = ordinal;
    ++enable.n_stages;
  }
  enable.on_mask[0] = 1u << 1;
  return enable;
}

/// A selector row: state i turns on the rule's i-th stage alone.
GsEfxEnable select_row(uint16_t type, uint8_t slot,
                       std::initializer_list<std::pair<std::string_view, uint8_t>> stages) {
  GsEfxEnable enable = switch_row(type, slot, stages);
  enable.mode = kGsEfxEnableSelect;
  enable.on_mask[0] = 0;
  return enable;
}

/// A GS DT1 message carrying @p data from address @p addr, with its checksum.
std::vector<uint8_t> dt1(uint32_t addr, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> msg{0xF0, 0x41, 0x10, 0x42, 0x12};
  unsigned sum = 0;
  for (int shift = 16; shift >= 0; shift -= 8) {
    const uint8_t byte = static_cast<uint8_t>((addr >> shift) & 0x7Fu);
    msg.push_back(byte);
    sum += byte;
  }
  for (uint8_t byte : data) {
    msg.push_back(static_cast<uint8_t>(byte & 0x7Fu));
    sum += byte & 0x7Fu;
  }
  msg.push_back(static_cast<uint8_t>((0x80u - (sum & 0x7Fu)) & 0x7Fu));
  msg.push_back(0xF7);
  return msg;
}

}  // namespace

TEST_CASE("the types the skeleton used to refuse realise a chain", "[midi][gs-skeleton]") {
  SECTION("Humanizer is the vowel filter") {
    const Chain chain = gs_efx_insert_chain(efx_of(0x0103), kNoRows);
    CHECK(names_of(chain) == std::vector<std::string>{"effects.filter.vowel"});
  }
  SECTION("3D Auto and 3D Manual are the binaural panner") {
    for (const uint16_t type : {uint16_t{0x0170}, uint16_t{0x0171}}) {
      INFO("type " << type);
      const Chain chain = gs_efx_insert_chain(efx_of(type), kNoRows);
      CHECK(names_of(chain) == std::vector<std::string>{"stereo.binaural"});
    }
  }
  SECTION("3D Chorus and 3D Delay end in the binaural panner") {
    CHECK(names_of(gs_efx_insert_chain(efx_of(0x0144), kNoRows)) ==
          std::vector<std::string>{"effects.modulation.chorus", "stereo.binaural"});
    CHECK(names_of(gs_efx_insert_chain(efx_of(0x0157), kNoRows)) ==
          std::vector<std::string>{"effects.delay.stereo", "stereo.binaural"});
  }
  SECTION("every parallel-2 type realises a chain") {
    for (uint16_t type = 0x1100; type <= 0x1108; ++type) {
      INFO("type " << type);
      CHECK_FALSE(gs_efx_insert_chain(efx_of(type), kNoRows).empty());
    }
  }
}

TEST_CASE("a parallel-2 type is two halves side by side, each ending in its own level and pan",
          "[midi][gs-skeleton]") {
  struct Expected {
    uint16_t type;
    std::vector<std::string> a;
    std::vector<std::string> b;
  };
  const std::string amp = "saturation.ampSim";
  // An OD Sel block: both pedals, then one amp per printed Amp Type state.
  const std::vector<std::string> od{
      "saturation.overdrive", "saturation.distortion", amp, amp, amp, amp};
  const std::vector<Expected> cases = {
      {0x1100, {"effects.modulation.chorus"}, {"effects.delay.stereo"}},
      {0x1101, {"effects.modulation.flanger"}, {"effects.delay.stereo"}},
      {0x1102, {"effects.modulation.chorus"}, {"effects.modulation.flanger"}},
      {0x1103, od, od},
      {0x1104, od, {"effects.modulation.rotary"}},
      {0x1105, od, {"effects.modulation.phaser"}},
      {0x1106, od, {"effects.modulation.autoWah"}},
      {0x1107, {"effects.modulation.phaser"}, {"effects.modulation.rotary"}},
      {0x1108, {"effects.modulation.phaser"}, {"effects.modulation.autoWah"}},
  };
  for (const Expected& expected : cases) {
    INFO("type " << expected.type);
    const Chain chain = gs_efx_insert_chain(efx_of(expected.type), kNoRows);
    std::vector<std::string> a = expected.a;
    a.insert(a.end(), {"utility.gain", "stereo.stereoBalance"});
    std::vector<std::string> b = expected.b;
    b.insert(b.end(), {"utility.gain", "stereo.stereoBalance"});
    REQUIRE(chain.size() == a.size() + b.size());

    std::vector<std::string> got_a;
    std::vector<std::string> got_b;
    for (size_t i = 0; i < chain.size(); ++i) {
      // Half A first, then half B: a stage's position says which half it is in.
      const uint8_t want = i < a.size() ? kGsEfxBranchHalfA : kGsEfxBranchHalfB;
      CHECK(static_cast<int>(chain[i].branch) == static_cast<int>(want));
      (i < a.size() ? got_a : got_b).push_back(chain[i].name);
      CHECK(chain[i].enabled);
    }
    CHECK(got_a == a);
    CHECK(got_b == b);

    // Each half owns one level stage and one raw constant-power pan stage.
    for (uint8_t ordinal = 0; ordinal < 2; ++ordinal) {
      const GsEfxStage* gain = find_stage(chain, "utility.gain", ordinal);
      const GsEfxStage* pan = find_stage(chain, "stereo.stereoBalance", ordinal);
      REQUIRE(gain != nullptr);
      REQUIRE(pan != nullptr);
      CHECK(static_cast<int>(gain->branch) ==
            (ordinal == 0 ? kGsEfxBranchHalfA : kGsEfxBranchHalfB));
      CHECK(carries(*pan, "\"law\":1"));
    }
  }

  SECTION("OD1/OD2 numbers its pedals and eight amps across both halves") {
    const Chain chain = gs_efx_insert_chain(efx_of(0x1103), kNoRows);
    const std::array<const char*, 4> presets = {
        "\"preset\":\"cleanCombo\"", "\"preset\":\"chimeEdge\"", "\"preset\":\"britStack\"",
        "\"preset\":\"rectifierChug\""};
    for (uint8_t ordinal = 0; ordinal < 8; ++ordinal) {
      INFO("ordinal " << static_cast<int>(ordinal));
      const GsEfxStage* stage = find_stage(chain, "saturation.ampSim", ordinal);
      REQUIRE(stage != nullptr);
      CHECK(static_cast<int>(stage->branch) ==
            (ordinal < 4 ? kGsEfxBranchHalfA : kGsEfxBranchHalfB));
      CHECK(carries(*stage, presets[ordinal % 4]));
    }
    for (uint8_t ordinal = 0; ordinal < 2; ++ordinal) {
      INFO("pedal ordinal " << static_cast<int>(ordinal));
      for (const char* pedal : {"saturation.overdrive", "saturation.distortion"}) {
        const GsEfxStage* stage = find_stage(chain, pedal, ordinal);
        REQUIRE(stage != nullptr);
        CHECK(static_cast<int>(stage->branch) ==
              (ordinal == 0 ? kGsEfxBranchHalfA : kGsEfxBranchHalfB));
      }
    }
  }
}

TEST_CASE("the skeleton places every stage a switch or selector can turn on",
          "[midi][gs-skeleton]") {
  SECTION("OD Sel: both pedals, then one amp per printed OD Amp state") {
    for (const uint16_t type :
         {uint16_t{0x0400}, uint16_t{0x0401}, uint16_t{0x0402}, uint16_t{0x0405}}) {
      INFO("type " << type);
      const Chain chain = gs_efx_insert_chain(efx_of(type), kNoRows);
      CHECK(find_stage(chain, "saturation.overdrive", 0) != nullptr);
      CHECK(find_stage(chain, "saturation.distortion", 0) != nullptr);
      const uint8_t amps = type == 0x0405 ? 3 : 4;
      for (uint8_t ordinal = 0; ordinal < amps; ++ordinal) {
        INFO("ordinal " << static_cast<int>(ordinal));
        const GsEfxStage* amp = find_stage(chain, "saturation.ampSim", ordinal);
        REQUIRE(amp != nullptr);
        // Each amp sits on its own cabinet; Amp Sw only takes it off.
        CHECK(carries(*amp, "\"preset\":"));
        CHECK(carries(*amp, "\"cabModel\":"));
      }
      CHECK(find_stage(chain, "saturation.ampSim", amps) == nullptr);
    }
  }
  SECTION("CF Sel: a chorus and a flanger") {
    for (uint16_t type = 0x0400; type <= 0x0406; ++type) {
      INFO("type " << type);
      const Chain chain = gs_efx_insert_chain(efx_of(type), kNoRows);
      CHECK(find_stage(chain, "effects.modulation.chorus", 0) != nullptr);
      CHECK(find_stage(chain, "effects.modulation.flanger", 0) != nullptr);
    }
  }
  SECTION("TP Sel: a tremolo and an auto-pan") {
    const Chain chain = gs_efx_insert_chain(efx_of(0x0406), kNoRows);
    CHECK(find_stage(chain, "effects.modulation.ringModulator", 0) != nullptr);
    CHECK(find_stage(chain, "stereo.autoPan", 0) != nullptr);
  }
  SECTION("series stages sit on the common front branch and start enabled") {
    for (const GsEfxStage& stage : gs_efx_insert_chain(efx_of(0x0400), kNoRows)) {
      CHECK(static_cast<int>(stage.branch) == kGsEfxBranchFront);
      CHECK(stage.enabled);
    }
  }
}

TEST_CASE("enable rows set each stage's flag from the byte", "[midi][gs-skeleton]") {
  SECTION("a switch turns its stage on at its on-states only") {
    const std::array<GsEfxEnable, 1> enables = {
        switch_row(0x0400, 3, {{"dynamics.compressor", 0}})};
    const GsEfxRowView view{nullptr, 0, enables.data(), enables.size()};
    GsEfx efx = efx_of(0x0400);
    efx.params[3] = 0;
    CHECK_FALSE(find_stage(gs_efx_insert_chain(efx, view), "dynamics.compressor", 0)->enabled);
    efx.params[3] = 1;
    CHECK(find_stage(gs_efx_insert_chain(efx, view), "dynamics.compressor", 0)->enabled);
  }
  SECTION("a selector turns on the one stage its state names") {
    const std::array<GsEfxEnable, 1> enables = {select_row(
        0x0400, 11, {{"effects.modulation.chorus", 0}, {"effects.modulation.flanger", 0}})};
    const GsEfxRowView view{nullptr, 0, enables.data(), enables.size()};
    GsEfx efx = efx_of(0x0400);
    for (uint8_t state = 0; state < 2; ++state) {
      INFO("state " << static_cast<int>(state));
      efx.params[11] = state;
      const Chain chain = gs_efx_insert_chain(efx, view);
      CHECK(find_stage(chain, "effects.modulation.chorus", 0)->enabled == (state == 0));
      CHECK(find_stage(chain, "effects.modulation.flanger", 0)->enabled == (state == 1));
      // A stage no rule names stays on.
      CHECK(find_stage(chain, "effects.delay.stereo", 0)->enabled);
    }
  }
  SECTION("a stage two rules name is on only where both say so") {
    // Rhodes Multi: TP Sw gates the pair, TP Sel picks one of it.
    const std::array<GsEfxEnable, 2> enables = {
        select_row(0x0406, 14, {{"effects.modulation.ringModulator", 0}, {"stereo.autoPan", 0}}),
        switch_row(0x0406, 18, {{"effects.modulation.ringModulator", 0}, {"stereo.autoPan", 0}})};
    const GsEfxRowView view{nullptr, 0, enables.data(), enables.size()};
    GsEfx efx = efx_of(0x0406);
    efx.params[14] = 1;
    efx.params[18] = 0;
    Chain chain = gs_efx_insert_chain(efx, view);
    CHECK_FALSE(find_stage(chain, "effects.modulation.ringModulator", 0)->enabled);
    CHECK_FALSE(find_stage(chain, "stereo.autoPan", 0)->enabled);
    efx.params[18] = 1;
    chain = gs_efx_insert_chain(efx, view);
    CHECK_FALSE(find_stage(chain, "effects.modulation.ringModulator", 0)->enabled);
    CHECK(find_stage(chain, "stereo.autoPan", 0)->enabled);
  }
  SECTION("an enable reaches the stage its ordinal names") {
    const std::array<GsEfxEnable, 1> enables = {
        select_row(0x1100, 0, {{"utility.gain", 0}, {"utility.gain", 1}})};
    const GsEfxRowView view{nullptr, 0, enables.data(), enables.size()};
    GsEfx efx = efx_of(0x1100);
    efx.params[0] = 1;
    const Chain chain = gs_efx_insert_chain(efx, view);
    CHECK_FALSE(find_stage(chain, "utility.gain", 0)->enabled);
    CHECK(find_stage(chain, "utility.gain", 1)->enabled);
  }
}

TEST_CASE("a binding row reaches the stage its name and ordinal name", "[midi][gs-skeleton]") {
  const std::array<GsEfxBindingRow, 2> rows = {
      linear_row(0x1100, 16, "utility.gain", 0, "levelDb", -10.0f, 10.0f),
      linear_row(0x1100, 18, "utility.gain", 1, "levelDb", -20.0f, 20.0f)};
  const GsEfxRowView view{rows.data(), rows.size(), nullptr, 0};
  GsEfx efx = efx_of(0x1100);
  efx.params[16] = 0x7F;
  efx.params[18] = 0x00;
  const Chain chain = gs_efx_insert_chain(efx, view);
  // Two same-named stages, one row each: neither row lands on the other's.
  CHECK(number_at(*find_stage(chain, "utility.gain", 0), "levelDb") == Catch::Approx(10.0f));
  CHECK(number_at(*find_stage(chain, "utility.gain", 1), "levelDb") == Catch::Approx(-20.0f));
  CHECK(chain.size() == 6);
}

TEST_CASE("a bound byte is written as the row evaluator reads it", "[midi][gs-skeleton]") {
  SECTION("a designed row writes its law's value") {
    const std::array<GsEfxBindingRow, 1> rows = {
        linear_row(0x0142, 5, "effects.modulation.chorus", 0, "feedback", 0.0f, 0.95f)};
    const GsEfxRowView view{rows.data(), rows.size(), nullptr, 0};
    for (const uint8_t byte : {uint8_t{0x00}, uint8_t{0x40}, uint8_t{0x7F}}) {
      INFO("byte " << static_cast<int>(byte));
      GsEfx efx = efx_of(0x0142);
      efx.params[5] = byte;
      const Chain chain = gs_efx_insert_chain(efx, view);
      CHECK(number_at(*find_stage(chain, "effects.modulation.chorus", 0), "feedback") ==
            Catch::Approx(gs_efx_binding_value(rows[0], byte)));
    }
  }
  SECTION("a bound byte replaces the skeleton's constant for the same key") {
    // The output stage a row creates carries the skeleton's fixed shelf corners.
    const std::array<GsEfxBindingRow, 1> rows = {
        linear_row(0x0142, 7, "eq.parametric", 0, "band0.frequencyHz", 100.0f, 400.0f)};
    const GsEfxRowView view{rows.data(), rows.size(), nullptr, 0};
    for (const uint8_t byte : {uint8_t{0x00}, uint8_t{0x7F}}) {
      INFO("byte " << static_cast<int>(byte));
      GsEfx efx = efx_of(0x0142);
      efx.params[7] = byte;
      const Chain chain = gs_efx_insert_chain(efx, view);
      const GsEfxStage* eq = find_stage(chain, "eq.parametric", 0);
      REQUIRE(eq != nullptr);
      CHECK(number_at(*eq, "band0.frequencyHz") ==
            Catch::Approx(gs_efx_binding_value(rows[0], byte)));
      const std::string& json = eq->params_json;
      CHECK(json.find("\"band0.frequencyHz\":") == json.rfind("\"band0.frequencyHz\":"));
    }
  }
  SECTION("a row whose stage the skeleton lacks appends it on the common back branch") {
    GsEfxBindingRow row{};
    row.type = 0x0142;
    row.slot = 19;
    row.kind = kGsEfxRowTranslated;
    row.conv_class = sonare::midi::synth::kGsEfxClassLevel;
    row.law = GsEfxDesignedLaw{kGsEfxFormNone, 0.0f, 0.0f, 0};
    row.out = GsEfxOut::kValue;
    row.stage = stage_id("utility.gain");
    row.key = key_id("levelDb");
    const std::array<GsEfxBindingRow, 1> rows = {row};
    const GsEfxRowView view{rows.data(), rows.size(), nullptr, 0};
    for (const uint8_t byte : {uint8_t{0x00}, uint8_t{0x7F}}) {
      INFO("byte " << static_cast<int>(byte));
      GsEfx efx = efx_of(0x0142);
      efx.params[19] = byte;
      const Chain chain = gs_efx_insert_chain(efx, view);
      const GsEfxStage* gain = find_stage(chain, "utility.gain", 0);
      REQUIRE(gain != nullptr);
      CHECK(static_cast<int>(gain->branch) == kGsEfxBranchBack);
      CHECK(number_at(*gain, "levelDb") == Catch::Approx(gs_efx_binding_value(row, byte)));
    }
  }
}

TEST_CASE("the unit holds its two control assignments", "[midi][gs-skeleton]") {
  SECTION("power-on values are the address rows' defaults, and those rows are audible") {
    const GsEfx efx;
    CHECK(efx.control_source == std::array<uint8_t, 2>{0x00, 0x00});
    CHECK(efx.control_depth == std::array<uint8_t, 2>{0x40, 0x40});
    int rows = 0;
    for (const GsAddressEntry& entry : kGsAddressTable) {
      if (entry.addr < 0x40031B || entry.addr > 0x40031E) continue;
      INFO("address " << entry.addr);
      ++rows;
      CHECK(entry.level == GsLevel::kAudible);
      const size_t control = (entry.addr - 0x40031B) / 2;
      const bool depth = ((entry.addr - 0x40031B) & 1u) != 0;
      CHECK(entry.def == (depth ? efx.control_depth[control] : efx.control_source[control]));
    }
    CHECK(rows == 4);
  }
  SECTION("each address writes its field and asks for a rebuild") {
    const std::array<uint32_t, 4> addrs = {0x40031B, 0x40031C, 0x40031D, 0x40031E};
    for (size_t i = 0; i < addrs.size(); ++i) {
      INFO("address " << addrs[i]);
      GsEfx efx = efx_of(0x0110);
      const std::vector<uint8_t> msg = dt1(addrs[i], {static_cast<uint8_t>(0x10 + i)});
      bool type_changed = true;
      CHECK(apply_gs_efx_sysex(efx, msg.data(), msg.size(), &type_changed));
      CHECK_FALSE(type_changed);
      const uint8_t held = (i % 2 == 0) ? efx.control_source[i / 2] : efx.control_depth[i / 2];
      CHECK(static_cast<int>(held) == static_cast<int>(0x10 + i));
    }
  }
  SECTION("one run writes all four, and a reset brings them back") {
    GsEfx efx;
    const std::vector<uint8_t> msg = dt1(0x40031B, {0x01, 0x7F, 0x60, 0x00});
    CHECK(apply_gs_efx_sysex(efx, msg.data(), msg.size()));
    CHECK(efx.control_source == std::array<uint8_t, 2>{0x01, 0x60});
    CHECK(efx.control_depth == std::array<uint8_t, 2>{0x7F, 0x00});
    // A GS reset replaces each unit with a value-initialised one.
    efx = GsEfx{};
    CHECK(efx.control_source == std::array<uint8_t, 2>{0x00, 0x00});
    CHECK(efx.control_depth == std::array<uint8_t, 2>{0x40, 0x40});
  }
  SECTION("the send EQ switch beside them still applies nothing") {
    GsEfx efx;
    const std::vector<uint8_t> msg = dt1(0x40031F, {0x00});
    CHECK_FALSE(apply_gs_efx_sysex(efx, msg.data(), msg.size()));
  }
}
