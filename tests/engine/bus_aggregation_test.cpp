/// @file bus_aggregation_test.cpp
/// @brief Block-level bus aggregation: a group bus with both clip and hosted
///        instrument contributors runs its insert chain once, over the summed
///        bus signal, rather than once per contributor.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "engine/realtime_engine.h"
#include "engine/track_mixer.h"
#include "midi/instrument.h"
#include "midi/midi_event.h"
#include "mixing/api/scene.h"
#include "rt/command.h"

namespace {

using sonare::engine::RealtimeEngine;

// Emits a constant level on every channel, so a block's instrument
// contribution to a bus is exactly known. Reports zero latency, keeping the
// engine on the non-PDC path where clips and instruments share one bus pass.
class DcInstrument final : public sonare::midi::MidiInstrument {
 public:
  explicit DcInstrument(float level) : level_(level) {}
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    for (int c = 0; c < num_channels; ++c) {
      if (!channels[c]) continue;
      for (int i = 0; i < num_samples; ++i) channels[c][i] += level_;
    }
  }
  void reset() override {}
  void on_event(uint32_t, const sonare::midi::MidiEvent&) noexcept override {}

 private:
  float level_ = 0.0f;
};

// A group bus whose only insert is a hard-working compressor: strongly
// non-linear, so applying it to two partial signals is audibly (and
// numerically) different from applying it once to their sum.
sonare::mixing::api::Bus compressing_bus(uint32_t bus_id) {
  sonare::mixing::api::Bus bus;
  bus.id = std::to_string(bus_id);
  sonare::mixing::api::Insert insert;
  insert.slot = sonare::mixing::api::InsertSlot::PreFader;
  insert.processor_name = "dynamics.compressor";
  insert.params_json =
      "{\"thresholdDb\":-24.0,\"ratio\":20.0,\"attackMs\":0.5,\"releaseMs\":10.0,\"kneeDb\":0.0}";
  bus.inserts.push_back(insert);
  return bus;
}

// A looping clip whose timeline length outlasts the whole render, so the DC
// source keeps feeding the bus for every block the test measures.
sonare::engine::ClipSchedule dc_clip(uint32_t clip_id, uint32_t track_id,
                                     const float* const* samples, int channels, int frames,
                                     int64_t length) {
  sonare::engine::ClipSchedule clip{
      clip_id, {samples, channels, frames}, 0.0, 0, 0, length, true, 1.0f, 0, 0};
  clip.track_id = track_id;
  return clip;
}

void play(RealtimeEngine& engine) {
  sonare::rt::Command command{};
  command.type = sonare::rt::CommandType::kTransportPlay;
  command.sample_time = -1;
  REQUIRE(engine.push_command(command));
}

constexpr int kBlock = 128;
constexpr int kBlocks = 96;
constexpr float kHalf = 0.4f;
constexpr int64_t kClipLength = static_cast<int64_t>(kBlock) * (kBlocks + 4);

// Renders `blocks` blocks and returns the mean magnitude of the final block, so
// the compressor's envelope has long settled.
double run(RealtimeEngine& engine, int blocks) {
  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  double sum = 0.0;
  for (int b = 0; b < blocks; ++b) {
    left.fill(0.0f);
    right.fill(0.0f);
    float* io[] = {left.data(), right.data()};
    engine.process(io, 2, kBlock);
    if (b + 1 == blocks) {
      for (int i = 0; i < kBlock; ++i) sum += std::abs(left[i]);
    }
  }
  return sum / kBlock;
}

}  // namespace

