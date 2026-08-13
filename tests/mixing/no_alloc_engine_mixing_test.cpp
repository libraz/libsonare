/// @file no_alloc_engine_mixing_test.cpp
/// @brief Engine and mixing no-allocation realtime tests.

#include "no_alloc_test_helpers.h"
#include "rt/delay_line.h"

namespace {

// A latent StereoPairOnly insert: it delays only the front pair it is handed, so
// the strip owes the remaining planes the same delay, and owes all of them that
// delay again once the insert is bypassed.
class LatentStereoInsert final : public sonare::rt::ProcessorBase {
 public:
  explicit LatentStereoInsert(int latency) : latency_(latency) {}

  void prepare(double, int) override {
    for (auto& delay : delays_) delay.prepare(static_cast<size_t>(latency_));
  }
  void process(float* const* channels, int num_channels, int num_samples) override {
    for (int ch = 0; ch < std::min(num_channels, 2); ++ch) {
      if (channels[ch] == nullptr) continue;
      for (int sample = 0; sample < num_samples; ++sample) {
        channels[ch][sample] = delays_[static_cast<size_t>(ch)].process(channels[ch][sample]);
      }
    }
  }
  void reset() override {
    for (auto& delay : delays_) delay.reset();
  }
  int latency_samples() const noexcept override { return latency_; }

 private:
  int latency_ = 0;
  std::array<sonare::rt::DelayLine, 2> delays_{};
};

}  // namespace

TEST_CASE("ChannelStrip process performs no heap allocation after prepare", "[mixing][rt]") {
  constexpr int kBlock = 256;
  sonare::mixing::ChannelStrip strip;
  strip.add_pre_insert(std::make_unique<sonare::mastering::dynamics::Compressor>(
      sonare::mastering::dynamics::CompressorConfig{}));
  strip.prepare(48000.0, kBlock);
  strip.set_polarity_invert(true, false);
  strip.set_channel_delay_samples(3);
  strip.set_input_trim_db(1.5f);
  strip.set_fader_db(-3.0f);
  strip.set_pan(0.2f);
  strip.set_width(1.25f);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 1.0f : 0.05f;
    right[static_cast<size_t>(i)] = 0.03f;
  }
  float* channels[] = {left.data(), right.data()};

  strip.process(channels, 2, kBlock);
  strip.reset();

  AllocationGuard guard;
  strip.process(channels, 2, kBlock);
  const size_t allocations = guard.count();

  REQUIRE(allocations == 0);
}

TEST_CASE("ChannelStrip insert alignment delays perform no heap allocation after prepare",
          "[mixing][rt][surround]") {
  // Both per-insert alignment banks run on the audio thread: one compensates the
  // planes a StereoPairOnly insert never saw, the other stands in for a bypassed
  // insert's latency. Both must be fully preallocated by prepare().
  constexpr int kBlock = 128;
  constexpr int kChannels = 6;
  constexpr int kLatency = 32;

  sonare::mixing::ChannelStripConfig config;
  config.enable_metering = false;
  sonare::mixing::ChannelStrip strip(config);
  strip.add_pre_insert(std::make_unique<LatentStereoInsert>(kLatency), /*stereo_pair_only=*/true);
  strip.prepare(48000.0, kBlock);

  std::array<std::array<float, kBlock>, kChannels> planes{};
  std::array<float*, kChannels> channels{};
  for (int ch = 0; ch < kChannels; ++ch) {
    planes[static_cast<size_t>(ch)].fill(0.05f);
    channels[static_cast<size_t>(ch)] = planes[static_cast<size_t>(ch)].data();
  }

  // Warm any lazily sized per-channel state, then measure the steady-state block.
  strip.process(channels.data(), kChannels, kBlock);
  strip.reset();
  {
    AllocationGuard guard;
    strip.process(channels.data(), kChannels, kBlock);
    REQUIRE(guard.count() == 0);
  }

  REQUIRE(strip.set_insert_bypassed(0, true));
  strip.process(channels.data(), kChannels, kBlock);
  strip.reset();
  {
    AllocationGuard guard;
    strip.process(channels.data(), kChannels, kBlock);
    REQUIRE(guard.count() == 0);
  }
}

