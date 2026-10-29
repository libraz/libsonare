/// @file part_fx_runtime_test.cpp
/// @brief A publish keeps the DSP state of every part chain and insertion unit
///        it does not change.
///
/// Each case drives two stages with the same input: one receives the edit, the
/// other (the control) does not, and the outputs of everything the edit did
/// not touch must match. Comparisons are on rendered audio, never on how many
/// processors were built.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "midi/part_rig.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/part_fx_stage.h"

#if defined(SONARE_MIDI_WITH_FX)

namespace {

namespace s = sonare::midi::synth;
using sonare::midi::kPartRigAllParts;
using sonare::midi::PartRig;
using sonare::midi::PartRigMode;
using sonare::midi::PartRigStage;

constexpr double kRate = 48000.0;
constexpr int kBlock = s::kPartFxChunkFrames;
constexpr uint16_t kStereoDelay = 0x0150;
constexpr uint16_t kOverdrive = 0x0110;
constexpr int kPartA = 0;
constexpr int kPartB = 1;
constexpr int kPartC = 2;
/// Blocks compared after an edit: past the GS delay types' power-on delay
/// times, so the units have returned several echoes.
constexpr int kTailBlocks = 200;
/// A stereo-delay slot whose byte reaches a realtime-safe control, so its edit
/// is queued rather than rebuilt.
constexpr uint8_t kRealtimeSlot = 2;

/// A framed GS DT1 write of @p value at 40 @p block @p offset, with the checksum.
std::array<uint8_t, 11> dt1(uint8_t block, uint8_t offset, uint8_t value) {
  std::array<uint8_t, 11> m = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, block, offset, value, 0x00, 0xF7};
  const uint32_t sum = m[5] + m[6] + m[7] + m[8];
  m[9] = static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu);
  return m;
}

/// An EFX type selection at 40 @p block 00 (03 is the spec unit, 31 unit 1).
std::array<uint8_t, 12> type_write(uint16_t type, uint8_t block = 0x03) {
  const auto msb = static_cast<uint8_t>(type >> 8);
  const auto lsb = static_cast<uint8_t>(type & 0x7F);
  std::array<uint8_t, 12> m = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                               block, 0x00, msb,  lsb,  0x00, 0xF7};
  const uint32_t sum = m[5] + m[6] + m[7] + m[8] + m[9];
  m[10] = static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu);
  return m;
}

/// Write of EFX PARAMETER @p slot + 1 on the spec unit.
std::array<uint8_t, 11> slot_write(uint8_t slot, uint8_t value) {
  return dt1(0x03, static_cast<uint8_t>(0x03 + slot), value);
}

PartRig chain(std::vector<PartRigStage> stages) {
  PartRig rig;
  rig.mode = PartRigMode::kChain;
  rig.stages = std::move(stages);
  return rig;
}

/// A long, bright feedback delay, so its tail is still ringing many blocks on.
PartRig delay_rig(int ms) {
  const std::string params = "{\"delayTimeLMs\":" + std::to_string(ms) +
                             ",\"delayTimeRMs\":" + std::to_string(ms) +
                             ",\"feedback\":0.7,\"dryWet\":0.5}";
  return chain({{"effects.delay.stereo", params}});
}

struct QuietHost final : s::PartFxHost {
  float part_controller_position(int, uint8_t) const noexcept override { return 0.0f; }
  float part_pan_units(int) const noexcept override { return 0.0f; }
};

/// Every controller held fully up, so an EFX CONTROL sourced from one moves its slot.
struct ModulatingHost final : s::PartFxHost {
  float part_controller_position(int, uint8_t) const noexcept override { return 1.0f; }
  float part_pan_units(int) const noexcept override { return 0.0f; }
};

s::PartFxStageConfig stage_config(s::GsEfxRealization realization = s::GsEfxRealization::kModern) {
  s::PartFxStageConfig cfg;
  cfg.bank_rig_binding = false;
  cfg.realization = realization;
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  return cfg;
}

