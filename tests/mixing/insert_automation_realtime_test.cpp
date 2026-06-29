/// @file insert_automation_realtime_test.cpp
/// @brief Tests for realtime PPQ automation of mixer-strip insert parameters
///        (track lane, master, and bus inserts) routed through the reserved
///        insert-automation id namespace.

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <memory>
#include <vector>

#include "engine/insert_automation_id.h"
#include "engine/realtime_engine.h"
#include "engine/track_mixer.h"
#include "mixing/channel_strip.h"
#include "rt/processor_base.h"

using Catch::Matchers::WithinAbs;
using sonare::engine::insert_param_index;
using sonare::engine::insert_param_param;
using sonare::engine::insert_param_strip;
using sonare::engine::is_insert_param_id;
using sonare::engine::kInsertStripBusBase;
using sonare::engine::kInsertStripMaster;
using sonare::engine::make_insert_param_id;

namespace {

// Insert processor whose param 0 ("gain") linearly scales the signal and whose
// most recently applied param value is observable. Recording the value at each
// process() call lets a test watch the per-sub-block smoother trajectory, not
// just the end state, so a one-pole glide is distinguishable from a hard step.
class ProbeGainProcessor final : public sonare::rt::ProcessorBase {
 public:
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    applied_at_process = gain_;
    ++process_count;
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] == nullptr) continue;
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] *= gain_;
      }
    }
  }
  void reset() override {}
  bool set_parameter(unsigned int param_id, float value) override {
    if (param_id != 0 || !std::isfinite(value)) return false;
    gain_ = value;
    last_set_value = value;
    ++set_count;
    return true;
  }
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override {
    return param_id == 0;
  }
  std::vector<sonare::rt::ParamDescriptor> parameter_descriptors() const override {
    return {{"gain", 0}};
  }

  float gain() const noexcept { return gain_; }

  // Value last pushed to set_parameter (the smoother output for this sub-block).
  float last_set_value = 0.0f;
  int set_count = 0;
  // Gain in effect when process() last ran, and how many times it has run.
  float applied_at_process = 1.0f;
  int process_count = 0;

 private:
  float gain_ = 1.0f;
};

sonare::engine::ClipSchedule dc_clip(uint32_t clip_id, uint32_t track_id,
                                     const float* const* samples, int channels, int frames) {
  sonare::engine::ClipSchedule clip{
      clip_id, {samples, channels, frames}, 0.0, 0, 0, frames, false, 1.0f, 0, 0};
  clip.track_id = track_id;
  return clip;
}

// Closed-form one-pole step response: starting from `start`, after `n` samples
// of a 5 ms smoother at `sample_rate` driven toward `target`. Mirrors
// rt::ParamSmoother so a test can assert the engine advanced the smoother by
// exactly one block's worth of samples (no over-advance).
float one_pole_after(float start, float target, int n, double sample_rate, float time_ms) {
  // coefficient = 1 - exp(-1 / (time_ms * 1e-3 * sample_rate)). This matches
  // util::time_to_attack_release_rate_f used by ParamSmoother.
  const double tau_samples = static_cast<double>(time_ms) * 1.0e-3 * sample_rate;
  const double coeff = 1.0 - std::exp(-1.0 / tau_samples);
  const double decay = std::pow(1.0 - coeff, static_cast<double>(n));
  return static_cast<float>(target + (start - target) * decay);
}

double block_energy(const float* data, int n) {
  double sum = 0.0;
  for (int i = 0; i < n; ++i) sum += static_cast<double>(data[i]) * data[i];
  return sum;
}

}  // namespace

TEST_CASE("Insert-automation ids round-trip strip/insert/param fields", "[mixing][automation]") {
  // Track lane selector.
  {
    const uint32_t id = make_insert_param_id(/*strip=*/5, /*insert=*/3, /*param=*/7);
    REQUIRE(is_insert_param_id(id));
    REQUIRE(insert_param_strip(id) == 5u);
    REQUIRE(insert_param_index(id) == 3u);
    REQUIRE(insert_param_param(id) == 7u);
  }
  // Master strip selector.
  {
    const uint32_t id = make_insert_param_id(kInsertStripMaster, 2, 0);
    REQUIRE(is_insert_param_id(id));
    REQUIRE(insert_param_strip(id) == kInsertStripMaster);
    REQUIRE(insert_param_index(id) == 2u);
    REQUIRE(insert_param_param(id) == 0u);
  }
  // Bus selector (bus N occupies kInsertStripBusBase - N).
  {
    const uint32_t selector = kInsertStripBusBase - 4u;
    const uint32_t id = make_insert_param_id(selector, 1, 255);
    REQUIRE(is_insert_param_id(id));
    REQUIRE(insert_param_strip(id) == selector);
    REQUIRE(insert_param_index(id) == 1u);
    REQUIRE(insert_param_param(id) == 255u);
  }
  // Field widths: each field is masked so the maximum value of one neighbour
  // never bleeds into another.
  {
    const uint32_t id = make_insert_param_id(0x1FFFu, 0xFFu, 0xFFu);
    REQUIRE(insert_param_strip(id) == 0x1FFFu);
    REQUIRE(insert_param_index(id) == 0xFFu);
    REQUIRE(insert_param_param(id) == 0xFFu);
  }
}