TEST_CASE("RealtimeEngine process performs no heap allocation after prepare", "[engine][rt]") {
  constexpr int kBlock = 128;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.25f);
  right.fill(-0.25f);
  float* channels[] = {left.data(), right.data()};

  engine.process(channels, 2, kBlock);

  AllocationGuard guard;
  engine.process(channels, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("MeterTelemetryTap process performs no heap allocation after prepare",
          "[engine][meter_telemetry][rt]") {
  constexpr int kBlock = 128;
  sonare::engine::MeterTelemetryTap tap;
  tap.prepare(48000.0, kBlock, 3, 16);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.5f);
  right.fill(0.25f);
  float* channels[] = {left.data(), right.data()};

  tap.process(channels, 2, kBlock, 0);

  AllocationGuard guard;
  tap.process(channels, 2, kBlock, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("AutomationEngine apply performs no heap allocation after prepare", "[automation][rt]") {
  sonare::transport::TempoMap tempo;
  tempo.prepare(48000.0);

  sonare::automation::AutomationLane lane(7);
  lane.set_points({{0.0, 0.0f, sonare::automation::CurveType::Linear},
                   {1.0, 1.0f, sonare::automation::CurveType::Linear}});

  ParameterCaptureProcessor processor;
  sonare::automation::AutomationEngine engine;
  engine.prepare(48000.0, &tempo);
  engine.set_lanes({lane});
  engine.acquire_lanes();
  engine.bind_target(7, &processor);

  sonare::transport::TransportState state{};
  state.sample_position = 0;
  engine.apply(state, 12000, 64);

  AllocationGuard guard;
  engine.apply(state, 12000, 64);
  REQUIRE(guard.count() == 0);
  REQUIRE(processor.last_param == 7);
  REQUIRE(processor.last_value > 0.49f);
}

TEST_CASE("MixingRuntime process performs no heap allocation after prepare",
          "[engine][mixing][rt]") {
  constexpr int kBlock = 128;
  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  sonare::engine::MixingRuntime runtime;
  REQUIRE(runtime.bind(&strip));
  runtime.prepare(48000.0, kBlock);
  REQUIRE(runtime.set_parameter(sonare::engine::MixingRuntime::kFaderDb, -3.0f));

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(1.0f);
  right.fill(1.0f);
  float* channels[] = {left.data(), right.data()};

  runtime.process_at(channels, 2, kBlock, 0);

  AllocationGuard guard;
  runtime.process_at(channels, 2, kBlock, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("MonitorRuntime process performs no heap allocation after prepare",
          "[engine][monitor][rt]") {
  constexpr int kBlock = 128;
  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.prepare(48000.0, kBlock);

  sonare::engine::MonitorRuntime runtime;
  runtime.prepare(48000.0, kBlock, 5.0f);
  REQUIRE(runtime.add_strip(&strip));
  runtime.set_monitor_mode(0, sonare::engine::MonitorMode::kAfl);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  std::array<float, kBlock> mon_l{};
  std::array<float, kBlock> mon_r{};
  left.fill(1.0f);
  right.fill(1.0f);
  float* channels[] = {left.data(), right.data()};
  float* monitor[] = {mon_l.data(), mon_r.data()};

  runtime.process_strip(0, channels, 2, kBlock, 0, monitor);

  AllocationGuard guard;
  runtime.process_strip(0, channels, 2, kBlock, kBlock, monitor);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("ClipPlayer process performs no heap allocation after prepare", "[engine][clip][rt]") {
  constexpr int kBlock = 128;
  std::array<float, kBlock> source_l{};
  std::array<float, kBlock> source_r{};
  source_l.fill(0.5f);
  source_r.fill(-0.5f);
  const float* source[] = {source_l.data(), source_r.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, kBlock);
  // Looping clip several blocks long so the steady-state (mid-playback) path is
  // exercised when the timeline advances past the first block.
  player.set_clips({{1, {source, 2, kBlock}, 0.0, 0, 0, kBlock * 4, true, 1.0f, 4, 4}});

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* out[] = {left.data(), right.data()};
  player.process_at(out, 2, kBlock, 0);

  AllocationGuard guard;
  // Advance the timeline so the second render is steady-state mid-playback
  // (past the fade-in, across a loop wrap), not a replay from position 0.
  player.process_at(out, 2, kBlock, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("CaptureSink process performs no heap allocation after prepare",
          "[engine][capture][rt]") {
  constexpr int kBlock = 128;
  std::array<float, kBlock> in_l{};
  std::array<float, kBlock> in_r{};
  in_l.fill(0.25f);
  in_r.fill(-0.25f);
  const float* input[] = {in_l.data(), in_r.data()};
  std::array<float, kBlock> out_l{};
  std::array<float, kBlock> out_r{};
  float* capture[] = {out_l.data(), out_r.data()};

  sonare::engine::CaptureSink sink;
  sink.prepare({capture, 2, kBlock});
  sink.arm(true);
  sink.process(input, 2, kBlock, 0);

  AllocationGuard guard;
  sink.process(input, 2, kBlock, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("Metronome process performs no heap allocation after prepare",
          "[engine][metronome][rt]") {
  constexpr int kBlock = 256;
  sonare::transport::TempoMap tempo;
  tempo.prepare(48000.0);
  sonare::engine::Metronome metro;
  metro.prepare(48000.0, &tempo);
  metro.set_config({true, 0.25f, 0.75f, 16});

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* channels[] = {left.data(), right.data()};
  metro.process(channels, 2, kBlock, 0);

  AllocationGuard guard;
  metro.process(channels, 2, kBlock, 24000);
  REQUIRE(guard.count() == 0);
}

#ifdef SONARE_WITH_GRAPH
TEST_CASE("GraphRuntime process performs no heap allocation after prepare", "[engine][graph][rt]") {
  constexpr int kBlock = 128;
  sonare::graph::Graph graph;
  REQUIRE(graph.add_node("in", std::make_unique<ScaleProcessor>(1.0f), 2));
  REQUIRE(graph.add_node("gain", std::make_unique<ScaleProcessor>(0.5f), 2));
  REQUIRE(graph.add_node("out", std::make_unique<ScaleProcessor>(1.0f), 2));
  REQUIRE(graph.connect({"in", 0, "gain", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"in", 1, "gain", 1, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"gain", 0, "out", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"gain", 1, "out", 1, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.compile());
  graph.prepare(48000.0, kBlock);

  sonare::engine::GraphRuntime runtime;
  REQUIRE(runtime.bind(&graph, "in", "out", 2));

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(1.0f);
  right.fill(-1.0f);
  float* channels[] = {left.data(), right.data()};

  runtime.process(channels, 2, 0, kBlock);

  AllocationGuard guard;
  runtime.process(channels, 2, 0, kBlock);
  REQUIRE(guard.count() == 0);
}
#endif

TEST_CASE("TrackMixerRuntime insert automation render performs no heap allocation after prepare",
          "[engine][track_mixer][rt]") {
  constexpr int kBlock = 128;
  std::array<float, kBlock> source{};
  source.fill(1.0f);
  const float* channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, kBlock);
  sonare::engine::ClipSchedule clip{1, {channels, 1, kBlock}, 0.0, 0, 0, kBlock, true, 1.0f, 0, 0};
  clip.track_id = 10;
  player.set_clips({clip});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, kBlock);
  REQUIRE(mixer.set_track_lanes({{10}}));

  sonare::mixing::ChannelStrip strip;
  strip.add_pre_insert(std::make_unique<ParameterCaptureProcessor>());
  REQUIRE(mixer.bind_track_strip(10, &strip));
  // An active insert-automation slot so the render path advances and pushes the
  // smoother every block (the steady-state automated path).
  REQUIRE(mixer.route_lane_insert_param_smoothed(0, 0, 0, 0.5f));

  std::array<float, kBlock> out{};
  float* io[] = {out.data()};
  REQUIRE(mixer.render_clips(player, io, 1, kBlock, 0));
  // Retarget so the smoother is still moving when the guarded render runs.
  REQUIRE(mixer.route_lane_insert_param_smoothed(0, 0, 0, 0.75f));

  AllocationGuard guard;
  REQUIRE(mixer.render_clips(player, io, 1, kBlock, 0));
  REQUIRE(guard.count() == 0);
}