/// One rendered block of a stage: every part bus and every unit bus, left legs
/// then right legs.
struct Block {
  std::array<std::vector<float>, 16> parts;
  std::array<std::vector<float>, s::kGsEfxUnitCount> units;
};

/// Renders one block the way a player does: adopt, settle, sum the input into
/// the part buses, run the part chains, merge routed parts into their units
/// and run the units. @p impulse_parts get a unit impulse at frame 0.
Block render_block(s::PartFxStage& fx, const std::vector<int>& impulse_parts, bool adopt = true,
                   const s::PartFxHost& host = QuietHost{}) {
  if (adopt) fx.acquire();
  fx.settle_block(host);
  fx.clear_part_buses();
  fx.clear_unit_buses();
  for (const int part : impulse_parts) fx.add_stereo(part, 0, 1.0f, 1.0f);
  const s::PartFxSnapshot* snap = fx.current();
  REQUIRE(snap != nullptr);
  fx.run_part_chains(kBlock, snap->part_bussed, snap->mono_prefix, host);
  for (int part = 0; part < 16; ++part) {
    const uint8_t unit = snap->part_unit[static_cast<size_t>(part)];
    if (unit == s::PartFxSnapshot::kNoUnit) continue;
    for (int i = 0; i < kBlock; ++i) {
      fx.unit_bus_l(unit)[i] += fx.bus_l(part)[i];
      fx.unit_bus_r(unit)[i] += fx.bus_r(part)[i];
    }
  }
  fx.run_units(kBlock, snap->unit_fed, nullptr);
  Block out;
  for (int part = 0; part < 16; ++part) {
    auto& v = out.parts[static_cast<size_t>(part)];
    v.assign(fx.bus_l(part), fx.bus_l(part) + kBlock);
    v.insert(v.end(), fx.bus_r(part), fx.bus_r(part) + kBlock);
  }
  for (size_t unit = 0; unit < s::kGsEfxUnitCount; ++unit) {
    auto& v = out.units[unit];
    v.assign(fx.unit_bus_l(unit), fx.unit_bus_l(unit) + kBlock);
    v.insert(v.end(), fx.unit_bus_r(unit), fx.unit_bus_r(unit) + kBlock);
  }
  return out;
}

float peak(const std::vector<float>& v) {
  float p = 0.0f;
  for (const float x : v) p = std::max(p, std::fabs(x));
  return p;
}

/// Largest sample difference over several blocks, relative to the control's peak.
struct Diff {
  float max_abs = 0.0f;
  float control_peak = 0.0f;
};

void accumulate(Diff& d, const std::vector<float>& got, const std::vector<float>& want) {
  REQUIRE(got.size() == want.size());
  for (size_t i = 0; i < got.size(); ++i) {
    d.max_abs = std::max(d.max_abs, std::fabs(got[i] - want[i]));
  }
  d.control_peak = std::max(d.control_peak, peak(want));
}

/// The two stages agree on the bus @p pick reads, for @p blocks blocks.
template <typename Pick>
Diff run_pair(s::PartFxStage& edited, s::PartFxStage& control, int blocks, Pick pick,
              const s::PartFxHost& host = QuietHost{}) {
  Diff d;
  for (int b = 0; b < blocks; ++b) {
    const Block e = render_block(edited, {}, true, host);
    const Block c = render_block(control, {}, true, host);
    accumulate(d, pick(e), pick(c));
  }
  return d;
}

void require_same(const Diff& d) {
  INFO("max abs diff " << d.max_abs << ", control peak " << d.control_peak);
  REQUIRE(d.control_peak > 1e-3f);  // Something was still ringing to compare.
  REQUIRE(d.max_abs <= 1e-6f * d.control_peak);
}