TEST_CASE("Insert-automation ids do not collide with the mixer fader namespace",
          "[mixing][automation]") {
  // The fader/pan/width namespace lives at 0x4D58xxxx (top bits 010); insert ids
  // live in the disjoint 111 octant. A fader id must never read as an insert id.
  constexpr uint32_t kEngineNamespace = 0x4D580000u;
  REQUIRE_FALSE(is_insert_param_id(kEngineNamespace | 0x0001u));        // a lane fader
  REQUIRE_FALSE(is_insert_param_id(kEngineNamespace | (0xFFu << 8u)));  // the master fader
  REQUIRE_FALSE(is_insert_param_id(kEngineNamespace | 0xFFFFu));
  // And a genuine insert id is recognised.
  REQUIRE(is_insert_param_id(make_insert_param_id(0, 0, 0)));
}

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)

TEST_CASE("Lane insert automation reaches the processor and smooths between breakpoints",
          "[mixing][automation]") {
  constexpr int kBlock = 256;
  constexpr double kSr = 48000.0;
  std::array<float, kBlock> source{};
  source.fill(1.0f);
  const float* channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(kSr, kBlock);
  player.set_clips({dc_clip(1, 10, channels, 1, kBlock)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(kSr, kBlock);
  REQUIRE(mixer.set_track_lanes({{10}}));

  auto* processor = new ProbeGainProcessor();
  sonare::mixing::ChannelStrip strip;
  strip.add_pre_insert(std::unique_ptr<sonare::rt::ProcessorBase>(processor));
  REQUIRE(mixer.bind_track_strip(10, &strip));

  // Resolve the JSON key the same way the engine does for an automation lane.
  size_t lane_index = 0;
  unsigned int param_id = 0;
  REQUIRE(mixer.resolve_track_insert_param(10, 0, "gain", &lane_index, &param_id));
  REQUIRE(lane_index == 0u);
  REQUIRE(param_id == 0u);

  // First breakpoint A = 0.25: the slot snaps to A on its first claim, so the
  // very first block already applies A (no glide from the reset 0).
  REQUIRE(mixer.route_lane_insert_param_smoothed(lane_index, 0, param_id, 0.25f));
  std::array<float, kBlock> out{};
  float* io[] = {out.data()};
  REQUIRE(mixer.render_clips(player, io, 1, kBlock, 0));
  REQUIRE_THAT(processor->applied_at_process, WithinAbs(0.25f, 1.0e-4f));

  // Jump to B = 0.75: the smoother glides over ~5 ms. The block right after the
  // jump must land strictly between A and B (smoothing engaged, not an instant
  // step), and never overshoot past B.
  REQUIRE(mixer.route_lane_insert_param_smoothed(lane_index, 0, param_id, 0.75f));
  out.fill(0.0f);
  REQUIRE(mixer.render_clips(player, io, 1, kBlock, 0));
  const float after_jump = processor->applied_at_process;
  REQUIRE(after_jump > 0.25f);
  REQUIRE(after_jump < 0.75f);

  // Several blocks later the smoother has settled at B.
  for (int b = 0; b < 8; ++b) {
    out.fill(0.0f);
    REQUIRE(mixer.render_clips(player, io, 1, kBlock, 0));
  }
  REQUIRE_THAT(processor->gain(), WithinAbs(0.75f, 1.0e-3f));
  REQUIRE_THAT(out.back(), WithinAbs(0.75f, 2.0e-3f));
}

TEST_CASE("Lane insert smoother advances exactly once per block (no double advance)",
          "[mixing][automation]") {
  // advance_insert_automations is invoked from three mutually-exclusive entry
  // points in track_mixer.cpp (render_clips / mix_source / begin_source_mix);
  // only one runs per sub-block. A single breakpoint jump driven block by block
  // must therefore track the closed-form one-pole response sample-for-sample. If
  // the smoother were advanced twice per block it would reach its target in half
  // the blocks, so the observed trajectory would run ahead of the analytic curve.
  constexpr int kBlock = 64;
  constexpr double kSr = 48000.0;
  std::array<float, kBlock> source{};
  source.fill(1.0f);
  const float* channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(kSr, kBlock);
  player.set_clips({dc_clip(1, 10, channels, 1, kBlock)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(kSr, kBlock);
  REQUIRE(mixer.set_track_lanes({{10}}));

  auto* processor = new ProbeGainProcessor();
  sonare::mixing::ChannelStrip strip;
  strip.add_pre_insert(std::unique_ptr<sonare::rt::ProcessorBase>(processor));
  REQUIRE(mixer.bind_track_strip(10, &strip));

  // Snap to A on first claim, render one block so A is in effect.
  REQUIRE(mixer.route_lane_insert_param_smoothed(0, 0, 0, 0.0f));
  std::array<float, kBlock> out{};
  float* io[] = {out.data()};
  REQUIRE(mixer.render_clips(player, io, 1, kBlock, 0));
  REQUIRE_THAT(processor->applied_at_process, WithinAbs(0.0f, 1.0e-6f));

  // Jump to 1.0 and advance block by block. The smoother output observed at each
  // process() must equal the closed-form one-pole evaluated at exactly
  // (block_index * kBlock) samples after the jump -- one block's advance apiece.
  REQUIRE(mixer.route_lane_insert_param_smoothed(0, 0, 0, 1.0f));
  float expected_start = 0.0f;
  for (int b = 1; b <= 12; ++b) {
    out.fill(0.0f);
    REQUIRE(mixer.render_clips(player, io, 1, kBlock, 0));
    const float expected = one_pole_after(expected_start, 1.0f, b * kBlock, kSr, 5.0f);
    INFO("block " << b << " observed " << processor->applied_at_process << " expected "
                  << expected);
    REQUIRE_THAT(processor->applied_at_process, WithinAbs(expected, 5.0e-4f));
  }
}

TEST_CASE("Bus insert automation reaches the bus processor", "[mixing][automation]") {
  constexpr int kBlock = 256;
  constexpr double kSr = 48000.0;
  std::array<float, kBlock> source{};
  source.fill(1.0f);
  const float* channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(kSr, kBlock);
  player.set_clips({dc_clip(1, 10, channels, 1, kBlock)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(kSr, kBlock);
  // The eq.parametric bus insert exposes band0.gainDb as an rt-safe key, so a
  // bus insert automation id resolves and reaches the FxBus processor.
  REQUIRE(mixer.set_buses({{1, 0.0f}}));
  sonare::engine::TrackLaneConfig lane{10};
  lane.output_bus_id = 1;
  REQUIRE(mixer.set_track_lanes({lane}));

  sonare::mixing::api::Bus bus;
  bus.id = "1";
  bus.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PreFader, "eq.parametric",
       R"({"band0.type":1,"band0.frequencyHz":1000,"band0.gainDb":0,"band0.enabled":1})"});
  REQUIRE(mixer.set_bus_strip(1, bus));

  size_t bus_index = 0;
  unsigned int param_id = 0;
  REQUIRE(mixer.resolve_bus_insert_param(1, 0, "band0.gainDb", &bus_index, &param_id));
  REQUIRE(bus_index == 0u);

  // Baseline at 0 dB.
  std::array<float, kBlock> flat{};
  float* flat_io[] = {flat.data()};
  for (int b = 0; b < 4; ++b) {
    flat.fill(0.0f);
    REQUIRE(mixer.render_clips(player, flat_io, 1, kBlock, 0));
  }
  const double flat_energy = block_energy(flat.data(), kBlock);

  // Boost band0 to +12 dB through the insert-automation path and let it settle.
  REQUIRE(mixer.route_bus_insert_param_smoothed(bus_index, 0, param_id, 12.0f));
  std::array<float, kBlock> boosted{};
  float* boosted_io[] = {boosted.data()};
  for (int b = 0; b < 12; ++b) {
    boosted.fill(0.0f);
    REQUIRE(mixer.render_clips(player, boosted_io, 1, kBlock, 0));
  }
  const double boosted_energy = block_energy(boosted.data(), kBlock);
  REQUIRE(boosted_energy > flat_energy * 1.5);
}

TEST_CASE("Invalid insert automation selectors are rejected before claiming slots",
          "[mixing][automation]") {
  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, 64);
  REQUIRE(mixer.set_track_lanes({{10}}));

  REQUIRE_FALSE(mixer.route_lane_insert_param_smoothed(
      sonare::engine::TrackMixerRuntime::kMaxTrackLanes, 0, 0, 0.5f));
  REQUIRE_FALSE(mixer.route_lane_insert_param_smoothed(0, 0, 0, 0.5f));
  REQUIRE_FALSE(mixer.route_bus_insert_param_smoothed(
      sonare::engine::TrackMixerRuntime::kMaxBusLanes, 0, 0, 0.5f));
  REQUIRE_FALSE(mixer.route_bus_insert_param_smoothed(0, 0, 0, 0.5f));
  REQUIRE(mixer.insert_automation_overflow_count() == 0u);
}

namespace {

// A scene-equivalent compressor strip for the master/bus reach tests driven
// through the full engine. The threshold defaults high so the baseline barely
// compresses; automating it down forces heavy gain reduction.
sonare::mixing::api::Strip compressor_strip(const char* id) {
  sonare::mixing::api::Strip strip;
  strip.id = id;
  sonare::mixing::api::Insert insert;
  insert.slot = sonare::mixing::api::InsertSlot::PreFader;
  insert.processor_name = "dynamics.compressor";
  insert.params_json = "{\"thresholdDb\":-3.0,\"ratio\":8.0,\"attackMs\":1.0,\"releaseMs\":20.0}";
  strip.inserts.push_back(insert);
  return strip;
}

sonare::automation::AutomationLane step_lane(uint32_t target_id, float value) {
  sonare::automation::AutomationLane lane(target_id);
  lane.set_points({{0.0, value, sonare::automation::CurveType::Hold}});
  return lane;
}

// Runs `blocks` blocks of a loud DC tone through the engine and sums the master
// output energy.
double run_engine_energy(sonare::engine::RealtimeEngine& engine, int block, int blocks) {
  double energy = 0.0;
  for (int b = 0; b < blocks; ++b) {
    std::array<float, 256> left{};
    std::array<float, 256> right{};
    left.fill(0.8f);
    right.fill(0.8f);
    float* io[] = {left.data(), right.data()};
    engine.process(io, 2, block);
    energy += block_energy(left.data(), block) + block_energy(right.data(), block);
  }
  return energy;
}

}  // namespace

TEST_CASE("Master insert automation lowers master energy through the engine",
          "[mixing][automation]") {
  constexpr int kBlock = 256;
  constexpr double kSr = 48000.0;

  sonare::engine::RealtimeEngine engine;
  engine.prepare(kSr, kBlock);
  REQUIRE(engine.set_master_strip(compressor_strip("master")));

  // Resolve the master compressor threshold to its reserved insert id and verify
  // it carries the master strip selector.
  const int64_t id = engine.resolve_master_insert_automation_id(0, "thresholdDb");
  REQUIRE(id >= 0);
  REQUIRE(is_insert_param_id(static_cast<uint32_t>(id)));
  REQUIRE(insert_param_strip(static_cast<uint32_t>(id)) == kInsertStripMaster);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  // Baseline: threshold left at -3 dB (barely compresses).
  const double baseline = run_engine_energy(engine, kBlock, 8);
  REQUIRE(baseline > 0.0);

  // Drop the threshold to -48 dB via a held automation lane; the master energy
  // must fall well below the baseline once the smoother settles.
  engine.automation().set_lanes({step_lane(static_cast<uint32_t>(id), -48.0f)});
  const double automated = run_engine_energy(engine, kBlock, 16);
  REQUIRE(automated < 0.5 * baseline);
}

TEST_CASE("Master insert automation slot is cleared when the master strip is replaced",
          "[mixing][automation]") {
  constexpr int kBlock = 256;
  constexpr double kSr = 48000.0;

  sonare::engine::RealtimeEngine engine;
  engine.prepare(kSr, kBlock);
  REQUIRE(engine.set_master_strip(compressor_strip("master")));

  const int64_t id = engine.resolve_master_insert_automation_id(0, "thresholdDb");
  REQUIRE(id >= 0);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  sonare::rt::Command set_threshold{};
  set_threshold.type = sonare::rt::CommandType::kSetParam;
  set_threshold.target_id = static_cast<uint32_t>(id);
  set_threshold.sample_time = -1;
  set_threshold.arg.f = -48.0f;
  REQUIRE(engine.push_command(set_threshold));

  REQUIRE(run_engine_energy(engine, kBlock, 4) > 0.0);

  // Replacing the master strip must drop the old smoothed insert target. If the
  // slot survives, the next render keeps forcing the fresh compressor threshold
  // down to -48 dB even though no automation lane or command is active.
  REQUIRE(engine.set_master_strip(compressor_strip("master")));
  const double after_replace = run_engine_energy(engine, kBlock, 8);

  sonare::engine::RealtimeEngine fresh;
  fresh.prepare(kSr, kBlock);
  REQUIRE(fresh.set_master_strip(compressor_strip("master")));
  REQUIRE(fresh.push_command(play));
  const double fresh_energy = run_engine_energy(fresh, kBlock, 8);

  REQUIRE(after_replace == Catch::Approx(fresh_energy).epsilon(0.05));
}

TEST_CASE("Bus insert automation lowers bus energy through the engine", "[mixing][automation]") {
  constexpr int kBlock = 256;
  constexpr double kSr = 48000.0;

  // Stable loud DC source the clip reads from for the whole test.
  std::array<float, kBlock> tone{};
  tone.fill(0.8f);
  const float* ch[] = {tone.data()};

  sonare::engine::RealtimeEngine engine;
  engine.prepare(kSr, kBlock);
  sonare::engine::ClipSchedule clip{1, {ch, 1, kBlock}, 0.0, 0, 0, kBlock, true, 1.0f, 0, 0};
  clip.track_id = 10;
  engine.set_clips({clip});
  REQUIRE(engine.set_track_buses({{1, 0.0f}}));
  // Route the lane's post-fader output to bus 1 via a 0 dB send so the bus
  // carries the lane signal into the master (the send path is the proven
  // lane->bus route). The compressor on the bus is what the automation drives.
  sonare::engine::TrackLaneConfig lane{10};
  lane.sends.push_back({1, 0.0f, true});
  REQUIRE(engine.set_track_lanes({lane}));
  REQUIRE(engine.set_bus_strip(1, [] {
    sonare::mixing::api::Bus bus;
    bus.id = "1";
    sonare::mixing::api::Insert insert;
    insert.slot = sonare::mixing::api::InsertSlot::PreFader;
    insert.processor_name = "dynamics.compressor";
    insert.params_json = "{\"thresholdDb\":-3.0,\"ratio\":8.0,\"attackMs\":1.0,\"releaseMs\":20.0}";
    bus.inserts.push_back(insert);
    return bus;
  }()));
  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  const int64_t id = engine.resolve_bus_insert_automation_id(1, 0, "thresholdDb");
  REQUIRE(id >= 0);
  REQUIRE(is_insert_param_id(static_cast<uint32_t>(id)));
  // The bus selector lives in the [kInsertStripBusBase - kMaxBusLanes, base] band.
  const uint32_t strip = insert_param_strip(static_cast<uint32_t>(id));
  REQUIRE(strip == kInsertStripBusBase);  // bus index 0

  auto run = [&]() {
    double energy = 0.0;
    for (int b = 0; b < 12; ++b) {
      std::array<float, kBlock> left{};
      float* io[] = {left.data()};
      engine.process(io, 1, kBlock);
      energy += block_energy(left.data(), kBlock);
    }
    return energy;
  };

  const double baseline = run();
  REQUIRE(baseline > 0.0);

  engine.automation().set_lanes({step_lane(static_cast<uint32_t>(id), -48.0f)});
  const double automated = run();
  REQUIRE(automated < 0.5 * baseline);
}

TEST_CASE("RealtimeEngine reports invalid insert automation id as unknown target",
          "[mixing][automation]") {
  constexpr int kBlock = 64;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  sonare::rt::Command invalid{};
  invalid.type = sonare::rt::CommandType::kSetParam;
  invalid.target_id = make_insert_param_id(
      static_cast<uint32_t>(sonare::engine::TrackMixerRuntime::kMaxTrackLanes), 0, 0);
  invalid.sample_time = -1;
  invalid.arg.f = 0.5f;
  REQUIRE(engine.push_command(invalid));

  std::array<float, kBlock> left{};
  float* io[] = {left.data()};
  engine.process(io, 1, kBlock);

  bool found = false;
  sonare::engine::Telemetry telemetry{};
  while (engine.pop_telemetry(telemetry)) {
    found = found || (telemetry.type == sonare::engine::TelemetryType::kError &&
                      telemetry.error == sonare::engine::TelemetryErrorCode::kUnknownTarget &&
                      telemetry.value == invalid.target_id);
  }
  REQUIRE(found);
}

TEST_CASE("Stale lane insert automation is a no-op after the strip is replaced",
          "[mixing][automation]") {
  constexpr int kBlock = 64;
  constexpr double kSr = 48000.0;
  std::array<float, kBlock> source{};
  source.fill(1.0f);
  const float* channels[] = {source.data()};

  // Only the surviving lane's track carries a clip, so the master output cleanly
  // reflects that lane's strip gain (no direct-to-master contribution from the
  // removed track).
  sonare::engine::ClipPlayer player;
  player.prepare(kSr, kBlock);
  player.set_clips({dc_clip(2, 20, channels, 1, kBlock)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(kSr, kBlock);
  REQUIRE(mixer.set_track_lanes({{10}}));

  auto* first = new ProbeGainProcessor();
  sonare::mixing::ChannelStrip first_strip;
  first_strip.add_pre_insert(std::unique_ptr<sonare::rt::ProcessorBase>(first));
  REQUIRE(mixer.bind_track_strip(10, &first_strip));
  REQUIRE(mixer.route_lane_insert_param_smoothed(0, 0, 0, 0.25f));

  // Republish the lane set with a different track id: the lane-change path clears
  // the insert-automation slots keyed to the removed lane.
  REQUIRE(mixer.set_track_lanes({{20}}));
  auto* second = new ProbeGainProcessor();
  sonare::mixing::ChannelStrip second_strip;
  second_strip.add_pre_insert(std::unique_ptr<sonare::rt::ProcessorBase>(second));
  REQUIRE(mixer.bind_track_strip(20, &second_strip));

  // Render: must not crash and the stale automation must not have driven the new
  // strip's gain (it stays at unity).
  std::array<float, kBlock> out{};
  float* io[] = {out.data()};
  REQUIRE(mixer.render_clips(player, io, 1, kBlock, 0));
  REQUIRE_THAT(second->gain(), WithinAbs(1.0f, 1.0e-6f));
  REQUIRE_THAT(out.back(), WithinAbs(1.0f, 1.0e-4f));

  // A fresh automation target on the new lane still works.
  REQUIRE(mixer.route_lane_insert_param_smoothed(0, 0, 0, 0.5f));
  for (int b = 0; b < 8; ++b) {
    out.fill(0.0f);
    REQUIRE(mixer.render_clips(player, io, 1, kBlock, 0));
  }
  REQUIRE_THAT(second->gain(), WithinAbs(0.5f, 1.0e-3f));
}

TEST_CASE("Insert automation slot table overflow is reported and existing slots survive",
          "[mixing][automation]") {
  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, 64);
  REQUIRE(mixer.set_track_lanes({{10}}));
  sonare::mixing::ChannelStrip strip;
  strip.add_pre_insert(std::make_unique<ProbeGainProcessor>());
  REQUIRE(mixer.bind_track_strip(10, &strip));

  // Claim distinct (insert, param) targets on lane 0 until the slot table is
  // full. The first request that fails marks the table at capacity; every
  // earlier request must have succeeded (no early starvation). The table holds
  // at most a few hundred slots, so this loop is bounded.
  size_t claimed = 0;
  bool overflowed = false;
  for (size_t i = 0; i < 4096 && !overflowed; ++i) {
    const bool ok = mixer.route_lane_insert_param_smoothed(
        0, static_cast<unsigned int>(i / 256), static_cast<unsigned int>(i % 256), 0.1f);
    if (ok) {
      ++claimed;
      REQUIRE(mixer.insert_automation_overflow_count() == 0u);
    } else {
      overflowed = true;
    }
  }
  REQUIRE(overflowed);
  REQUIRE(claimed > 0u);
  REQUIRE(mixer.insert_automation_overflow_count() == 1u);

  // A second distinct target while full overflows again (advisory counter
  // increments), and is safely rejected.
  REQUIRE_FALSE(mixer.route_lane_insert_param_smoothed(0, 0xFEu, 0xFEu, 0.2f));
  REQUIRE(mixer.insert_automation_overflow_count() == 2u);

  // An already-claimed target still resolves (retargets its existing slot) even
  // while the table is full: it is not rejected and does not bump the counter.
  REQUIRE(mixer.route_lane_insert_param_smoothed(0, 0, 0, 0.3f));
  REQUIRE(mixer.insert_automation_overflow_count() == 2u);
}

#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH
