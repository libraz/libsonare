/// @file instrument_automation_test.cpp
/// @brief Realtime PPQ automation of hosted-instrument parameters routed
///        through the reserved instrument-automation id namespace.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "automation/automation_engine.h"
#include "engine/insert_automation_id.h"
#include "engine/instrument_automation_id.h"
#include "engine/realtime_engine.h"
#include "midi/instrument.h"
#include "midi/midi_event.h"
#include "midi/synth/native_synth.h"
#include "rt/command.h"

using sonare::engine::instrument_param_param;
using sonare::engine::instrument_param_slot;
using sonare::engine::is_instrument_param_id;
using sonare::engine::make_instrument_param_id;
using sonare::engine::RealtimeEngine;

namespace {

// Instrument exposing one automatable parameter ("level") that scales a DC
// output, so a test can observe the value the engine pushed per block.
class ProbeInstrument final : public sonare::midi::MidiInstrument {
 public:
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    applied_at_process.push_back(level_);
    for (int c = 0; c < num_channels; ++c) {
      for (int i = 0; i < num_samples; ++i) channels[c][i] += level_;
    }
  }
  void reset() override {}
  void on_event(uint32_t, const sonare::midi::MidiEvent&) noexcept override {}

  int parameter_id_for_key(const std::string& key) const noexcept override {
    if (key == "level") return 0;
    if (key == "tone") return 1;
    return -1;
  }
  bool apply_parameter(unsigned int param_id, float value) noexcept override {
    if (param_id == 0) {
      level_ = value;
      return true;
    }
    if (param_id == 1) {
      tone = value;
      return true;
    }
    return false;
  }

  float level() const noexcept { return level_; }
  float tone = 0.0f;
  std::vector<float> applied_at_process;

 private:
  float level_ = 0.0f;
};

sonare::automation::AutomationLane hold_lane(uint32_t target_id, float value) {
  sonare::automation::AutomationLane lane(target_id);
  lane.set_points({{0.0, value, sonare::automation::CurveType::Hold}});
  return lane;
}

void run_blocks(RealtimeEngine& engine, int block, int blocks) {
  std::array<float, 256> left{};
  std::array<float, 256> right{};
  for (int b = 0; b < blocks; ++b) {
    left.fill(0.0f);
    right.fill(0.0f);
    float* io[] = {left.data(), right.data()};
    engine.process(io, 2, block);
  }
}

}  // namespace

TEST_CASE("Instrument-automation ids round-trip slot and param fields", "[engine][automation]") {
  const uint32_t id = make_instrument_param_id(/*slot=*/5, /*param=*/7);
  REQUIRE(is_instrument_param_id(id));
  REQUIRE(instrument_param_slot(id) == 5u);
  REQUIRE(instrument_param_param(id) == 7u);

  // Field widths: the maximum of one field never bleeds into its neighbour.
  const uint32_t wide = make_instrument_param_id(0x1FFFu, 0xFFu);
  REQUIRE(instrument_param_slot(wide) == 0x1FFFu);
  REQUIRE(instrument_param_param(wide) == 0xFFu);
}

TEST_CASE("Instrument-automation ids are disjoint from the other reserved namespaces",
          "[engine][automation]") {
  constexpr uint32_t kEngineNamespace = 0x4D580000u;  // mixer fader/pan/width
  REQUIRE_FALSE(is_instrument_param_id(kEngineNamespace | 0x0001u));
  REQUIRE_FALSE(is_instrument_param_id(kEngineNamespace | 0xFFFFu));
  // Strip inserts live in the 111 octant; instrument params in 110.
  REQUIRE_FALSE(is_instrument_param_id(sonare::engine::make_insert_param_id(0, 0, 0)));
  REQUIRE_FALSE(
      is_instrument_param_id(sonare::engine::make_insert_param_id(0x1FFFu, 0xFFu, 0xFFu)));
  REQUIRE_FALSE(sonare::engine::is_insert_param_id(make_instrument_param_id(0, 0)));
  REQUIRE_FALSE(sonare::engine::is_insert_param_id(make_instrument_param_id(0x1FFFu, 0xFFu)));
  // Every reserved namespace is rejected as a host-usable graph parameter id.
  REQUIRE(RealtimeEngine::parameter_target_reserved(make_instrument_param_id(3, 1)));
}