void require_different(const Diff& d) {
  INFO("max abs diff " << d.max_abs << ", control peak " << d.control_peak);
  REQUIRE(d.control_peak > 1e-3f);
  REQUIRE(d.max_abs > 1e-3f * d.control_peak);
}

template <size_t N>
bool control_sysex(s::PartFxStage& fx, const std::array<uint8_t, N>& m) {
  return fx.apply_control_sysex(m.data(), m.size());
}

/// Both stages start from the same state and render the same opening blocks,
/// with an impulse into @p impulse_parts on the first one.
void prime(s::PartFxStage& fx, const std::vector<int>& impulse_parts, int blocks = 4) {
  fx.publish();
  for (int b = 0; b < blocks; ++b) render_block(fx, b == 0 ? impulse_parts : std::vector<int>{});
}

/// Spec unit 0 holds a stereo delay; the parts in @p fed feed it.
void route_to_delay_unit(s::PartFxStage& fx, const std::vector<int>& fed) {
  for (const int part : fed) fx.assign_part(static_cast<uint8_t>(part), 1);
  const auto type = type_write(kStereoDelay);
  REQUIRE(fx.apply_unit_sysex(type.data(), type.size()));
}

}  // namespace

TEST_CASE("changing one part's rig keeps another part's chain tail", "[part-fx-runtime]") {
  s::PartFxStage edited(stage_config());
  s::PartFxStage control(stage_config());
  for (s::PartFxStage* fx : {&edited, &control}) {
    fx->prepare(kRate);
    REQUIRE(fx->set_part_rig(kPartA, delay_rig(20)));
    REQUIRE(fx->set_part_rig(kPartB, delay_rig(30)));
    prime(*fx, {kPartA});
  }
  REQUIRE(edited.set_part_rig_and_publish(kPartB, delay_rig(45), true));
  require_same(run_pair(edited, control, kTailBlocks, [](const Block& b) { return b.parts[kPartA]; }));
}

TEST_CASE("changing a part's rig keeps the tail of the unit it shares", "[part-fx-runtime]") {
  s::PartFxStage edited(stage_config());
  s::PartFxStage control(stage_config());
  for (s::PartFxStage* fx : {&edited, &control}) {
    fx->prepare(kRate);
    REQUIRE(fx->set_part_rig(kPartB, delay_rig(30)));
    route_to_delay_unit(*fx, {kPartA, kPartB});
    prime(*fx, {kPartA});
  }
  REQUIRE(edited.set_part_rig_and_publish(kPartB, delay_rig(45), true));
  require_same(run_pair(edited, control, kTailBlocks, [](const Block& b) { return b.units[0]; }));
}

TEST_CASE("changing a part's rig leaves a unit it does not feed alone", "[part-fx-runtime]") {
  s::PartFxStage edited(stage_config());
  s::PartFxStage control(stage_config());
  for (s::PartFxStage* fx : {&edited, &control}) {
    fx->prepare(kRate);
    REQUIRE(fx->set_part_rig(kPartB, delay_rig(30)));
    fx->assign_part(kPartA, 2);  // Extension unit 1.
    fx->assign_part(kPartB, 1);  // Spec unit 0.
    const auto unit1 = type_write(kStereoDelay, 0x31);
    REQUIRE(fx->apply_unit_sysex(unit1.data(), unit1.size()));
    const auto unit0 = type_write(kStereoDelay);
    REQUIRE(fx->apply_unit_sysex(unit0.data(), unit0.size()));
    prime(*fx, {kPartA});
  }
  REQUIRE(edited.set_part_rig_and_publish(kPartB, delay_rig(45), true));
  require_same(run_pair(edited, control, kTailBlocks, [](const Block& b) { return b.units[1]; }));
}

