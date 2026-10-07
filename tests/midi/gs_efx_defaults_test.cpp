/// @file gs_efx_defaults_test.cpp
/// @brief EFX power-on defaults: a type write (LSB of `40 03 01`) loads that
///        type's 20-byte default block, a parameter written before the type
///        is lost, and an MSB-only write changes nothing.
///
/// The twenty bytes are a function of the TYPE, so both write orders are pinned
/// rather than one: the order a bulk DT1 uses keeps the parameters that follow
/// the type, and the reverse order loses them. That second one is a cost and not
/// a defect -- it is what the machine does -- so it is recorded here rather than
/// worked around, and a change to it has to be an edit to this file.
///
/// Reach is an output: the case reports how many comparisons it made, so a
/// version that stopped reaching the types shows up as a count that fell rather
/// than as a run that looked exactly like a pass. One flat body rather than
/// sections, for the same reason -- a per-section count answers a question
/// nobody asked and hides the one that was.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>

#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_efx_tables.h"
#include "midi/synth/gs_layer.h"

namespace {

using sonare::midi::synth::apply_gs_efx_units_sysex;
using sonare::midi::synth::gs_efx_insert_chain;
using sonare::midi::synth::gs_efx_parameter_reset_default;
using sonare::midi::synth::gs_efx_power_on_params;
using sonare::midi::synth::gs_efx_type_defaults;
using sonare::midi::synth::GsEfx;
using sonare::midi::synth::GsEfxTypeDefaults;
using sonare::midi::synth::kGsEfxTypeDefaults;
using sonare::midi::synth::kGsEfxTypeThru;
using sonare::midi::synth::kGsEfxUnitCount;

/// The params of the one stage named @p name, or an empty string where the
/// chain carries no such stage.
std::string stage_params(const std::vector<sonare::midi::synth::GsEfxStage>& chain,
                         const std::string& name) {
  for (const auto& stage : chain) {
    if (stage.name == name) return stage.params_json;
  }
  return {};
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

/// Applies @p msg to a unit array holding @p efx at @p unit and reads that unit
/// back. Returns whether any byte reached a field; @p type_changed is the unit's
/// own bit of the type-change mask.
bool apply_to_unit(GsEfx& efx, const std::vector<uint8_t>& msg, bool* type_changed = nullptr,
                   size_t unit = 0) {
  std::array<GsEfx, kGsEfxUnitCount> units{};
  units[unit] = efx;
  uint32_t changed = 0;
  const bool touched = apply_gs_efx_units_sysex(units, msg.data(), msg.size(), &changed);
  efx = units[unit];
  if (type_changed != nullptr) *type_changed = ((changed >> unit) & 1u) != 0;
  return touched;
}

/// Writes the EFX TYPE as a file does: both bytes of `40 03 00` in one message.
bool write_type(GsEfx& efx, uint16_t type, bool* type_changed = nullptr) {
  return apply_to_unit(
      efx, dt1(0x400300, {static_cast<uint8_t>(type >> 8), static_cast<uint8_t>(type & 0x7Fu)}),
      type_changed);
}

/// Writes one EFX PARAMETER, numbered from 1 as the manual numbers them.
bool write_param(GsEfx& efx, unsigned parameter, uint8_t value) {
  return apply_to_unit(efx, dt1(0x400302u + parameter, {value}));
}

/// True when @p params is @p type's twenty power-on bytes.
bool is_block_of(const std::array<uint8_t, 20>& params, uint16_t type) {
  for (size_t slot = 0; slot < params.size(); ++slot) {
    if (params[slot] != gs_efx_parameter_reset_default(type, slot)) return false;
  }
  return true;
}

bool same_block(const std::array<uint8_t, 20>& a, const std::array<uint8_t, 20>& b) {
  for (size_t slot = 0; slot < a.size(); ++slot) {
    if (a[slot] != b[slot]) return false;
  }
  return true;
}

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

}  // namespace

TEST_CASE("an EFX type write loads that type's defaults at LSB resolution", "[gs-efx-defaults]") {
  Tally tally;

  // The block powers on holding the Thru type's own twenty bytes. Thru is not a
  // block of zeros: its output level powers up at unity, which is the reading
  // "byte 0 means unset" could not tell from an untouched unit.
  {
    const GsEfx efx;
    const std::array<uint8_t, 20> thru = gs_efx_power_on_params();
    tally.same(efx.type == kGsEfxTypeThru, "a fresh unit is Thru");
    tally.same(same_block(efx.params, thru), "a fresh unit holds the Thru block");
    tally.same(thru[19] == 127, "Thru powers up at unity output level");
  }

  // Every measured type loads its own twenty bytes over whatever stood.
  for (const GsEfxTypeDefaults& defaults : kGsEfxTypeDefaults) {
    GsEfx efx;
    efx.params.fill(0x2A);  // a byte no default is, so a survivor would be visible
    REQUIRE(write_type(efx, defaults.type));
    const std::string label = "type " + std::to_string(defaults.type);
    tally.same(efx.type == defaults.type, label + " resolved on its LSB");
    tally.same(same_block(efx.params, defaults.params), label + " loaded its own block");
  }

  // A bulk DT1 keeps the parameters that follow the type in the same message.
  // Address order is 00 MSB, 01 LSB, 02 reserved, 03.. PARAMETER 1 on, so the
  // defaults land at 01 and the three parameters after them overwrite.
  {
    GsEfx efx;
    const std::vector<uint8_t> msg = dt1(0x400300, {0x01, 0x50, 0x00, 0x11, 0x22, 0x33});
    REQUIRE(apply_to_unit(efx, msg));
    tally.same(efx.type == 0x0150, "the bulk write selected the delay");
    tally.same(efx.params[0] == 0x11, "PARAMETER 1 survived the load");
    tally.same(efx.params[1] == 0x22, "PARAMETER 2 survived the load");
    tally.same(efx.params[2] == 0x33, "PARAMETER 3 survived the load");
    // What the message did not reach is the type's own default, not zero and
    // not what the block held.
    tally.same(efx.params[3] == gs_efx_parameter_reset_default(0x0150, 3),
               "PARAMETER 4 is the delay's own default");
    tally.same(efx.params[19] == gs_efx_parameter_reset_default(0x0150, 19),
               "PARAMETER 20 is the delay's own default");
  }

  // A parameter written before its type is lost. That is the cost of resolving
  // on the type rather than remembering the byte, and it is the machine's.
  {
    GsEfx efx;
    REQUIRE(write_param(efx, 1, 0x11));
    tally.same(efx.params[0] == 0x11, "the parameter landed before the type arrived");
    REQUIRE(write_type(efx, 0x0150));
    tally.same(efx.params[0] == gs_efx_parameter_reset_default(0x0150, 0),
               "selecting the type overwrote the parameter written before it");
    tally.same(efx.params[0] != 0x11, "the byte written first did not survive");
  }

  // An MSB-only write resolves nothing, and the pair it would have made is not
  // a type the machine has.
  {
    GsEfx efx;
    REQUIRE(write_type(efx, 0x0150));
    const std::array<uint8_t, 20> delay = efx.params;

    bool type_changed = true;
    const std::vector<uint8_t> msb_only = dt1(0x400300, {0x04});
    REQUIRE(apply_to_unit(efx, msb_only, &type_changed));
    tally.same(efx.type == 0x0150, "the resolved type is still the delay");
    tally.same(!type_changed, "an MSB-only write is not a type change");
    tally.same(same_block(efx.params, delay), "an MSB-only write loaded no defaults");
    tally.same(gs_efx_type_defaults(0x0450) == nullptr, "(new MSB, old LSB) is not a type");

    const std::vector<uint8_t> lsb_only = dt1(0x400301, {0x01});
    REQUIRE(apply_to_unit(efx, lsb_only, &type_changed));
    tally.same(efx.type == 0x0401, "the LSB paired with the MSB written earlier");
    tally.same(type_changed, "the LSB is where the type changes");
    tally.same(is_block_of(efx.params, 0x0401), "the LSB loaded the paired type's block");
  }

  // An LSB write alone pairs with the MSB already held.
  {
    GsEfx efx;
    REQUIRE(write_type(efx, 0x0150));
    const std::vector<uint8_t> msg = dt1(0x400301, {0x57});
    REQUIRE(apply_to_unit(efx, msg));
    tally.same(efx.type == 0x0157, "the stored MSB carried into the new type");
    tally.same(is_block_of(efx.params, 0x0157), "the paired type loaded its own block");
  }

  // An extension unit resolves on its own LSB the same way. The extension's
  // rows are a second EFX PARAMETER row, so the type axis has to reach them or
  // fifteen of the sixteen units would keep the abolished reading.
  {
    GsEfx efx;
    const std::vector<uint8_t> msg = dt1(0x403700, {0x01, 0x50});
    REQUIRE(apply_to_unit(efx, msg, nullptr, 7));
    tally.same(efx.type == 0x0150, "an extension unit resolved its type");
    tally.same(is_block_of(efx.params, 0x0150), "an extension unit loaded the type's block");
  }

  // A run reaches a unit only at that unit's own 00-1F block: reserved offsets
  // are skipped, and a byte past 1F never lands in the block before it.
  {
    const auto same_state = [](const GsEfx& a, const GsEfx& b) {
      return a.type == b.type && a.type_msb == b.type_msb && a.params == b.params &&
             a.send_reverb == b.send_reverb && a.send_chorus == b.send_chorus &&
             a.send_delay == b.send_delay && a.control_source == b.control_source &&
             a.control_depth == b.control_depth && a.assigned == b.assigned;
    };
    GsEfx baseline;
    baseline.type = 0x0150;
    baseline.type_msb = 0x01;
    baseline.params.fill(0x2A);
    baseline.send_reverb = 0x11;
    baseline.send_chorus = 0x22;
    baseline.send_delay = 0x33;
    baseline.control_source = {0x44, 0x55};
    baseline.control_depth = {0x66, 0x77};
    baseline.assigned = false;

    // Each run ends on unit 1's TYPE: unit 0 keeps its state, unit 1 selects
    // the overdrive and is the only unit whose type changed.
    const auto check_rolls_into_unit1 = [&](const std::vector<uint8_t>& msg) {
      std::array<GsEfx, kGsEfxUnitCount> units{};
      units[0] = baseline;
      uint32_t changed = 0;
      REQUIRE(apply_gs_efx_units_sysex(units, msg.data(), msg.size(), &changed));
      REQUIRE(same_state(units[0], baseline));
      REQUIRE(units[1].type == 0x0110);
      REQUIRE(is_block_of(units[1].params, 0x0110));
      REQUIRE(units[1].assigned);
      REQUIRE(changed == (uint32_t{1} << 1));
    };

    // Starts in unit 0's reserved tail and rolls over into unit 1's TYPE.
    check_rolls_into_unit1(dt1(0x40307F, {0x00, 0x01, 0x10}));

    // A start outside 00-1F skips to the next block rather than refusing the run.
    {
      std::vector<uint8_t> data(18, 0x00);
      data[16] = 0x01;
      data[17] = 0x10;
      check_rolls_into_unit1(dt1(0x403070, data));
    }

    // 40 30 1E-1F carry no unit-0 field (EFX CONTROL is 40 03 only), and the
    // reserved tail after them is skipped on the way to unit 1.
    {
      std::vector<uint8_t> data(0x80 - 0x1E + 2, 0x00);
      data[0x80 - 0x1E] = 0x01;
      data[0x80 - 0x1E + 1] = 0x10;
      check_rolls_into_unit1(dt1(0x40301E, data));
    }

    // A long run from 40 03 1E keeps its depth write and drops bytes past 1F:
    // the next group is 40 04, not another unit.
    {
      std::vector<uint8_t> data(0x80 - 0x1E + 2, 0x00);
      data[0] = 0x12;
      data[1] = 0x01;  // SEND EQ SWITCH: in-range but intentionally ignored.
      data[0x80 - 0x1E] = 0x01;
      data[0x80 - 0x1E + 1] = 0x10;
      std::array<GsEfx, kGsEfxUnitCount> units{};
      units[0] = baseline;
      const std::vector<uint8_t> msg = dt1(0x40031E, data);
      REQUIRE(apply_gs_efx_units_sysex(units, msg.data(), msg.size()));
      REQUIRE(units[0].control_depth[1] == 0x12);
      REQUIRE(units[0].type == baseline.type);
      REQUIRE(units[0].type_msb == baseline.type_msb);
      REQUIRE(units[0].params == baseline.params);
      REQUIRE(units[0].assigned);
      for (size_t unit = 1; unit < units.size(); ++unit) REQUIRE(same_state(units[unit], GsEfx{}));
    }
  }

  // A zero byte is the value zero, in the block and in the translation. The
  // second half is what the abolished reading used to intercept: it answered a
  // zero with the insert's own default and emitted no key at all, so a file
  // asking for silence got unity and nothing downstream could tell.
  {
    GsEfx efx;
    REQUIRE(write_type(efx, 0x0110));
    tally.same(gs_efx_parameter_reset_default(0x0110, 19) != 0,
               "the overdrive's output level does not power up at zero");
    tally.same(stage_params(gs_efx_insert_chain(efx), "utility.gain").find("\"levelDb\"") !=
                   std::string::npos,
               "the overdrive's own default output level reaches the translation");

    REQUIRE(write_param(efx, 20, 0));
    tally.same(efx.params[19] == 0, "a written zero stands rather than reading as unset");
    // -24 dB is the floor the translation carries, so the silent byte lands on
    // it rather than being dropped.
    tally.same(stage_params(gs_efx_insert_chain(efx), "utility.gain").find("\"levelDb\":-24") !=
                   std::string::npos,
               "output level 0 translates to the floor rather than to no key");

    // Effect Balance had the same reading. The pitch shifter powers up at 48,
    // and a file writing 0 asks for all-direct rather than for nothing.
    GsEfx shifter;
    REQUIRE(write_type(shifter, 0x0160));
    REQUIRE(write_param(shifter, 16, 0));
    tally.same(stage_params(gs_efx_insert_chain(shifter), "effects.modulation.pitchShifter")
                       .find("\"dryWet\":0.000000") != std::string::npos,
               "effect balance 0 translates to all-direct rather than to no key");
  }

  // Both of the rotary multi's type numbers load the same block. The archive
  // files the measurement under 03 00 and the manual's appendix calls the same
  // effect 02 0C, so a lookup keyed by the number has to answer for either
  // spelling; selecting one and selecting the other are the same instruction.
  {
    GsEfx by_appendix;
    GsEfx by_body;
    REQUIRE(write_type(by_appendix, 0x020C));
    REQUIRE(write_type(by_body, 0x0300));
    tally.same(by_appendix.type == 0x020C, "the appendix's number resolved");
    tally.same(by_body.type == 0x0300, "the chapter body's number resolved");
    tally.same(same_block(by_appendix.params, by_body.params),
               "the two numbers for one effect load one block");
  }

  // Three types carry nineteen measured bytes rather than twenty, because one
  // comparison ran and refused. The placeholder is zero and the cleared bit is
  // the only thing separating it from a byte measured as zero.
  {
    int refused = 0;
    for (const GsEfxTypeDefaults& defaults : kGsEfxTypeDefaults) {
      for (size_t slot = 0; slot < defaults.params.size(); ++slot) {
        if ((defaults.measured & (1u << slot)) != 0) continue;
        ++refused;
        GsEfx efx;
        efx.params.fill(0x2A);
        REQUIRE(write_type(efx, defaults.type));
        tally.same(efx.params[slot] == 0, "type " + std::to_string(defaults.type) + " slot " +
                                              std::to_string(slot + 1) + " poured its placeholder");
      }
    }
    tally.same(refused == 3, "three slots across the whole table were refused");
  }

  WARN("comparisons: " << tally.count());
  REQUIRE(tally.count() >= 140);
}