TEST_CASE("Instrument parameters resolve only for automatable keys", "[engine][automation]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 256);

  // No instrument bound yet.
  REQUIRE(engine.resolve_instrument_automation_id(0, "level") < 0);

  ProbeInstrument instrument;
  REQUIRE(engine.set_midi_instrument(7, &instrument));

  const int64_t level_id = engine.resolve_instrument_automation_id(7, "level");
  REQUIRE(level_id >= 0);
  REQUIRE(is_instrument_param_id(static_cast<uint32_t>(level_id)));
  REQUIRE(instrument_param_param(static_cast<uint32_t>(level_id)) == 0u);

  const int64_t tone_id = engine.resolve_instrument_automation_id(7, "tone");
  REQUIRE(tone_id >= 0);
  REQUIRE(instrument_param_param(static_cast<uint32_t>(tone_id)) == 1u);
  // Same destination -> same slot, so the two ids differ only in the param field.
  REQUIRE(instrument_param_slot(static_cast<uint32_t>(tone_id)) ==
          instrument_param_slot(static_cast<uint32_t>(level_id)));

  REQUIRE(engine.resolve_instrument_automation_id(7, "notAParam") < 0);
  // A destination with no instrument stays unresolvable.
  REQUIRE(engine.resolve_instrument_automation_id(9, "level") < 0);

  engine.set_midi_instrument(7, nullptr);
}

TEST_CASE("Distinct destinations get distinct automation slots", "[engine][automation]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 256);
  ProbeInstrument a;
  ProbeInstrument b;
  REQUIRE(engine.set_midi_instrument(1, &a));
  REQUIRE(engine.set_midi_instrument(2, &b));

  const int64_t id_a = engine.resolve_instrument_automation_id(1, "level");
  const int64_t id_b = engine.resolve_instrument_automation_id(2, "level");
  REQUIRE(id_a >= 0);
  REQUIRE(id_b >= 0);
  REQUIRE(instrument_param_slot(static_cast<uint32_t>(id_a)) !=
          instrument_param_slot(static_cast<uint32_t>(id_b)));

  // Resolving the same destination again returns the same id (a lane saved
  // earlier keeps matching after a reload).
  REQUIRE(engine.resolve_instrument_automation_id(1, "level") == id_a);

  engine.set_midi_instrument(1, nullptr);
  engine.set_midi_instrument(2, nullptr);
}

TEST_CASE("An instrument-parameter lane drives the instrument at block precision",
          "[engine][automation]") {
  constexpr int kBlock = 256;
  RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  ProbeInstrument instrument;
  REQUIRE(engine.set_midi_instrument(0, &instrument));
  const int64_t id = engine.resolve_instrument_automation_id(0, "level");
  REQUIRE(id >= 0);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  engine.automation().set_lanes({hold_lane(static_cast<uint32_t>(id), 0.25f)});
  run_blocks(engine, kBlock, 8);

  // The first observed value snaps to the lane's target rather than gliding up
  // from zero, and it stays there.
  REQUIRE(instrument.level() == 0.25f);
  REQUIRE_FALSE(instrument.applied_at_process.empty());

  // A second lane value follows within a few blocks.
  engine.automation().set_lanes({hold_lane(static_cast<uint32_t>(id), 0.75f)});
  run_blocks(engine, kBlock, 8);
  REQUIRE(std::abs(instrument.level() - 0.75f) < 1.0e-3f);

  engine.set_midi_instrument(0, nullptr);
}

TEST_CASE("An instrument automation id routes through the one-shot parameter path",
          "[engine][automation]") {
  // The id is documented as usable both from a PPQ lane and from a direct
  // set_parameter. The lane path is covered above; this drives the command
  // path, which reaches the instrument through a separate dispatch site.
  constexpr int kBlock = 256;
  RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  ProbeInstrument instrument;
  REQUIRE(engine.set_midi_instrument(4, &instrument));
  const int64_t id = engine.resolve_instrument_automation_id(4, "level");
  REQUIRE(id >= 0);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  sonare::rt::Command set{};
  set.type = sonare::rt::CommandType::kSetParam;
  set.target_id = static_cast<uint32_t>(id);
  set.arg.f = 0.5f;
  set.sample_time = -1;
  REQUIRE(engine.push_command(set));
  run_blocks(engine, kBlock, 2);
  REQUIRE(instrument.level() == 0.5f);

  sonare::rt::Command smoothed{};
  smoothed.type = sonare::rt::CommandType::kSetParamSmoothed;
  smoothed.target_id = static_cast<uint32_t>(id);
  smoothed.arg.f = 0.125f;
  smoothed.sample_time = -1;
  REQUIRE(engine.push_command(smoothed));
  run_blocks(engine, kBlock, 16);
  REQUIRE(std::abs(instrument.level() - 0.125f) < 1.0e-3f);

  engine.set_midi_instrument(4, nullptr);
}