TEST_CASE("a destination default rebuilds only the parts without their own entry",
          "[part-fx-runtime]") {
  s::PartFxStage edited(stage_config());
  s::PartFxStage control(stage_config());
  for (s::PartFxStage* fx : {&edited, &control}) {
    fx->prepare(kRate);
    REQUIRE(fx->set_part_rig(kPartA, delay_rig(20)));
    REQUIRE(fx->set_part_rig(kPartRigAllParts, delay_rig(30)));
    prime(*fx, {kPartA, kPartC});
  }
  REQUIRE(edited.set_part_rig_and_publish(kPartRigAllParts, delay_rig(45), true));
  Diff own;
  Diff defaulted;
  for (int b = 0; b < kTailBlocks; ++b) {
    const Block e = render_block(edited, {});
    const Block c = render_block(control, {});
    accumulate(own, e.parts[kPartA], c.parts[kPartA]);
    accumulate(defaulted, e.parts[kPartC], c.parts[kPartC]);
  }
  require_same(own);
  // Part C takes the default, so its chain is the one rebuilt.
  require_different(defaulted);
}

TEST_CASE("assign, send and CONTROL edits keep the unit; a type change rebuilds it",
          "[part-fx-runtime]") {
  const auto assign_b = dt1(0x41 + kPartB, 0x22, 0x01);  // Part 2 joins the spec unit.
  const auto send = dt1(0x03, 0x17, 0x10);                // EFX -> reverb send.
  const auto source = dt1(0x03, 0x1B, 0x01);              // CONTROL 1 <- CC1.
  const auto type = type_write(kOverdrive);
  struct Edit {
    const char* name;
    const uint8_t* data;
    size_t size;
    bool keeps;
  };
  const Edit edits[] = {
      {"assign", assign_b.data(), assign_b.size(), true},
      {"send", send.data(), send.size(), true},
      {"control source", source.data(), source.size(), true},
      {"type", type.data(), type.size(), false},
  };
  for (const Edit& edit : edits) {
    DYNAMIC_SECTION(edit.name) {
      s::PartFxStage edited(stage_config());
      s::PartFxStage control(stage_config());
      for (s::PartFxStage* fx : {&edited, &control}) {
        fx->prepare(kRate);
        route_to_delay_unit(*fx, {kPartA});
        prime(*fx, {kPartA});
      }
      // Each of these needs a full publish; none is a parameter-only edit.
      REQUIRE(edited.apply_control_sysex(edit.data, edit.size));
      edited.publish();
      const Diff d = run_pair(edited, control, kTailBlocks, [](const Block& b) { return b.units[0]; });
      if (edit.keeps) {
        require_same(d);
      } else {
        require_different(d);
      }
    }
  }
}

TEST_CASE("a queued parameter edit survives publishes made before it is applied",
          "[part-fx-runtime]") {
  s::PartFxStage edited(stage_config());
  s::PartFxStage control(stage_config());
  for (s::PartFxStage* fx : {&edited, &control}) {
    fx->prepare(kRate);
    REQUIRE(fx->set_part_rig(kPartB, delay_rig(30)));
    route_to_delay_unit(*fx, {kPartA});
    prime(*fx, {kPartA});
  }
  const auto edit = slot_write(kRealtimeSlot, 0x10);
  REQUIRE_FALSE(control_sysex(control, edit));
  REQUIRE_FALSE(control_sysex(edited, edit));
  // Two publishes before the audio side adopts either, so they coalesce.
  REQUIRE(edited.set_part_rig_and_publish(kPartB, delay_rig(45), true));
  REQUIRE(edited.set_part_rig_and_publish(kPartB, delay_rig(60), true));
  require_same(run_pair(edited, control, kTailBlocks, [](const Block& b) { return b.units[0]; }));
}