TEST_CASE("A group bus insert acts once on the summed clip + instrument signal",
          "[engine][track_mixer][bus]") {
  std::vector<float> half(kBlock * 4, kHalf);
  const float* half_channels[] = {half.data(), half.data()};
  std::vector<float> whole(kBlock * 4, 2.0f * kHalf);
  const float* whole_channels[] = {whole.data(), whole.data()};

  // Split: a clip on lane 10 and an instrument on lane 11, each contributing
  // kHalf, both routed into group bus 1.
  double split = 0.0;
  {
    RealtimeEngine engine;
    engine.prepare(48000.0, kBlock);
    REQUIRE(engine.set_track_buses({{1, 0.0f, sonare::ChannelLayout::Stereo}}));
    sonare::engine::TrackLaneConfig clip_lane{10};
    clip_lane.output_bus_id = 1;
    sonare::engine::TrackLaneConfig inst_lane{11};
    inst_lane.output_bus_id = 1;
    REQUIRE(engine.set_track_lanes({clip_lane, inst_lane}));
    REQUIRE(engine.set_bus_strip(1, compressing_bus(1)));
    engine.set_clips(
        {dc_clip(1, 10, half_channels, 2, static_cast<int>(half.size()), kClipLength)});
    DcInstrument instrument(kHalf);
    REQUIRE(engine.set_midi_instrument(11, &instrument));
    play(engine);
    split = run(engine, kBlocks);
    engine.set_midi_instrument(11, nullptr);
  }

  // Reference: the same total level reaching the same bus through a single
  // contributor, so the insert unambiguously sees the summed signal once.
  double summed = 0.0;
  {
    RealtimeEngine engine;
    engine.prepare(48000.0, kBlock);
    REQUIRE(engine.set_track_buses({{1, 0.0f, sonare::ChannelLayout::Stereo}}));
    sonare::engine::TrackLaneConfig clip_lane{10};
    clip_lane.output_bus_id = 1;
    REQUIRE(engine.set_track_lanes({clip_lane}));
    REQUIRE(engine.set_bus_strip(1, compressing_bus(1)));
    engine.set_clips(
        {dc_clip(1, 10, whole_channels, 2, static_cast<int>(whole.size()), kClipLength)});
    play(engine);
    summed = run(engine, kBlocks);
  }

  REQUIRE(summed > 1.0e-4);
  // Before block-level aggregation the bus chain ran twice per block, once over
  // the clip contribution and once over the instrument contribution, so the
  // split case landed well above the reference (each partial sat further below
  // the threshold and was compressed less).
  REQUIRE(std::abs(split - summed) < 0.02 * summed);
}

TEST_CASE("A group bus insert acts once whether rendered live or offline",
          "[engine][track_mixer][bus]") {
  std::vector<float> half(kBlock * 4, kHalf);
  const float* half_channels[] = {half.data(), half.data()};

  auto build = [&](RealtimeEngine& engine, DcInstrument& instrument) {
    engine.prepare(48000.0, kBlock);
    REQUIRE(engine.set_track_buses({{1, 0.0f, sonare::ChannelLayout::Stereo}}));
    sonare::engine::TrackLaneConfig clip_lane{10};
    clip_lane.output_bus_id = 1;
    sonare::engine::TrackLaneConfig inst_lane{11};
    inst_lane.output_bus_id = 1;
    REQUIRE(engine.set_track_lanes({clip_lane, inst_lane}));
    REQUIRE(engine.set_bus_strip(1, compressing_bus(1)));
    engine.set_clips(
        {dc_clip(1, 10, half_channels, 2, static_cast<int>(half.size()), kClipLength)});
    REQUIRE(engine.set_midi_instrument(11, &instrument));
  };

  DcInstrument live_instrument(kHalf);
  RealtimeEngine live;
  build(live, live_instrument);
  play(live);
  const double live_level = run(live, kBlocks);
  live.set_midi_instrument(11, nullptr);

  DcInstrument offline_instrument(kHalf);
  RealtimeEngine offline;
  build(offline, offline_instrument);
  std::vector<float> left(static_cast<size_t>(kBlock) * kBlocks, 0.0f);
  std::vector<float> right(left.size(), 0.0f);
  float* out[] = {left.data(), right.data()};
  offline.render_offline(out, 2, static_cast<int64_t>(left.size()), kBlock);
  offline.set_midi_instrument(11, nullptr);

  double offline_level = 0.0;
  for (int i = 0; i < kBlock; ++i) {
    offline_level += std::abs(left[left.size() - kBlock + static_cast<size_t>(i)]);
  }
  offline_level /= kBlock;

  REQUIRE(live_level > 1.0e-4);
  REQUIRE(std::abs(live_level - offline_level) < 0.01 * live_level);
}