TEST_CASE("An unbound destination makes its automation lane inert, not dangling",
          "[engine][automation]") {
  constexpr int kBlock = 256;
  RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  ProbeInstrument instrument;
  REQUIRE(engine.set_midi_instrument(3, &instrument));
  const int64_t id = engine.resolve_instrument_automation_id(3, "level");
  REQUIRE(id >= 0);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));
  engine.automation().set_lanes({hold_lane(static_cast<uint32_t>(id), 0.5f)});
  run_blocks(engine, kBlock, 4);
  REQUIRE(instrument.level() == 0.5f);

  // Unbind: the lane keeps firing but reaches nothing, and the id still names
  // the same destination afterwards.
  engine.set_midi_instrument(3, nullptr);
  run_blocks(engine, kBlock, 4);
  REQUIRE(engine.resolve_instrument_automation_id(3, "level") < 0);

  REQUIRE(engine.set_midi_instrument(3, &instrument));
  REQUIRE(engine.resolve_instrument_automation_id(3, "level") == id);
}

TEST_CASE("NativeSynth exposes its continuous patch fields and rejects structural ones",
          "[engine][automation]") {
  using sonare::midi::synth::NativeSynth;
  using sonare::midi::synth::NativeSynthParamId;

  NativeSynth synth;
  REQUIRE(synth.parameter_id_for_key("cutoffHz") ==
          static_cast<int>(NativeSynthParamId::kCutoffHz));
  REQUIRE(synth.parameter_id_for_key("gain") == static_cast<int>(NativeSynthParamId::kGain));
  REQUIRE(synth.parameter_id_for_key("pitchOffsetCents") ==
          static_cast<int>(NativeSynthParamId::kPitchOffsetCents));

  // Structural fields are not automatable: they resize voice pools or swap DSP
  // topology, neither of which is audio-thread safe.
  REQUIRE(synth.parameter_id_for_key("engineMode") == -1);
  REQUIRE(synth.parameter_id_for_key("waveform") == -1);
  REQUIRE(synth.parameter_id_for_key("unison") == -1);
  REQUIRE(synth.parameter_id_for_key("polyphony") == -1);
  REQUIRE(synth.parameter_id_for_key("preset") == -1);
  REQUIRE(synth.parameter_id_for_key("modRoutings") == -1);
  REQUIRE(synth.parameter_id_for_key("body") == -1);

  // Every enumerated name resolves, and the values are clamped on apply.
  for (size_t i = 0; i < sonare::midi::synth::native_synth_param_count(); ++i) {
    const char* name = sonare::midi::synth::native_synth_param_name_at(i);
    REQUIRE(name != nullptr);
    REQUIRE(synth.parameter_id_for_key(name) >= 0);
  }

  REQUIRE(synth.apply_parameter(static_cast<unsigned int>(NativeSynthParamId::kCutoffHz), 1.0e9f));
  REQUIRE(synth.patch().cutoff_hz == 22000.0f);
  REQUIRE(synth.apply_parameter(static_cast<unsigned int>(NativeSynthParamId::kCutoffHz), -5.0f));
  REQUIRE(synth.patch().cutoff_hz == 10.0f);
  // A non-finite value leaves the previous setting standing rather than
  // poisoning the patch.
  REQUIRE(synth.apply_parameter(static_cast<unsigned int>(NativeSynthParamId::kCutoffHz),
                                std::numeric_limits<float>::quiet_NaN()));
  REQUIRE(synth.patch().cutoff_hz == 10.0f);
  // An id past the table is rejected.
  REQUIRE_FALSE(synth.apply_parameter(9999u, 1.0f));
}

TEST_CASE("An instrument swap retires the previous instrument's automation slots",
          "[engine][automation]") {
  constexpr int kBlock = 256;
  RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  ProbeInstrument first;
  ProbeInstrument second;
  REQUIRE(engine.set_midi_instrument(4, &first));
  const int64_t id = engine.resolve_instrument_automation_id(4, "level");
  REQUIRE(id >= 0);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));
  engine.automation().set_lanes({hold_lane(static_cast<uint32_t>(id), 0.5f)});
  run_blocks(engine, kBlock, 4);
  REQUIRE(first.level() == 0.5f);

  // Swapping instruments must not push the outgoing instrument's in-flight
  // value into the incoming one before the host re-resolves and re-drives.
  engine.automation().set_lanes({});
  REQUIRE(engine.set_midi_instrument(4, &second));
  run_blocks(engine, kBlock, 4);
  REQUIRE(second.level() == 0.0f);

  engine.set_midi_instrument(4, nullptr);
}