TEST_CASE("an edit queued for a rebuilt unit waits until that unit is adopted",
          "[part-fx-runtime]") {
  s::PartFxStage edited(stage_config());
  s::PartFxStage control(stage_config());
  for (s::PartFxStage* fx : {&edited, &control}) {
    fx->prepare(kRate);
    fx->assign_part(kPartA, 1);
    const auto first = type_write(kOverdrive);
    REQUIRE(fx->apply_unit_sysex(first.data(), first.size()));
    prime(*fx, {});
    // The stereo delay replaces the overdrive: a rebuild of unit 0.
    const auto delay = type_write(kStereoDelay);
    REQUIRE(control_sysex(*fx, delay));
    fx->publish();
  }
  const auto edit = slot_write(kRealtimeSlot, 0x10);
  // The control adopts the rebuilt unit before the edit is queued.
  render_block(control, {kPartA});
  REQUIRE_FALSE(control_sysex(control, edit));
  // The edited stage is still running the overdrive snapshot when the edit,
  // addressed to the new unit, reaches the drain.
  REQUIRE_FALSE(control_sysex(edited, edit));
  edited.settle_block(QuietHost{});
  render_block(edited, {kPartA});
  require_same(run_pair(edited, control, kTailBlocks, [](const Block& b) { return b.units[0]; }));
}

TEST_CASE("clear_mirror and prepare still rebuild every chain", "[part-fx-runtime]") {
  for (const bool use_prepare : {false, true}) {
    DYNAMIC_SECTION((use_prepare ? "prepare" : "clear_mirror")) {
      s::PartFxStage fx(stage_config());
      fx.prepare(kRate);
      REQUIRE(fx.set_part_rig(kPartA, delay_rig(20)));
      prime(fx, {kPartA});
      float before = 0.0f;
      for (int b = 0; b < 16; ++b) before = std::max(before, peak(render_block(fx, {}).parts[kPartA]));
      REQUIRE(before > 1e-3f);
      if (use_prepare) {
        fx.prepare(kRate);
      } else {
        fx.clear_mirror();
      }
      fx.publish();
      fx.settle_quiescent(QuietHost{});
      float after = 0.0f;
      for (int b = 0; b < 16; ++b) after = std::max(after, peak(render_block(fx, {}).parts[kPartA]));
      REQUIRE(after == 0.0f);
    }
  }
}

TEST_CASE("a classic unit keeps its state across an unrelated rig change",
          "[part-fx-runtime]") {
  s::PartFxStage edited(stage_config(s::GsEfxRealization::kClassic));
  s::PartFxStage control(stage_config(s::GsEfxRealization::kClassic));
  for (s::PartFxStage* fx : {&edited, &control}) {
    fx->prepare(kRate);
    REQUIRE(fx->set_part_rig(kPartB, delay_rig(30)));
    route_to_delay_unit(*fx, {kPartA, kPartB});
    prime(*fx, {kPartA});
  }
  REQUIRE(edited.set_part_rig_and_publish(kPartB, delay_rig(45), true));
  require_same(run_pair(edited, control, kTailBlocks, [](const Block& b) { return b.units[0]; }));
}

TEST_CASE("a type change rebuilds that unit and keeps the others", "[part-fx-runtime]") {
  s::PartFxStage edited(stage_config());
  s::PartFxStage control(stage_config());
  for (s::PartFxStage* fx : {&edited, &control}) {
    fx->prepare(kRate);
    fx->assign_part(kPartA, 1);  // Spec unit 0.
    fx->assign_part(kPartC, 2);  // Extension unit 1.
    const auto unit0 = type_write(kStereoDelay);
    REQUIRE(fx->apply_unit_sysex(unit0.data(), unit0.size()));
    const auto unit1 = type_write(kStereoDelay, 0x31);
    REQUIRE(fx->apply_unit_sysex(unit1.data(), unit1.size()));
    prime(*fx, {kPartA, kPartC});
  }
  REQUIRE(control_sysex(edited, type_write(kOverdrive)));
  edited.publish();
  Diff rebuilt;
  Diff kept;
  for (int b = 0; b < kTailBlocks; ++b) {
    const Block e = render_block(edited, {});
    const Block c = render_block(control, {});
    accumulate(rebuilt, e.units[0], c.units[0]);
    accumulate(kept, e.units[1], c.units[1]);
  }
  require_different(rebuilt);
  require_same(kept);
}

TEST_CASE("a realisation switch rebuilds the unit", "[part-fx-runtime]") {
  s::PartFxStage edited(stage_config());
  s::PartFxStage control(stage_config());
  for (s::PartFxStage* fx : {&edited, &control}) {
    fx->prepare(kRate);
    route_to_delay_unit(*fx, {kPartA});
    prime(*fx, {kPartA});
  }
  edited.set_realization(s::GsEfxRealization::kClassic);
  edited.publish();
  require_different(run_pair(edited, control, kTailBlocks, [](const Block& b) { return b.units[0]; }));
}

TEST_CASE("a parameter byte that needs a publish rebuilds the unit", "[part-fx-runtime]") {
  // Find a stereo-delay slot whose edit the queue cannot carry.
  int slot = -1;
  for (int k = 0; k < 20 && slot < 0; ++k) {
    s::PartFxStage probe(stage_config());
    probe.prepare(kRate);
    route_to_delay_unit(probe, {kPartA});
    probe.publish();
    probe.acquire();
    const auto edit = slot_write(static_cast<uint8_t>(k), 0x05);
    const s::GsEfx before = probe.efx()[0];
    const bool rebuild = control_sysex(probe, edit);
    if (rebuild && probe.efx()[0].params != before.params) slot = k;
  }
  if (slot < 0) SKIP("every stereo-delay slot is carried by the queue");
  s::PartFxStage edited(stage_config());
  s::PartFxStage control(stage_config());
  for (s::PartFxStage* fx : {&edited, &control}) {
    fx->prepare(kRate);
    route_to_delay_unit(*fx, {kPartA});
    prime(*fx, {kPartA});
  }
  REQUIRE(control_sysex(edited, slot_write(static_cast<uint8_t>(slot), 0x05)));
  edited.publish();
  require_different(run_pair(edited, control, kTailBlocks, [](const Block& b) { return b.units[0]; }));
}

TEST_CASE("switching a kept unit's CONTROL source off puts its slot back at the base",
          "[part-fx-runtime]") {
  const ModulatingHost host;
  s::PartFxStage edited(stage_config());
  s::PartFxStage control(stage_config());
  s::PartFxStage modulated(stage_config());  // Left modulated: shows the slot is heard.
  for (s::PartFxStage* fx : {&edited, &control, &modulated}) {
    fx->prepare(kRate);
    route_to_delay_unit(*fx, {kPartA});
    const auto source = dt1(0x03, 0x1B, 0x01);  // CONTROL 1 <- CC1.
    REQUIRE(fx->apply_unit_sysex(source.data(), source.size()));
    const auto depth = dt1(0x03, 0x1C, 0x7F);  // Full positive modulation.
    REQUIRE(fx->apply_unit_sysex(depth.data(), depth.size()));
    fx->publish();
    for (int b = 0; b < 4; ++b) render_block(*fx, b == 0 ? std::vector<int>{kPartA} : std::vector<int>{}, true, host);
  }
  // Source off on one side, depth to the centre on the other: both leave the
  // slot at its base, so the kept units must agree from here on.
  REQUIRE(control_sysex(edited, dt1(0x03, 0x1B, 0x00)));
  edited.publish();
  REQUIRE(control_sysex(control, dt1(0x03, 0x1C, 0x40)));
  control.publish();
  Diff to_base;
  Diff to_modulated;
  for (int b = 0; b < kTailBlocks; ++b) {
    const Block e = render_block(edited, {}, true, host);
    const Block c = render_block(control, {}, true, host);
    const Block m = render_block(modulated, {}, true, host);
    accumulate(to_base, e.units[0], c.units[0]);
    accumulate(to_modulated, e.units[0], m.units[0]);
  }
  require_same(to_base);
  require_different(to_modulated);
}

#endif  // SONARE_MIDI_WITH_FX
