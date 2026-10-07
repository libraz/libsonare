#include "engine/realtime_engine.h"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "engine/clip_player.h"
#include "engine/engine_controller.h"
#if defined(SONARE_WITH_MIXING)
#include "engine/insert_automation_id.h"
#include "mastering/dynamics/compressor.h"
#endif
#include "engine/parameter_base_table.h"
#include "engine/telemetry.h"
#include "transport/tempo_map.h"
#include "util/exception.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include "midi/instrument.h"
#include "midi/midi_event.h"
#endif

namespace {

#if defined(SONARE_WITH_MIXING)
constexpr uint32_t engine_lane_param_target(uint32_t lane_index, uint32_t param_kind) {
  return 0x4D580000u | (lane_index << 8u) | param_kind;
}

constexpr uint32_t engine_master_param_target(uint32_t param_kind) {
  return 0x4D580000u | (0xFFu << 8u) | param_kind;
}
#endif

TEST_CASE("RealtimeEngine prepares scratch for the declared channel count", "[engine]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 64, 16, 16, 64);
  const size_t full_channel_scratch_bytes = engine.prepared_scratch_bytes();
  engine.prepare(48000.0, 64, 16, 16, 2);
  REQUIRE(engine.prepared_channels() == 2);
  REQUIRE(engine.prepared_scratch_bytes() == full_channel_scratch_bytes / 32);

  std::array<float, 64> left{};
  std::array<float, 64> right{};
  left.fill(0.25f);
  right.fill(-0.25f);
  float* stereo[] = {left.data(), right.data()};
  engine.process(stereo, 2, 64);
  REQUIRE(left[0] == Catch::Approx(0.25f));
  REQUIRE(right[0] == Catch::Approx(-0.25f));

  std::array<float, 64> extra{};
  extra.fill(1.0f);
  float* over_prepared[] = {left.data(), right.data(), extra.data()};
  engine.process(over_prepared, 3, 64);
  REQUIRE(extra[0] == Catch::Approx(0.0f));
}

#if defined(SONARE_WITH_ARRANGEMENT)
/// @brief Host instrument whose prepare() fails once, on demand.
/// @details The engine prepares every bound instrument from inside its own
///          prepare(), and an instrument's prepare() is neither noexcept nor
///          owned by the engine. Arming this reproduces a control-thread failure
///          at that seam without exhausting real memory, and it sits partway
///          through the sequence: the engine has already adopted the new sample
///          rate, block size and channel count, and has already resized some
///          scratch, but has not yet reserved its queues or the capture scratch.
class FailingPrepareInstrument final : public sonare::midi::MidiInstrument {
 public:
  void arm() noexcept { armed_ = true; }
  int prepare_calls() const noexcept { return prepare_calls_; }

  void prepare(double, int) override {
    ++prepare_calls_;
    if (armed_) {
      armed_ = false;
      throw sonare::SonareException(sonare::ErrorCode::OutOfMemory,
                                    "instrument prepare could not allocate");
    }
  }
  void process(float* const*, int, int) override {}
  void reset() override {}
  void on_event(uint32_t, const sonare::midi::MidiEvent&) noexcept override {}

 private:
  bool armed_ = false;
  int prepare_calls_ = 0;
};

/// @brief Drains telemetry and reports whether the engine refused to render.
bool drained_not_prepared(sonare::engine::RealtimeEngine& engine) {
  bool seen = false;
  sonare::engine::Telemetry record{};
  while (engine.pop_telemetry(record)) {
    if (record.error == sonare::engine::TelemetryErrorCode::kNotPrepared) seen = true;
  }
  return seen;
}
#endif  // defined(SONARE_WITH_ARRANGEMENT)

template <size_t N>
void fill_signal(std::array<float, N>& left, std::array<float, N>& right) {
  for (size_t i = 0; i < N; ++i) {
    left[i] = static_cast<float>(i) * 0.01f;
    right[i] = -static_cast<float>(i) * 0.02f;
  }
}

class CaptureProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit CaptureProcessor(const sonare::transport::Transport* transport = nullptr)
      : transport_(transport) {}

  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  bool set_parameter_impl(unsigned int param_id, float value) override {
    params[static_cast<size_t>(count)] = param_id;
    values[static_cast<size_t>(count)] = value;
    render_frames[static_cast<size_t>(count)] =
        transport_ != nullptr ? transport_->render_frame() : -1;
    ++count;
    return true;
  }

  std::array<unsigned int, 128> params{};
  std::array<float, 128> values{};
  std::array<int64_t, 128> render_frames{};
  int count = 0;

 private:
  const sonare::transport::Transport* transport_ = nullptr;
};

std::vector<int64_t> parameter_change_frames(const CaptureProcessor& processor) {
  std::vector<int64_t> frames;
  for (int i = 1; i < processor.count; ++i) {
    if (processor.values[static_cast<size_t>(i)] > processor.values[static_cast<size_t>(i - 1)]) {
      frames.push_back(processor.render_frames[static_cast<size_t>(i)]);
    }
  }
  return frames;
}

#if defined(SONARE_WITH_MIXING)
class InsertCommandProbe final : public sonare::rt::ProcessorBase {
 public:
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override {
    return param_id == 0 || param_id == 7;
  }
  std::vector<sonare::rt::ParamDescriptor> parameter_descriptors() const override {
    return {{"gain", 0}, {"unrelated", 7}};
  }
  bool set_parameter_impl(unsigned int param_id, float value) override {
    if (param_id == 0) {
      insert_value = value;
      ++insert_count;
      return true;
    }
    if (param_id == 7) {
      unrelated_value = value;
      ++unrelated_count;
      return true;
    }
    return false;
  }

  float insert_value = 1.0f;
  float unrelated_value = 0.0f;
  int insert_count = 0;
  int unrelated_count = 0;
};
#endif

#if defined(SONARE_WITH_GRAPH)
class GraphLatencyProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit GraphLatencyProcessor(int latency_q8) : latency_q8_(latency_q8) {}
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  int latency_samples_q8() const noexcept override { return latency_q8_; }

 private:
  int latency_q8_ = 0;
};
#endif

}  // namespace

TEST_CASE("RealtimeEngine pass-through output is deterministic", "[engine][realtime]") {
  constexpr int kFrames = 128;
  sonare::engine::RealtimeEngine a;
  sonare::engine::RealtimeEngine b;
  a.prepare(48000.0, kFrames);
  b.prepare(48000.0, kFrames);

  std::array<float, kFrames> a_l{};
  std::array<float, kFrames> a_r{};
  std::array<float, kFrames> b_l{};
  std::array<float, kFrames> b_r{};
  fill_signal(a_l, a_r);
  fill_signal(b_l, b_r);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(a.push_command(play));
  REQUIRE(b.push_command(play));

  float* a_io[] = {a_l.data(), a_r.data()};
  float* b_io[] = {b_l.data(), b_r.data()};
  a.process(a_io, 2, kFrames);
  b.process(b_io, 2, kFrames);

  REQUIRE(a_l == b_l);
  REQUIRE(a_r == b_r);
  REQUIRE(a.transport().render_frame() == kFrames);
  REQUIRE(a.transport().sample_position() == kFrames);
}

TEST_CASE("RealtimeEngine control flush prevents an offline mirror command ring from filling",
          "[engine][realtime]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 64, /*command_capacity=*/4, /*telemetry_capacity=*/4);

  sonare::rt::Command seek{};
  seek.type = sonare::rt::CommandType::kTransportSeekSample;
  seek.sample_time = -1;
  for (int64_t sample = 0; sample < 1024; ++sample) {
    seek.arg.i = sample;
    REQUIRE(engine.push_command(seek));
    engine.flush_control_commands();
  }

  REQUIRE(engine.transport().sample_position() == 1023);
}

#if defined(SONARE_WITH_MIXING)
TEST_CASE("RealtimeEngine rejects registering one strip in mixing and monitor runtimes",
          "[engine][realtime]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  sonare::mixing::ChannelStrip strip;

  REQUIRE(engine.bind_mixing_strip(&strip));
  REQUIRE_FALSE(engine.add_monitor_strip(&strip));

  sonare::engine::RealtimeEngine monitor_first;
  monitor_first.prepare(48000.0, 64);
  sonare::mixing::ChannelStrip other;
  REQUIRE(monitor_first.add_monitor_strip(&other));
  REQUIRE_FALSE(monitor_first.bind_mixing_strip(&other));
}

TEST_CASE("RealtimeEngine reports track and master strip latency", "[engine][realtime]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 16);
  REQUIRE(engine.set_track_lanes({{10}, {20}}));

  sonare::mixing::ChannelStrip track_strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  track_strip.add_pre_insert(std::make_unique<sonare::mixing::AlignmentDelay>(4));
  REQUIRE(engine.bind_track_strip(10, &track_strip));
  REQUIRE(engine.graph_latency_samples_q8() == (4 << 8));

  sonare::mixing::ChannelStrip master_strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  master_strip.add_pre_insert(std::make_unique<sonare::mixing::AlignmentDelay>(3));
  REQUIRE(engine.bind_mixing_strip(&master_strip));
  engine.set_mixing_enabled(true);
  REQUIRE(engine.graph_latency_samples_q8() == (7 << 8));

  engine.set_mixing_enabled(false);
  REQUIRE(engine.graph_latency_samples_q8() == (4 << 8));
}
#endif  // defined(SONARE_WITH_MIXING)

#if defined(SONARE_WITH_GRAPH)
TEST_CASE("RealtimeEngine includes a swapped routing graph in reported PDC", "[engine][realtime]") {
  auto graph = std::make_unique<sonare::graph::Graph>();
  REQUIRE(graph->add_node("in", std::make_unique<GraphLatencyProcessor>(0), 1));
  REQUIRE(graph->add_node("latent", std::make_unique<GraphLatencyProcessor>(9 << 8), 1));
  REQUIRE(graph->add_node("out", std::make_unique<GraphLatencyProcessor>(3 << 8), 1));
  REQUIRE(graph->connect({"in", 0, "latent", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph->connect({"latent", 0, "out", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph->compile());
  graph->prepare(48000.0, 64);

  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  REQUIRE(engine.swap_graph(std::move(graph), "in", "out", 1));
  REQUIRE(engine.graph_latency_samples_q8() == (12 << 8));
}
#endif

TEST_CASE("RealtimeEngine publishes lane bus input and master meter targets",
          "[engine][realtime]") {
#if defined(SONARE_WITH_MIXING)
  constexpr int kBlock = 128;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock, 64, 16);

  std::array<float, kBlock> track_a{};
  std::array<float, kBlock> track_b{};
  track_a.fill(0.5f);
  track_b.fill(0.25f);
  const float* a_channels[] = {track_a.data()};
  const float* b_channels[] = {track_b.data()};
  sonare::engine::ClipSchedule clips[2]{};
  clips[0].id = 1;
  clips[0].track_id = 10;
  clips[0].buffer = {a_channels, 1, kBlock};
  clips[0].length_samples = kBlock;
  clips[0].gain = 1.0f;
  clips[1].id = 2;
  clips[1].track_id = 20;
  clips[1].buffer = {b_channels, 1, kBlock};
  clips[1].length_samples = kBlock;
  clips[1].gain = 1.0f;
  engine.set_clips({clips[0], clips[1]});

  REQUIRE(engine.set_track_buses({{1, 0.0f}}));
  sonare::engine::TrackLaneConfig lane_a{10};
  lane_a.sends.push_back({1, 0.0f, true});
  sonare::engine::TrackLaneConfig lane_b{20};
  REQUIRE(engine.set_track_lanes({lane_a, lane_b}));
  engine.set_capture_source(sonare::engine::CaptureSource::kInput);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::array<float, kBlock> left{};
  left.fill(0.125f);
  float* io[] = {left.data()};
  engine.process(io, 1, kBlock);

  bool found_input = false;
  bool found_lane_1 = false;
  bool found_lane_2 = false;
  bool found_bus = false;
  bool found_master = false;
  sonare::engine::MeterTelemetryRecord record{};
  while (engine.pop_meter_telemetry(record)) {
    if (record.target_id == 0xFFFFu) {
      found_input = true;
      REQUIRE(record.peak_db[0] == Catch::Approx(-18.0618f).margin(0.05f));
      // Lightweight targets do not measure loudness: the field stays at the dB
      // floor (finite, JSON-safe), never NaN.
      REQUIRE(std::isfinite(record.integrated_lufs));
      REQUIRE(record.integrated_lufs == Catch::Approx(sonare::constants::kFloorDb));
    } else if (record.target_id == 1) {
      found_lane_1 = true;
      REQUIRE(record.peak_db[0] == Catch::Approx(-3.0103f).margin(0.05f));
      REQUIRE(std::isfinite(record.integrated_lufs));
      REQUIRE(record.integrated_lufs == Catch::Approx(sonare::constants::kFloorDb));
    } else if (record.target_id == 2) {
      found_lane_2 = true;
      REQUIRE(record.peak_db[0] == Catch::Approx(-12.0412f).margin(0.05f));
    } else if (record.target_id == 33) {
      found_bus = true;
      REQUIRE(record.peak_db[0] == Catch::Approx(-3.0103f).margin(0.05f));
    } else if (record.target_id == 0) {
      found_master = true;
      REQUIRE(record.peak_db[0] > -1.0f);
    }
  }

  REQUIRE(found_input);
  REQUIRE(found_lane_1);
  REQUIRE(found_lane_2);
  REQUIRE(found_bus);
  REQUIRE(found_master);
#endif
}

#if defined(SONARE_WITH_MIXING)
TEST_CASE("Queued master integrated meter reset starts a new loudness program",
          "[engine][realtime][meter]") {
  constexpr int kBlock = 1024;
  constexpr int kProgramBlocks = 160;  // More than the 3 s short-term window.
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock, 64, 16);
  engine.set_input_monitor(true, 1.0f);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    const float sample =
        0.5f * std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(i) / 48000.0f);
    left[static_cast<size_t>(i)] = sample;
    right[static_cast<size_t>(i)] = sample;
  }
  float* io[] = {left.data(), right.data()};

  for (int block = 0; block < kProgramBlocks; ++block) {
    engine.process(io, 2, kBlock);
  }

  sonare::engine::MeterTelemetryRecord record{};
  sonare::engine::MeterTelemetryRecord before_reset{};
  bool found_before_reset = false;
  while (engine.pop_meter_telemetry(record)) {
    if (record.target_id == 0) {
      before_reset = record;
      found_before_reset = true;
    }
  }
  REQUIRE(found_before_reset);
  REQUIRE(std::isfinite(before_reset.integrated_lufs));
  REQUIRE(before_reset.integrated_lufs > sonare::constants::kFloorDb + 1.0f);

  // A queued transport command proves that the meter reset is one ordinary
  // command and does not purge commands already waiting behind it.
  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  REQUIRE(engine.push_command(play));
  REQUIRE(engine.reset_master_meter_integrated());
  engine.process(io, 2, kBlock);
  REQUIRE(engine.transport().playing());

  sonare::engine::MeterTelemetryRecord after_reset{};
  bool found_after_reset = false;
  while (engine.pop_meter_telemetry(record)) {
    if (record.target_id == 0) {
      after_reset = record;
      found_after_reset = true;
    }
  }
  REQUIRE(found_after_reset);
  REQUIRE(after_reset.integrated_lufs == Catch::Approx(sonare::constants::kFloorDb));
  // reset_integrated() must retain the short/momentary and true-peak windows.
  REQUIRE(after_reset.momentary_lufs > sonare::constants::kFloorDb + 1.0f);
  REQUIRE(after_reset.short_term_lufs > sonare::constants::kFloorDb + 1.0f);
  REQUIRE(after_reset.max_true_peak_db > sonare::constants::kFloorDb + 1.0f);

  for (int block = 0; block < kProgramBlocks; ++block) {
    engine.process(io, 2, kBlock);
  }
  sonare::engine::MeterTelemetryRecord after_new_program{};
  bool found_new_program = false;
  while (engine.pop_meter_telemetry(record)) {
    if (record.target_id == 0) {
      after_new_program = record;
      found_new_program = true;
    }
  }
  REQUIRE(found_new_program);
  REQUIRE(std::isfinite(after_new_program.integrated_lufs));
  REQUIRE(after_new_program.integrated_lufs > sonare::constants::kFloorDb + 1.0f);
}

TEST_CASE("Insert constructed values are readable through resolved track bus and master ids",
          "[engine][realtime][mixing]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 64);

  sonare::mixing::api::Strip track;
  track.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PreFader, "utility.gain", R"({"levelDb":-3})"});
  REQUIRE(engine.set_track_lanes({{10}}));
  REQUIRE(engine.set_track_strip(10, track));

  sonare::mixing::api::Bus bus;
  bus.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PreFader, "utility.gain", R"({"levelDb":-6})"});
  REQUIRE(engine.set_track_buses({{1}}));
  REQUIRE(engine.set_bus_strip(1, bus));

  sonare::mixing::api::Strip master;
  master.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PreFader, "utility.gain", R"({"levelDb":-12})"});
  REQUIRE(engine.set_master_strip(master));

  const int64_t track_id = engine.resolve_track_insert_automation_id(10, 0, "levelDb");
  const int64_t bus_id = engine.resolve_bus_insert_automation_id(1, 0, "levelDb");
  const int64_t master_id = engine.resolve_master_insert_automation_id(0, "levelDb");
  REQUIRE(track_id >= 0);
  REQUIRE(bus_id >= 0);
  REQUIRE(master_id >= 0);

  float value = 0.0f;
  REQUIRE(engine.insert_parameter_constructed_value(static_cast<uint32_t>(track_id), &value));
  REQUIRE(value == Catch::Approx(-3.0f));
  REQUIRE(engine.insert_parameter_constructed_value(static_cast<uint32_t>(bus_id), &value));
  REQUIRE(value == Catch::Approx(-6.0f));
  REQUIRE(engine.insert_parameter_constructed_value(static_cast<uint32_t>(master_id), &value));
  REQUIRE(value == Catch::Approx(-12.0f));

  REQUIRE_FALSE(engine.insert_parameter_constructed_value(0, &value));
  REQUIRE_FALSE(
      engine.insert_parameter_constructed_value(static_cast<uint32_t>(track_id), nullptr));
}
#endif

#if defined(SONARE_WITH_MIXING)
TEST_CASE("RealtimeEngine routes monitor PFL bus into output", "[engine][realtime]") {
  constexpr int kFrames = 16;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kFrames);
  sonare::mixing::ChannelStrip strip({-6.0206f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.prepare(48000.0, kFrames);
  REQUIRE(engine.add_monitor_strip(&strip));
  engine.set_monitoring_enabled(true);
  engine.monitor().set_monitor_mode(0, sonare::engine::MonitorMode::kPfl);

  std::array<float, kFrames> left{};
  left.fill(1.0f);
  float* io[] = {left.data()};
  engine.process(io, 1, kFrames);

  REQUIRE(left.back() > 1.70f);
  REQUIRE(left.back() < 1.72f);
}

TEST_CASE("RealtimeEngine can route monitor PFL bus separately from output", "[engine][realtime]") {
  constexpr int kFrames = 16;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kFrames);
  sonare::mixing::ChannelStrip strip({-6.0206f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.prepare(48000.0, kFrames);
  REQUIRE(engine.add_monitor_strip(&strip));
  engine.set_monitoring_enabled(true);
  engine.monitor().set_monitor_mode(0, sonare::engine::MonitorMode::kPfl);

  std::array<float, kFrames> left{};
  std::array<float, kFrames> cue{};
  left.fill(1.0f);
  float* io[] = {left.data()};
  float* monitor[] = {cue.data()};
  engine.process_with_monitor(io, monitor, 1, kFrames);

  REQUIRE(cue.front() == Catch::Approx(1.0f).margin(1.0e-6f));
  REQUIRE(cue.back() == Catch::Approx(1.0f).margin(1.0e-6f));
  REQUIRE(left.back() > 0.70f);
  REQUIRE(left.back() < 0.72f);
}
#endif  // defined(SONARE_WITH_MIXING)

TEST_CASE("RealtimeEngine applies scheduled transport commands inside a block",
          "[engine][realtime]") {
  constexpr int kFrames = 128;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kFrames);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = 32;
  REQUIRE(engine.push_command(play));

  std::array<float, kFrames> left{};
  float* io[] = {left.data()};
  engine.process(io, 1, kFrames);

  REQUIRE(engine.transport().render_frame() == 128);
  REQUIRE(engine.transport().sample_position() == 96);
}

TEST_CASE("RealtimeEngine defers commands scheduled at block end to the next block",
          "[engine][realtime]") {
  constexpr int kFrames = 128;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kFrames);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = kFrames;
  REQUIRE(engine.push_command(play));

  std::array<float, kFrames> left{};
  float* io[] = {left.data()};
  engine.process(io, 1, kFrames);

  REQUIRE_FALSE(engine.transport().snapshot().playing);
  REQUIRE(engine.transport().render_frame() == kFrames);
  REQUIRE(engine.transport().sample_position() == 0);

  engine.process(io, 1, kFrames);
  REQUIRE(engine.transport().snapshot().playing);
  REQUIRE(engine.transport().sample_position() == kFrames);
}

TEST_CASE("RealtimeEngine silences oversized blocks and emits telemetry", "[engine][realtime]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 64);

  std::array<float, 128> left{};
  std::array<float, 128> right{};
  left.fill(1.0f);
  right.fill(-1.0f);
  float* io[] = {left.data(), right.data()};
  engine.process(io, 2, 128);

  for (float sample : left) {
    REQUIRE(sample == 0.0f);
  }
  for (float sample : right) {
    REQUIRE(sample == 0.0f);
  }

  sonare::engine::Telemetry telemetry{};
  REQUIRE(engine.pop_telemetry(telemetry));
  REQUIRE(telemetry.type == sonare::engine::TelemetryType::kError);
  REQUIRE(telemetry.error == sonare::engine::TelemetryErrorCode::kMaxBlockExceeded);
  REQUIRE(telemetry.render_frame == 0);
  REQUIRE(telemetry.value == 128);
}

TEST_CASE("RealtimeEngine reports channel-bound telemetry after block-size validation",
          "[engine][realtime]") {
  constexpr int kPreparedChannels = 2;
  constexpr int kBlock = 64;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock, 16, 16, kPreparedChannels);

  std::array<float, kBlock * 2> left{};
  std::array<float, kBlock * 2> right{};
  std::array<float, kBlock * 2> extra{};
  left.fill(1.0f);
  right.fill(-1.0f);
  extra.fill(1.0f);
  float* too_many_channels[] = {left.data(), right.data(), extra.data()};

  // The block-size guard remains first when both limits are exceeded.
  engine.process(too_many_channels, 3, kBlock * 2);
  for (float sample : left) REQUIRE(sample == 0.0f);
  for (float sample : right) REQUIRE(sample == 0.0f);
  for (float sample : extra) REQUIRE(sample == 0.0f);
  REQUIRE(engine.transport().render_frame() == kBlock * 2);

  sonare::engine::Telemetry telemetry{};
  REQUIRE(engine.pop_telemetry(telemetry));
  REQUIRE(telemetry.type == sonare::engine::TelemetryType::kError);
  REQUIRE(telemetry.error == sonare::engine::TelemetryErrorCode::kMaxBlockExceeded);
  REQUIRE(telemetry.value == static_cast<uint32_t>(kBlock * 2));

  // A block within the prepared size reports the requested channel count and
  // keeps the existing silence/advance behavior for a channel overflow.
  left.fill(1.0f);
  right.fill(-1.0f);
  extra.fill(1.0f);
  engine.process(too_many_channels, 3, kBlock);
  for (int i = 0; i < kBlock; ++i) {
    REQUIRE(left[static_cast<size_t>(i)] == 0.0f);
    REQUIRE(right[static_cast<size_t>(i)] == 0.0f);
    REQUIRE(extra[static_cast<size_t>(i)] == 0.0f);
  }
  REQUIRE(engine.transport().render_frame() == kBlock * 3);

  REQUIRE(engine.pop_telemetry(telemetry));
  REQUIRE(telemetry.type == sonare::engine::TelemetryType::kError);
  REQUIRE(telemetry.error == sonare::engine::TelemetryErrorCode::kMaxChannelsExceeded);
  REQUIRE(telemetry.value == 3);

  // The exact prepared channel count remains a valid processing shape.
  left.fill(0.25f);
  right.fill(-0.25f);
  float* prepared_channels[] = {left.data(), right.data()};
  engine.process(prepared_channels, kPreparedChannels, kBlock);
  REQUIRE(left[0] == Catch::Approx(0.25f));
  REQUIRE(right[0] == Catch::Approx(-0.25f));
  REQUIRE(engine.pop_telemetry(telemetry));
  REQUIRE(telemetry.type == sonare::engine::TelemetryType::kProcessBlock);
  REQUIRE(telemetry.error == sonare::engine::TelemetryErrorCode::kNone);
  REQUIRE(telemetry.value == static_cast<uint32_t>(kBlock));
}

TEST_CASE("EngineController queues commands and drains telemetry", "[engine][realtime]") {
  sonare::engine::EngineController controller;
  controller.prepare(48000.0, 128);
  REQUIRE(controller.play());

  std::array<float, 128> left{};
  float* io[] = {left.data()};
  controller.engine().process(io, 1, 128);

  std::array<sonare::engine::Telemetry, 4> telemetry{};
  size_t written = 0;
  REQUIRE(controller.drain_telemetry(telemetry.data(), telemetry.size(), &written));
  REQUIRE(written == 1);
  REQUIRE(telemetry[0].type == sonare::engine::TelemetryType::kProcessBlock);
  REQUIRE(telemetry[0].render_frame == 0);
  REQUIRE(telemetry[0].timeline_sample == 128);
}

TEST_CASE("RealtimeEngine applies automation at sub-block boundaries", "[engine][realtime]") {
  constexpr int kFrames = 128;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kFrames);

  CaptureProcessor processor;
  sonare::automation::AutomationLane lane(7);
  lane.set_points({{0.0, 0.0f, sonare::automation::CurveType::Linear},
                   {64.0 / 24000.0, 0.5f, sonare::automation::CurveType::Linear},
                   {128.0 / 24000.0, 1.0f, sonare::automation::CurveType::Linear}});
  engine.automation().set_lanes({lane});
  engine.automation().bind_target(7, &processor);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::array<float, kFrames> left{};
  float* io[] = {left.data()};
  engine.process(io, 1, kFrames);

  REQUIRE(processor.count >= 2);
  REQUIRE(processor.params[0] == 7);
  REQUIRE(processor.values[0] == 0.0f);
  REQUIRE(processor.params[1] == 7);
  REQUIRE(processor.values[1] == 0.5f);
}

TEST_CASE("RealtimeEngine reapplies automation after every loop wrap in one block",
          "[engine][realtime]") {
  constexpr double kSampleRate = 48000.0;
  constexpr int kLoopSamples = 100;
  constexpr int kBreakpointSamples = 25;
  constexpr int kFrames = 350;

  sonare::engine::RealtimeEngine engine;
  engine.prepare(kSampleRate, kFrames);
  engine.set_tempo(60.0);
  engine.set_loop(0.0, static_cast<double>(kLoopSamples) / kSampleRate, true);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::array<float, kLoopSamples> priming_output{};
  float* priming_io[] = {priming_output.data()};
  engine.process(priming_io, 1, kLoopSamples);
  REQUIRE(engine.transport().playing());
  REQUIRE(engine.transport().sample_position() == 0);
  REQUIRE(engine.sample_at_ppq(static_cast<double>(kLoopSamples) / kSampleRate) == kLoopSamples);
  const int64_t block_render_start = engine.transport().render_frame();

  CaptureProcessor processor(&engine.transport());
  sonare::automation::AutomationLane lane(7);
  lane.set_points({{0.0, 0.0f, sonare::automation::CurveType::Hold},
                   {static_cast<double>(kBreakpointSamples) / kSampleRate, 1.0f,
                    sonare::automation::CurveType::Hold}});
  engine.automation().set_lanes({lane});
  engine.automation().bind_target(7, &processor);

  std::array<float, kFrames> output{};
  float* io[] = {output.data()};
  engine.process(io, 1, kFrames);

  std::vector<int64_t> changes = parameter_change_frames(processor);
  for (int64_t& frame : changes) frame -= block_render_start;
  const std::vector<int64_t> expected{25, 125, 225, 325};
  REQUIRE(changes == expected);
}

TEST_CASE("RealtimeEngine repeats loop automation from a mid-loop block start",
          "[engine][realtime]") {
  constexpr double kSampleRate = 48000.0;
  constexpr int kLoopSamples = 100;
  constexpr int kStartSample = 50;
  constexpr int kBreakpointSamples = 25;
  constexpr int kFrames = 350;

  sonare::engine::RealtimeEngine engine;
  engine.prepare(kSampleRate, kFrames);
  engine.set_tempo(60.0);
  engine.set_loop(0.0, static_cast<double>(kLoopSamples) / kSampleRate, true);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::array<float, kLoopSamples + kStartSample> priming_output{};
  float* priming_io[] = {priming_output.data()};
  engine.process(priming_io, 1, kLoopSamples + kStartSample);
  REQUIRE(engine.transport().playing());
  REQUIRE(engine.transport().sample_position() == kStartSample);
  REQUIRE(engine.sample_at_ppq(static_cast<double>(kLoopSamples) / kSampleRate) == kLoopSamples);
  const int64_t block_render_start = engine.transport().render_frame();

  CaptureProcessor processor(&engine.transport());
  sonare::automation::AutomationLane lane(7);
  lane.set_points({{0.0, 0.0f, sonare::automation::CurveType::Hold},
                   {static_cast<double>(kBreakpointSamples) / kSampleRate, 1.0f,
                    sonare::automation::CurveType::Hold}});
  engine.automation().set_lanes({lane});
  engine.automation().bind_target(7, &processor);

  std::array<float, kFrames> output{};
  float* io[] = {output.data()};
  engine.process(io, 1, kFrames);

  std::vector<int64_t> changes = parameter_change_frames(processor);
  for (int64_t& frame : changes) frame -= block_render_start;
  const std::vector<int64_t> expected{75, 175, 275};
  REQUIRE(changes == expected);
}

TEST_CASE("RealtimeEngine keeps automation boundaries correct without multiple wraps",
          "[engine][realtime]") {
  constexpr double kSampleRate = 48000.0;
  constexpr int kLoopSamples = 100;
  constexpr int kBreakpointSamples = 25;

  auto run = [&](int frames, bool looping) {
    sonare::engine::RealtimeEngine engine;
    engine.prepare(kSampleRate, frames);
    engine.set_tempo(60.0);
    if (looping) {
      engine.set_loop(0.0, static_cast<double>(kLoopSamples) / kSampleRate, true);
    }

    sonare::rt::Command play{};
    play.type = sonare::rt::CommandType::kTransportPlay;
    play.sample_time = -1;
    REQUIRE(engine.push_command(play));

    const int priming_frames = looping ? kLoopSamples : 1;
    std::vector<float> priming_output(static_cast<size_t>(priming_frames));
    float* priming_io[] = {priming_output.data()};
    engine.process(priming_io, 1, priming_frames);
    REQUIRE(engine.transport().playing());
    if (looping) REQUIRE(engine.transport().sample_position() == 0);
    const int64_t block_render_start = engine.transport().render_frame();

    CaptureProcessor processor(&engine.transport());
    sonare::automation::AutomationLane lane(7);
    lane.set_points({{0.0, 0.0f, sonare::automation::CurveType::Hold},
                     {static_cast<double>(kBreakpointSamples) / kSampleRate, 1.0f,
                      sonare::automation::CurveType::Hold}});
    engine.automation().set_lanes({lane});
    engine.automation().bind_target(7, &processor);

    std::vector<float> output(static_cast<size_t>(frames));
    float* io[] = {output.data()};
    engine.process(io, 1, frames);
    std::vector<int64_t> changes = parameter_change_frames(processor);
    for (int64_t& frame : changes) frame -= block_render_start;
    return changes;
  };

  const std::vector<int64_t> no_wrap{24};
  REQUIRE(run(80, false) == no_wrap);

  const std::vector<int64_t> one_wrap{25, 125};
  REQUIRE(run(150, true) == one_wrap);
}

TEST_CASE("ParameterBaseTable records, looks up, and updates values", "[engine]") {
  sonare::engine::ParameterBaseTable table;
  table.prepare(8, 4);

  float value = 0.0f;
  REQUIRE_FALSE(table.lookup(1, &value));  // nothing recorded yet.
  REQUIRE(table.entry_count() == 0);

  REQUIRE(table.record(1, 0.5f));
  REQUIRE(table.entry_count() == 1);
  REQUIRE(table.lookup(1, &value));
  REQUIRE(value == 0.5f);

  // Recording the same id again updates it in place, not a new entry.
  REQUIRE(table.record(1, -0.25f));
  REQUIRE(table.entry_count() == 1);
  REQUIRE(table.lookup(1, &value));
  REQUIRE(value == -0.25f);

  REQUIRE(table.record(2, 1.0f));
  REQUIRE(table.entry_count() == 2);
  REQUIRE(table.lookup(2, &value));
  REQUIRE(value == 1.0f);
  // The first id's value is unaffected by recording a second, distinct one.
  REQUIRE(table.lookup(1, &value));
  REQUIRE(value == -0.25f);

  REQUIRE_FALSE(table.lookup(3, &value));  // id 3 was never recorded.
  REQUIRE_FALSE(table.record(0, 1.0f));    // 0 is the reserved invalid id.
  REQUIRE(table.entry_count() == 2);       // the rejected id 0 is not an entry.
}

TEST_CASE("ParameterBaseTable reports capacity overflow once max_entries is reached", "[engine]") {
  // A small capacity, independent of RealtimeEngine's 4096-entry sizing, so
  // the overflow signal that drives telemetry 21 is reachable directly.
  sonare::engine::ParameterBaseTable table;
  table.prepare(16, 4);

  REQUIRE(table.record(1, 1.0f));
  REQUIRE(table.record(2, 2.0f));
  REQUIRE(table.record(3, 3.0f));
  REQUIRE(table.record(4, 4.0f));
  REQUIRE(table.entry_count() == 4);

  // A fifth distinct id cannot be recorded: "not recorded" is what the
  // caller (RealtimeEngine::record_parameter_base) turns into telemetry 21.
  REQUIRE_FALSE(table.record(5, 5.0f));
  REQUIRE(table.entry_count() == 4);
  float value = 0.0f;
  REQUIRE_FALSE(table.lookup(5, &value));

  // An update to an id already present still succeeds even while full.
  REQUIRE(table.record(2, -2.0f));
  REQUIRE(table.entry_count() == 4);
  REQUIRE(table.lookup(2, &value));
  REQUIRE(value == -2.0f);
}

TEST_CASE("ParameterBaseTable erase_if preserves collision chains", "[engine]") {
  sonare::engine::ParameterBaseTable table;
  table.prepare(8, 8);

  // These ids all hash to slot 1 when the table uses its mask. Removing the
  // first one must not make either later entry look absent.
  REQUIRE(table.record(1, 1.0f));
  REQUIRE(table.record(9, 9.0f));
  REQUIRE(table.record(17, 17.0f));
  REQUIRE(table.entry_count() == 3);

  REQUIRE(table.erase_if([](uint32_t id, float) noexcept { return id == 1; }) == 1u);
  float value = 0.0f;
  REQUIRE_FALSE(table.lookup(1, &value));
  REQUIRE(table.lookup(9, &value));
  REQUIRE(value == 9.0f);
  REQUIRE(table.lookup(17, &value));
  REQUIRE(value == 17.0f);

  // A subsequent insert reuses the tombstone while retaining the existing
  // collision chain and entry count semantics.
  REQUIRE(table.record(25, 25.0f));
  REQUIRE(table.lookup(25, &value));
  REQUIRE(value == 25.0f);
  REQUIRE(table.entry_count() == 3);
}

#if defined(SONARE_WITH_MIXING)
TEST_CASE("Clearing an insert base drops queued and pending stale commands",
          "[engine][realtime][mixing]") {
  constexpr int kBlock = 64;
  constexpr int64_t kFuture = 1000;

  sonare::mixing::ChannelStrip strip;
  auto probe = std::make_unique<InsertCommandProbe>();
  InsertCommandProbe* probe_ptr = probe.get();
  strip.add_pre_insert(std::move(probe));

  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock, /*command_capacity=*/128);
  REQUIRE(engine.set_track_lanes({{10}}));
  REQUIRE(engine.bind_track_strip(10, &strip));
  REQUIRE(engine.automation().bind_target(7, probe_ptr));

  const uint32_t stale_id = sonare::engine::make_insert_param_id(0, 0, 0);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  // Fill the first drain batch with unrelated future commands. The two stale
  // insert commands then remain in the command ring, followed by an unrelated
  // command whose FIFO position must survive the scrub.
  for (uint32_t i = 0; i < sonare::engine::RealtimeEngine::kMaxCommandsPerBlock - 1; ++i) {
    sonare::rt::Command filler{};
    filler.type = sonare::rt::CommandType::kSetParam;
    filler.target_id = 7;
    filler.sample_time = kFuture;
    filler.arg.f = static_cast<float>(i);
    REQUIRE(engine.push_command(filler));
  }
  sonare::rt::Command stale_generic{};
  stale_generic.type = sonare::rt::CommandType::kSetParam;
  stale_generic.target_id = stale_id;
  stale_generic.sample_time = kFuture;
  stale_generic.arg.f = 0.25f;
  REQUIRE(engine.push_command(stale_generic));

  sonare::rt::Command stale_legacy{};
  stale_legacy.type = sonare::rt::CommandType::kSetTrackInsertParam;
  stale_legacy.target_id = 0;  // lane 0, insert 0, param 0
  stale_legacy.sample_time = kFuture;
  stale_legacy.arg.f = 0.5f;
  REQUIRE(engine.push_command(stale_legacy));

  sonare::rt::Command unrelated{};
  unrelated.type = sonare::rt::CommandType::kSetParam;
  unrelated.target_id = 7;
  unrelated.sample_time = kFuture;
  unrelated.arg.f = 777.0f;
  REQUIRE(engine.push_command(unrelated));

  std::array<float, kBlock> output{};
  float* io[] = {output.data()};
  engine.process(io, 1, kBlock);  // drains 64; stale commands remain queued
  REQUIRE(engine.clear_track_insert_parameter_bases(10));

  for (int block = 0; block < 20; ++block) {
    output.fill(0.0f);
    engine.process(io, 1, kBlock);
  }

  // The unrelated command made it through the queue rotation, while both
  // legacy and tagged insert commands were discarded before they could claim
  // the insert smoother.
  REQUIRE(probe_ptr->unrelated_count > 0);
  REQUIRE(probe_ptr->unrelated_value == 777.0f);
  REQUIRE(probe_ptr->insert_count == 0);
  REQUIRE(probe_ptr->insert_value == 1.0f);

  // Repeat with commands that have already entered pending_. This exercises
  // the fixed-bank compaction path separately from queue rotation.
  const int64_t pending_time = engine.transport().render_frame() + 128;
  stale_generic.sample_time = pending_time;
  stale_generic.arg.f = 0.125f;
  REQUIRE(engine.push_command(stale_generic));
  stale_legacy.sample_time = pending_time;
  stale_legacy.arg.f = 0.875f;
  REQUIRE(engine.push_command(stale_legacy));
  engine.process(io, 1, kBlock);  // both stale commands are staged as future
  REQUIRE(engine.clear_track_insert_parameter_bases(10));
  for (int block = 0; block < 3; ++block) {
    output.fill(0.0f);
    engine.process(io, 1, kBlock);
  }
  REQUIRE(probe_ptr->insert_count == 0);
  REQUIRE(probe_ptr->insert_value == 1.0f);
}

TEST_CASE("Bus removal scrubs retired selectors while preserving active bus commands",
          "[engine][realtime][mixing]") {
  constexpr int kBlock = 64;
  constexpr int64_t kQueuedFuture = 1024;
  constexpr int64_t kPendingFuture = 2048;

  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock, /*command_capacity=*/128);
  REQUIRE(engine.set_track_buses({{1, 0.0f}, {2, 0.0f}}));

  sonare::mixing::api::Bus bus;
  bus.id = "1";
  bus.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PreFader, "utility.gain", R"({"levelDb":0})"});
  REQUIRE(engine.set_bus_strip(1, bus));
  bus.id = "2";
  REQUIRE(engine.set_bus_strip(2, bus));

  sonare::engine::TrackLaneConfig lane{10};
  lane.output_bus_id = 2;
  REQUIRE(engine.set_track_lanes({lane}));
  constexpr size_t kSourceFrames = static_cast<size_t>(kBlock * 64);
  std::array<float, kSourceFrames> source{};
  source.fill(1.0f);
  const float* source_channels[] = {source.data()};
  sonare::engine::ClipSchedule clip{};
  clip.id = 1;
  clip.track_id = 10;
  clip.buffer = {source_channels, 1, kSourceFrames};
  clip.length_samples = static_cast<int64_t>(kSourceFrames);
  clip.gain = 1.0f;
  engine.set_clips({clip});

  const int64_t old_bus_id = engine.resolve_bus_insert_automation_id(1, 0, "levelDb");
  const int64_t other_bus_id = engine.resolve_bus_insert_automation_id(2, 0, "levelDb");
  REQUIRE(old_bus_id >= 0);
  REQUIRE(other_bus_id >= 0);
  REQUIRE(old_bus_id != other_bus_id);

  const uint32_t old_target = static_cast<uint32_t>(old_bus_id);
  const uint32_t other_target = static_cast<uint32_t>(other_bus_id);
  auto command = [](uint32_t target, int64_t sample_time, float value) {
    sonare::rt::Command result{};
    result.type = sonare::rt::CommandType::kSetParam;
    result.target_id = target;
    result.sample_time = sample_time;
    result.arg.f = value;
    return result;
  };

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  // Fill the pending bank with valid commands for the other bus, then leave a
  // retired bus command in the ring. Removal must discard only the stale ring
  // record: the other bus's pending edits must remain FIFO and must not produce
  // a queue or pending overflow when the future timestamp arrives.
  std::array<float, kBlock> output{};
  float* io[] = {output.data()};
  engine.process(io, 1, kBlock);  // Start playback before scheduling edits.
  for (size_t i = 0; i < sonare::engine::RealtimeEngine::kMaxCommandsPerBlock; ++i) {
    REQUIRE(engine.push_command(command(other_target, kQueuedFuture, -6.0f)));
  }
  REQUIRE(engine.push_command(command(old_target, kQueuedFuture, -12.0f)));

  engine.process(io, 1, kBlock);  // 64 commands enter pending_; stale stays queued.
  REQUIRE(engine.set_track_buses({{2, 0.0f}}));
  REQUIRE(engine.set_track_buses({{2, 0.0f}, {1, 0.0f}}));
  bus.id = "1";
  REQUIRE(engine.set_bus_strip(1, bus));

  for (int block = 0; block < 20; ++block) {
    output.fill(0.0f);
    engine.process(io, 1, kBlock);
  }
  const float other_bus_level_after_remove = output.back();
  REQUIRE(other_bus_level_after_remove > 0.35f);
  REQUIRE(other_bus_level_after_remove < 0.70f);

  const int64_t new_bus_id = engine.resolve_bus_insert_automation_id(1, 0, "levelDb");
  REQUIRE(new_bus_id >= 0);
  REQUIRE(new_bus_id != old_bus_id);

  // Reintroduce an old-generation command deliberately after the re-add. The
  // clear API must scrub every historical selector for this identity, while a
  // valid command for bus 2 remains staged and applies without an error.
  REQUIRE(engine.push_command(command(old_target, kPendingFuture, -18.0f)));
  REQUIRE(engine.push_command(command(static_cast<uint32_t>(new_bus_id), kPendingFuture, -6.0f)));
  REQUIRE(engine.push_command(command(other_target, kPendingFuture, -3.0f)));
  engine.process(io, 1, kBlock);  // Stage all three future commands in pending_.
  REQUIRE(engine.clear_bus_insert_parameter_bases(1));

  for (int block = 0; block < 20; ++block) {
    output.fill(0.0f);
    engine.process(io, 1, kBlock);
  }
  // The command for the still-active bus survived both scrubs and was applied
  // after the retained -6 dB value, so this final level is audibly higher.
  REQUIRE(output.back() > other_bus_level_after_remove + 0.10f);
  REQUIRE(output.back() < 0.85f);

  int unknown_target_count = 0;
  int overflow_count = 0;
  sonare::engine::Telemetry telemetry{};
  while (engine.pop_telemetry(telemetry)) {
    if (telemetry.error == sonare::engine::TelemetryErrorCode::kUnknownTarget &&
        (telemetry.value == old_target || telemetry.value == static_cast<uint32_t>(new_bus_id))) {
      ++unknown_target_count;
    }
    if (telemetry.error == sonare::engine::TelemetryErrorCode::kCommandQueueOverflow ||
        telemetry.error == sonare::engine::TelemetryErrorCode::kPendingCommandOverflow) {
      ++overflow_count;
    }
  }
  REQUIRE(unknown_target_count == 0);
  REQUIRE(overflow_count == 0);
}

TEST_CASE("Queued track insert edits follow the track through a lane reorder",
          "[engine][realtime][mixing]") {
  constexpr int kBlock = 64;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  REQUIRE(engine.set_track_lanes({{10}, {20}}));

  sonare::mixing::ChannelStrip track_a;
  sonare::mixing::ChannelStrip track_b;
  auto probe_a = std::make_unique<InsertCommandProbe>();
  auto probe_b = std::make_unique<InsertCommandProbe>();
  InsertCommandProbe* probe_a_ptr = probe_a.get();
  InsertCommandProbe* probe_b_ptr = probe_b.get();
  track_a.add_pre_insert(std::move(probe_a));
  track_b.add_pre_insert(std::move(probe_b));
  REQUIRE(engine.bind_track_strip(10, &track_a));
  REQUIRE(engine.bind_track_strip(20, &track_b));

  // Resolve by track id while the original lane order is still in force, then
  // reorder before the audio/control command is drained. The command must keep
  // naming track 10 instead of silently becoming lane 0 (track 20).
  REQUIRE(engine.set_track_insert_param_detailed(10, 0, "gain", 0.25f) ==
          sonare::engine::InsertParamSetResult::kQueued);
  sonare::rt::Command legacy{};
  legacy.type = sonare::rt::CommandType::kSetTrackInsertParam;
  legacy.target_id = (1u << 16u) | 0u;  // lane 1, insert 0, param 0
  legacy.sample_time = -1;
  legacy.arg.f = 0.5f;
  REQUIRE(engine.push_command(legacy));
  REQUIRE(engine.set_track_lanes({{20}, {10}}));
  engine.flush_control_commands();
  engine.settle_parameters();

  REQUIRE(probe_a_ptr->insert_value == Catch::Approx(0.25f));
  REQUIRE(probe_b_ptr->insert_value == Catch::Approx(0.5f));

  // The raw legacy command must retain the same manual base as the generic
  // helper. Automation release should return track 20 to 0.5, not its
  // construction value of 1.0.
  const int64_t target = engine.resolve_track_insert_automation_id(20, 0, "gain");
  REQUIRE(target >= 0);
  sonare::automation::AutomationLane automated(static_cast<uint32_t>(target));
  automated.set_points({{0.0, 0.75f, sonare::automation::CurveType::Hold}});
  engine.automation().set_lanes({automated});
  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));
  std::array<float, kBlock> output{};
  float* io[] = {output.data()};
  engine.process(io, 1, kBlock);
  engine.settle_parameters();
  REQUIRE(probe_b_ptr->insert_value == Catch::Approx(0.75f));

  sonare::automation::AutomationLane released(static_cast<uint32_t>(target));
  engine.automation().set_lanes({released});
  engine.process(io, 1, kBlock);
  engine.settle_parameters();
  REQUIRE(probe_b_ptr->insert_value == Catch::Approx(0.5f));
}

TEST_CASE("Published track insert automation follows the track through a lane reorder",
          "[engine][realtime][mixing]") {
  constexpr int kBlock = 64;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  REQUIRE(engine.set_track_lanes({{10}, {20}}));

  sonare::mixing::ChannelStrip track_a;
  sonare::mixing::ChannelStrip track_b;
  auto probe_a = std::make_unique<InsertCommandProbe>();
  auto probe_b = std::make_unique<InsertCommandProbe>();
  InsertCommandProbe* probe_a_ptr = probe_a.get();
  InsertCommandProbe* probe_b_ptr = probe_b.get();
  track_a.add_pre_insert(std::move(probe_a));
  track_b.add_pre_insert(std::move(probe_b));
  REQUIRE(engine.bind_track_strip(10, &track_a));
  REQUIRE(engine.bind_track_strip(20, &track_b));

  const int64_t target = engine.resolve_track_insert_automation_id(10, 0, "gain");
  REQUIRE(target >= 0);
  sonare::automation::AutomationLane lane(static_cast<uint32_t>(target));
  lane.set_points({{0.0, 0.25f, sonare::automation::CurveType::Hold}});
  engine.automation().set_lanes({lane});

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));
  // The lane is already published when the control-thread reorder occurs. It
  // must retain track 10's identity instead of following the old lane 0 slot.
  REQUIRE(engine.set_track_lanes({{20}, {10}}));

  std::array<float, kBlock> output{};
  float* io[] = {output.data()};
  engine.process(io, 1, kBlock);
  engine.settle_parameters();

  REQUIRE(probe_a_ptr->insert_value == Catch::Approx(0.25f));
  REQUIRE(probe_b_ptr->insert_value == Catch::Approx(1.0f));
}

TEST_CASE("Track insert automation smoother keeps continuity through a lane reorder",
          "[engine][realtime][mixing]") {
  constexpr int kBlock = 32;
  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, kBlock);
  REQUIRE(mixer.set_track_lanes({{10}, {20}}));

  sonare::mixing::api::Strip strip;
  strip.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PreFader, "utility.gain", R"({"levelDb":0})"});
  REQUIRE(mixer.set_track_strip(10, strip));
  mixer.settle_smoothers();

  size_t lane_index = 0;
  unsigned int param_id = 0;
  REQUIRE(mixer.resolve_track_insert_param(10, 0, "levelDb", &lane_index, &param_id));
  REQUIRE(mixer.route_lane_insert_param_smoothed(lane_index, 0, param_id, -12.0f));
  mixer.settle_insert_automations();

  std::array<float, kBlock> source{};
  std::array<float, kBlock> output{};
  source.fill(1.0f);
  float* source_channels[] = {source.data()};
  float* output_channels[] = {output.data()};
  REQUIRE(mixer.mix_source(10, source_channels, output_channels, 1, kBlock));
  const float before_reorder = output.back();
  REQUIRE(before_reorder == Catch::Approx(std::pow(10.0f, -12.0f / 20.0f)));

  // The active automation target is addressed again after the selector moves
  // from lane 0 to lane 1. Rendering this block must retain the settled value;
  // reclaiming a fresh slot would glide from the insert's construction value.
  REQUIRE(mixer.set_track_lanes({{20}, {10}}));
  REQUIRE(mixer.resolve_track_insert_param(10, 0, "levelDb", &lane_index, &param_id));
  REQUIRE(lane_index == 1);
  REQUIRE(mixer.route_lane_insert_param_smoothed(lane_index, 0, param_id, -12.0f));
  output.fill(0.0f);
  REQUIRE(mixer.mix_source(10, source_channels, output_channels, 1, kBlock));
  REQUIRE(output.back() == Catch::Approx(before_reorder));
}

TEST_CASE("Removed published track automation does not retarget a replacement lane",
          "[engine][realtime][mixing]") {
  constexpr int kBlock = 64;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  REQUIRE(engine.set_track_lanes({{10}, {20}}));

  sonare::mixing::ChannelStrip old_track;
  sonare::mixing::ChannelStrip retained_track;
  auto old_probe = std::make_unique<InsertCommandProbe>();
  auto retained_probe = std::make_unique<InsertCommandProbe>();
  InsertCommandProbe* old_probe_ptr = old_probe.get();
  InsertCommandProbe* retained_probe_ptr = retained_probe.get();
  old_track.add_pre_insert(std::move(old_probe));
  retained_track.add_pre_insert(std::move(retained_probe));
  REQUIRE(engine.bind_track_strip(10, &old_track));
  REQUIRE(engine.bind_track_strip(20, &retained_track));

  const int64_t target = engine.resolve_track_insert_automation_id(10, 0, "gain");
  REQUIRE(target >= 0);
  sonare::automation::AutomationLane lane(static_cast<uint32_t>(target));
  lane.set_points({{0.0, 0.25f, sonare::automation::CurveType::Hold}});
  engine.automation().set_lanes({lane});

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));
  REQUIRE(engine.set_track_lanes({{20}}));

  std::array<float, kBlock> output{};
  float* io[] = {output.data()};
  engine.process(io, 1, kBlock);
  engine.settle_parameters();

  REQUIRE(old_probe_ptr->insert_value == Catch::Approx(1.0f));
  REQUIRE(retained_probe_ptr->insert_value == Catch::Approx(1.0f));
}

TEST_CASE("Future queued track insert edits follow the track through a lane reorder",
          "[engine][realtime][mixing]") {
  constexpr int kBlock = 64;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  REQUIRE(engine.set_track_lanes({{10}, {20}}));

  sonare::mixing::ChannelStrip track_a;
  sonare::mixing::ChannelStrip track_b;
  auto probe_a = std::make_unique<InsertCommandProbe>();
  auto probe_b = std::make_unique<InsertCommandProbe>();
  InsertCommandProbe* probe_a_ptr = probe_a.get();
  InsertCommandProbe* probe_b_ptr = probe_b.get();
  track_a.add_pre_insert(std::move(probe_a));
  track_b.add_pre_insert(std::move(probe_b));
  REQUIRE(engine.bind_track_strip(10, &track_a));
  REQUIRE(engine.bind_track_strip(20, &track_b));

  sonare::rt::Command future{};
  future.type = sonare::rt::CommandType::kSetParam;
  future.target_id = sonare::engine::make_insert_param_id(0, 0, 0);
  future.sample_time = engine.transport().render_frame() + 2 * kBlock;
  future.arg.f = 0.25f;
  REQUIRE(engine.push_command(future));
  std::array<float, kBlock> output{};
  float* io[] = {output.data()};
  engine.process(io, 1, kBlock);  // Stage the future command in pending_.

  REQUIRE(engine.set_track_lanes({{20}, {10}}));
  for (int block = 0; block < 3; ++block) engine.process(io, 1, kBlock);
  engine.settle_parameters();

  REQUIRE(probe_a_ptr->insert_value == Catch::Approx(0.25f));
  REQUIRE(probe_b_ptr->insert_value == Catch::Approx(1.0f));
}

TEST_CASE("Queued generic lane fader and pan edits follow a lane reorder",
          "[engine][realtime][mixing]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 8;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  REQUIRE_FALSE(engine.set_track_lanes({{0}}));

  // Give the two tracks different levels so applying the commands to the
  // wrong positional lane remains observable after the reorder. Track 10 is
  // deliberately louder and is the track that receives both generic edits.
  std::array<float, kFrames> track_a_l{};
  std::array<float, kFrames> track_a_r{};
  std::array<float, kFrames> track_b_l{};
  std::array<float, kFrames> track_b_r{};
  track_a_l.fill(2.0f);
  track_a_r.fill(2.0f);
  track_b_l.fill(1.0f);
  track_b_r.fill(1.0f);
  const float* a[] = {track_a_l.data(), track_a_r.data()};
  const float* b[] = {track_b_l.data(), track_b_r.data()};
  sonare::engine::ClipSchedule clip_a{1, {a, 2, kFrames}, 0.0, 0, 0, kFrames, false, 1.0f, 0, 0};
  clip_a.track_id = 10;
  sonare::engine::ClipSchedule clip_b{2, {b, 2, kFrames}, 0.0, 0, 0, kFrames, false, 1.0f, 0, 0};
  clip_b.track_id = 20;
  engine.set_clips({clip_a, clip_b});
  REQUIRE(engine.set_track_lanes({{10}, {20}}));

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));
  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* io[] = {left.data(), right.data()};
  engine.process(io, 2, kBlock);

  sonare::rt::Command fader{};
  fader.type = sonare::rt::CommandType::kSetParam;
  fader.target_id = engine_lane_param_target(0, sonare::engine::TrackMixerRuntime::kFaderDb);
  fader.sample_time = -1;
  fader.arg.f = -12.0f;
  REQUIRE(engine.push_command(fader));

  sonare::rt::Command pan{};
  pan.type = sonare::rt::CommandType::kSetParam;
  pan.target_id = engine_lane_param_target(0, sonare::engine::TrackMixerRuntime::kPan);
  pan.sample_time = -1;
  pan.arg.f = 1.0f;
  REQUIRE(engine.push_command(pan));

  // Both commands still carry the original lane 0 selector. Remapping them
  // before the next process block must leave track 20 centered and apply the
  // gain/pan pair to track 10 now at lane 1.
  REQUIRE(engine.set_track_lanes({{20}, {10}}));
  left.fill(0.0f);
  right.fill(0.0f);
  engine.process(io, 2, kBlock);

  REQUIRE(left.back() < 1.6f);
  REQUIRE(right.back() > left.back() + 0.2f);
}

TEST_CASE("Queued track insert edits remain the latest manual base after automation release",
          "[engine][realtime][mixing]") {
  constexpr int kBlock = 64;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  REQUIRE(engine.set_track_lanes({{10}}));

  sonare::mixing::ChannelStrip strip;
  auto probe = std::make_unique<InsertCommandProbe>();
  InsertCommandProbe* probe_ptr = probe.get();
  strip.add_pre_insert(std::move(probe));
  REQUIRE(engine.bind_track_strip(10, &strip));
  REQUIRE(engine.set_track_insert_param_detailed(10, 0, "gain", 0.25f) ==
          sonare::engine::InsertParamSetResult::kQueued);
  engine.flush_control_commands();
  engine.settle_parameters();
  REQUIRE(probe_ptr->insert_value == Catch::Approx(0.25f));

  const int64_t target = engine.resolve_track_insert_automation_id(10, 0, "gain");
  REQUIRE(target >= 0);
  sonare::automation::AutomationLane lane(static_cast<uint32_t>(target));
  lane.set_points({{0.0, 0.75f, sonare::automation::CurveType::Hold}});
  engine.automation().set_lanes({lane});

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));
  std::array<float, kBlock> output{};
  float* io[] = {output.data()};
  engine.process(io, 1, kBlock);
  engine.settle_parameters();
  REQUIRE(probe_ptr->insert_value == Catch::Approx(0.75f));

  sonare::automation::AutomationLane emptied(static_cast<uint32_t>(target));
  engine.automation().set_lanes({emptied});
  engine.process(io, 1, kBlock);
  engine.settle_parameters();
  REQUIRE(probe_ptr->insert_value == Catch::Approx(0.25f));
}

TEST_CASE("Removed track selectors do not retarget a replacement lane",
          "[engine][realtime][mixing]") {
  constexpr int kBlock = 64;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  REQUIRE(engine.set_track_lanes({{10}, {20}}));

  sonare::mixing::ChannelStrip old_strip;
  auto old_probe = std::make_unique<InsertCommandProbe>();
  InsertCommandProbe* old_probe_ptr = old_probe.get();
  old_strip.add_pre_insert(std::move(old_probe));
  sonare::mixing::ChannelStrip retained_strip;
  auto retained_probe = std::make_unique<InsertCommandProbe>();
  InsertCommandProbe* retained_probe_ptr = retained_probe.get();
  retained_strip.add_pre_insert(std::move(retained_probe));
  REQUIRE(engine.bind_track_strip(10, &old_strip));
  REQUIRE(engine.bind_track_strip(20, &retained_strip));

  // Record a base for the removed track and leave a second edit queued. Both
  // the old base and the queued command use selector 0, which the replacement
  // track will occupy after the republish.
  REQUIRE(engine.apply_track_insert_param_by_name_now(10, 0, "gain", 0.25f));
  engine.settle_insert_parameters();
  REQUIRE(engine.set_track_insert_param_detailed(10, 0, "gain", 0.5f) ==
          sonare::engine::InsertParamSetResult::kQueued);
  REQUIRE(old_probe_ptr->insert_value == Catch::Approx(0.25f));

  REQUIRE(engine.set_track_lanes({{30}, {20}}));
  sonare::mixing::ChannelStrip replacement_strip;
  auto replacement_probe = std::make_unique<InsertCommandProbe>();
  InsertCommandProbe* replacement_probe_ptr = replacement_probe.get();
  replacement_strip.add_pre_insert(std::move(replacement_probe));
  REQUIRE(engine.bind_track_strip(30, &replacement_strip));
  engine.flush_control_commands();
  engine.settle_parameters();

  REQUIRE(retained_probe_ptr->insert_value == Catch::Approx(1.0f));
  REQUIRE(replacement_probe_ptr->insert_value == Catch::Approx(1.0f));
}

TEST_CASE("Removed and re-added track selectors drop queued commands and bases",
          "[engine][realtime][mixing]") {
  constexpr int kBlock = 64;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  REQUIRE(engine.set_track_lanes({{10}, {20}}));

  sonare::mixing::ChannelStrip old_strip;
  auto old_probe = std::make_unique<InsertCommandProbe>();
  old_strip.add_pre_insert(std::move(old_probe));
  sonare::mixing::ChannelStrip retained_strip;
  auto retained_probe = std::make_unique<InsertCommandProbe>();
  InsertCommandProbe* retained_probe_ptr = retained_probe.get();
  retained_strip.add_pre_insert(std::move(retained_probe));
  REQUIRE(engine.bind_track_strip(10, &old_strip));
  REQUIRE(engine.bind_track_strip(20, &retained_strip));

  REQUIRE(engine.apply_track_insert_param_by_name_now(10, 0, "gain", 0.25f));
  engine.settle_insert_parameters();
  REQUIRE(engine.set_track_insert_param_detailed(10, 0, "gain", 0.5f) ==
          sonare::engine::InsertParamSetResult::kQueued);

  REQUIRE(engine.set_track_lanes({{20}}));
  REQUIRE(engine.set_track_lanes({{10}, {20}}));
  sonare::mixing::ChannelStrip readded_strip;
  auto readded_probe = std::make_unique<InsertCommandProbe>();
  InsertCommandProbe* readded_probe_ptr = readded_probe.get();
  readded_strip.add_pre_insert(std::move(readded_probe));
  REQUIRE(engine.bind_track_strip(10, &readded_strip));
  engine.flush_control_commands();
  engine.settle_parameters();
  REQUIRE(retained_probe_ptr->insert_value == Catch::Approx(1.0f));
  REQUIRE(readded_probe_ptr->insert_value == Catch::Approx(1.0f));

  const int64_t target = engine.resolve_track_insert_automation_id(10, 0, "gain");
  REQUIRE(target >= 0);
  sonare::automation::AutomationLane automated(static_cast<uint32_t>(target));
  automated.set_points({{0.0, 0.75f, sonare::automation::CurveType::Hold}});
  engine.automation().set_lanes({automated});
  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));
  std::array<float, kBlock> output{};
  float* io[] = {output.data()};
  engine.process(io, 1, kBlock);
  engine.settle_parameters();
  REQUIRE(readded_probe_ptr->insert_value == Catch::Approx(0.75f));

  engine.automation().set_lanes(
      {sonare::automation::AutomationLane(static_cast<uint32_t>(target))});
  engine.process(io, 1, kBlock);
  engine.settle_parameters();
  // The removed track's 0.25 base was erased; release leaves the new track's
  // current 0.75 value in place instead of restoring stale state.
  REQUIRE(readded_probe_ptr->insert_value == Catch::Approx(0.75f));
}

TEST_CASE("Queued master insert edits remain the latest manual base after automation release",
          "[engine][realtime][mixing]") {
  constexpr int kBlock = 64;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  sonare::mixing::api::Strip master;
  master.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PreFader, "utility.gain", R"({"levelDb":0})"});
  REQUIRE(engine.set_master_strip(master));
  const int64_t target = engine.resolve_master_insert_automation_id(0, "levelDb");
  REQUIRE(target >= 0);

  REQUIRE(engine.set_master_insert_param_detailed(0, "levelDb", -6.0f) ==
          sonare::engine::InsertParamSetResult::kQueued);
  engine.flush_control_commands();
  engine.settle_parameters();

  sonare::automation::AutomationLane lane(static_cast<uint32_t>(target));
  lane.set_points({{0.0, -18.0f, sonare::automation::CurveType::Hold}});
  engine.automation().set_lanes({lane});
  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));
  std::array<float, kBlock> output{};
  float* io[] = {output.data()};
  engine.process(io, 1, kBlock);
  engine.settle_parameters();

  sonare::automation::AutomationLane emptied(static_cast<uint32_t>(target));
  engine.automation().set_lanes({emptied});
  engine.process(io, 1, kBlock);
  engine.settle_parameters();
  // The queued manual -6 dB must be restored; the construction default is 0 dB.
  output.fill(1.0f);
  engine.process(io, 1, kBlock);
  REQUIRE(output.back() == Catch::Approx(std::pow(10.0f, -6.0f / 20.0f)).margin(0.02f));
}

TEST_CASE("First master insert touch ramps from the retained manual base",
          "[engine][realtime][mixing]") {
  constexpr int kBlock = 64;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  sonare::mixing::api::Strip master;
  master.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PreFader, "utility.gain", R"({"levelDb":0})"});
  REQUIRE(engine.set_master_strip(master));
  REQUIRE(engine.apply_master_insert_param_by_name_now(0, "levelDb", -6.0f));
  engine.settle_insert_parameters();

  // Replacing the chain clears the assigned smoother slot while retaining the
  // manual base for the same reserved master id. The next first touch must
  // start from -6 dB, rather than snapping the fresh strip to its -12 dB target.
  auto replacement = master;
  replacement.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PreFader, "utility.gain", R"({"levelDb":0})"});
  REQUIRE(engine.set_master_strip(replacement));
  REQUIRE(engine.set_master_insert_param_detailed(0, "levelDb", -12.0f) ==
          sonare::engine::InsertParamSetResult::kQueued);
  engine.flush_control_commands();

  std::array<float, kBlock> output{};
  output.fill(1.0f);
  float* io[] = {output.data()};
  engine.process(io, 1, kBlock);
  const float target_gain = std::pow(10.0f, -12.0f / 20.0f);
  REQUIRE(output.back() > target_gain + 0.02f);
  REQUIRE(output.back() < std::pow(10.0f, -6.0f / 20.0f));
}

TEST_CASE("Rejected master insert restore preserves its automation slot",
          "[engine][realtime][mixing]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 64);

  sonare::mixing::api::Strip master;
  master.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PreFader, "effects.delay.stereo", "{}"});
  REQUIRE(engine.set_master_strip(master));

  // The delay's realtime-safe invertL parameter rejects fractional values. The
  // engine may still have a live smoother slot for a previously published
  // target; restore must leave that slot assigned when the processor rejects
  // the edit.
  constexpr size_t kSlots = 16;
  const std::array<const char*, kSlots> keys = {
      "delayTimeLMs", "delayTimeRMs", "feedback",    "pingPong",    "dryWet",      "dampingHz",
      "tap3Ms",       "tap4Ms",       "tap1LevelDb", "tap2LevelDb", "tap3LevelDb", "tap4LevelDb",
      "tap3Pan",      "tap4Pan",      "invertL",     "invertR"};
  std::array<int64_t, kSlots> targets{};
  for (size_t i = 0; i < targets.size(); ++i) {
    const int64_t target = engine.resolve_master_insert_automation_id(0, keys[i]);
    REQUIRE(target >= 0);
    targets[i] = target;
    REQUIRE(engine.automation().set_parameter(static_cast<uint32_t>(target), -12.0f));
  }

  REQUIRE_FALSE(engine.restore_master_insert_param_by_name(0, "invertL", 0.5f));

  // All sixteen smoother slots are still occupied. A seventeenth target must
  // therefore be refused instead of reusing the slot whose restore failed.
  const int64_t seventeenth = engine.resolve_master_insert_automation_id(0, "modRateHz");
  REQUIRE(seventeenth >= 0);
  REQUIRE_FALSE(engine.automation().set_parameter(static_cast<uint32_t>(seventeenth), -12.0f));
  REQUIRE(engine.insert_automation_overflow_count() == 1);
}

TEST_CASE("First master insert automation seeds from the processor's last applied value",
          "[engine][realtime][mixing]") {
  constexpr int kBlock = 64;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  sonare::mixing::api::Strip master;
  master.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PreFader, "utility.gain", R"({"levelDb":0})"});
  REQUIRE(engine.set_master_strip(master));
  const int64_t target = engine.resolve_master_insert_automation_id(0, "levelDb");
  REQUIRE(target >= 0);

  // This direct processor edit updates last_applied without adding a manual
  // base-table entry. The first smoother claim must still begin at -6 dB.
  REQUIRE(engine.mixing().strip() != nullptr);
  REQUIRE(engine.mixing().strip()->apply_insert_parameter(0, 0, -6.0f));
  REQUIRE(engine.automation().set_parameter(static_cast<uint32_t>(target), -12.0f));

  std::array<float, kBlock> output{};
  output.fill(1.0f);
  float* io[] = {output.data()};
  engine.process(io, 1, kBlock);
  const float target_gain = std::pow(10.0f, -12.0f / 20.0f);
  REQUIRE(output.back() > target_gain + 0.02f);
  REQUIRE(output.back() < std::pow(10.0f, -6.0f / 20.0f));
}

TEST_CASE("Settled master insert automation ignores a stale manual base",
          "[engine][realtime][mixing]") {
  constexpr int kBlock = 64;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  sonare::mixing::api::Strip master;
  master.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PreFader, "utility.gain", R"({"levelDb":0})"});
  REQUIRE(engine.set_master_strip(master));
  REQUIRE(engine.apply_master_insert_param_by_name_now(0, "levelDb", -6.0f));
  engine.settle_insert_parameters();

  const int64_t target = engine.resolve_master_insert_automation_id(0, "levelDb");
  REQUIRE(target >= 0);
  REQUIRE(engine.automation().set_parameter(static_cast<uint32_t>(target), -12.0f));
  engine.settle_insert_parameters();
  REQUIRE(engine.automation().set_parameter(static_cast<uint32_t>(target), -18.0f));

  std::array<float, kBlock> output{};
  output.fill(1.0f);
  float* io[] = {output.data()};
  engine.process(io, 1, kBlock);
  const float target_gain = std::pow(10.0f, -18.0f / 20.0f);
  REQUIRE(output.back() > target_gain + 0.02f);
  // The previous -6 dB manual base must not be reused for this settled slot.
  REQUIRE(output.back() < 0.30f);
}

TEST_CASE("Non-realtime insert parameters are rejected before queueing",
          "[engine][realtime][mixing]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 64);

  const sonare::mixing::api::Insert non_realtime_insert{sonare::mixing::api::InsertSlot::PreFader,
                                                        "eq.linearPhase", "{}"};

  REQUIRE(engine.set_track_lanes({{10}}));
  sonare::mixing::api::Strip track;
  track.inserts.push_back(non_realtime_insert);
  REQUIRE(engine.set_track_strip(10, track));
  REQUIRE(engine.resolve_track_insert_automation_id(10, 0, "band0.frequencyHz") == -1);
  REQUIRE(engine.set_track_insert_param_detailed(10, 0, "band0.frequencyHz", 1200.0f) ==
          sonare::engine::InsertParamSetResult::kInvalidTarget);
  REQUIRE_FALSE(engine.set_track_insert_param(10, 0, "band0.frequencyHz", 1200.0f));

  REQUIRE(engine.set_track_buses({{20, 0.0f}}));
  sonare::mixing::api::Bus bus;
  bus.inserts.push_back(non_realtime_insert);
  REQUIRE(engine.set_bus_strip(20, bus));
  REQUIRE(engine.resolve_bus_insert_automation_id(20, 0, "band0.frequencyHz") == -1);
  REQUIRE(engine.set_bus_insert_param_detailed(20, 0, "band0.frequencyHz", 1200.0f) ==
          sonare::engine::InsertParamSetResult::kInvalidTarget);
  REQUIRE_FALSE(engine.set_bus_insert_param(20, 0, "band0.frequencyHz", 1200.0f));

  sonare::mixing::api::Strip master;
  master.inserts.push_back(non_realtime_insert);
  REQUIRE(engine.set_master_strip(master));
  REQUIRE(engine.resolve_master_insert_automation_id(0, "band0.frequencyHz") == -1);
  REQUIRE(engine.set_master_insert_param_detailed(0, "band0.frequencyHz", 1200.0f) ==
          sonare::engine::InsertParamSetResult::kInvalidTarget);
  REQUIRE_FALSE(engine.set_master_insert_param(0, "band0.frequencyHz", 1200.0f));
}
#endif

TEST_CASE(
    "RealtimeEngine restores the manual base value when its lane empties, "
    "manual value sent before the clear",
    "[engine][realtime]") {
  constexpr int kFrames = 64;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kFrames);

  CaptureProcessor processor;
  engine.automation().bind_target(7, &processor);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  sonare::rt::Command manual{};
  manual.type = sonare::rt::CommandType::kSetParam;
  manual.target_id = 7;
  manual.sample_time = -1;
  manual.arg.f = -6.0f;
  REQUIRE(engine.push_command(manual));

  sonare::automation::AutomationLane lane(7);
  lane.set_points({{0.0, -12.0f, sonare::automation::CurveType::Hold}});
  engine.automation().set_lanes({lane});

  std::array<float, kFrames> left{};
  float* io[] = {left.data()};
  engine.process(io, 1, kFrames);
  engine.settle_parameters();

  // The manual value is recorded as the base, but the lane is still alive
  // this block and overwrites it.
  REQUIRE(processor.count > 0);
  REQUIRE(processor.values[static_cast<size_t>(processor.count - 1)] == -12.0f);

  sonare::automation::AutomationLane emptied(7);  // clear: no points.
  engine.automation().set_lanes({emptied});
  engine.process(io, 1, kFrames);
  engine.settle_parameters();

  // Once the lane empties, the target reverts to the recorded manual value.
  REQUIRE(processor.values[static_cast<size_t>(processor.count - 1)] == -6.0f);
}

TEST_CASE(
    "RealtimeEngine applies a manual value sent after its lane clears "
    "with no base recorded yet",
    "[engine][realtime]") {
  constexpr int kFrames = 64;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kFrames);

  CaptureProcessor processor;
  engine.automation().bind_target(7, &processor);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  sonare::automation::AutomationLane lane(7);
  lane.set_points({{0.0, -20.0f, sonare::automation::CurveType::Hold}});
  engine.automation().set_lanes({lane});

  std::array<float, kFrames> left{};
  float* io[] = {left.data()};
  engine.process(io, 1, kFrames);
  engine.settle_parameters();
  REQUIRE(processor.count > 0);
  REQUIRE(processor.values[static_cast<size_t>(processor.count - 1)] == -20.0f);

  // Clear the lane before any manual value was ever sent: no base is
  // recorded, so the release is a no-op and the last value is left as-is.
  sonare::automation::AutomationLane emptied(7);
  engine.automation().set_lanes({emptied});
  const int count_before_clear = processor.count;
  engine.process(io, 1, kFrames);
  engine.settle_parameters();
  REQUIRE(processor.count == count_before_clear);

  // The manual value sent afterward applies directly -- nothing is left to
  // drive it back.
  sonare::rt::Command manual{};
  manual.type = sonare::rt::CommandType::kSetParam;
  manual.target_id = 7;
  manual.sample_time = -1;
  manual.arg.f = -6.0f;
  REQUIRE(engine.push_command(manual));
  engine.process(io, 1, kFrames);
  engine.settle_parameters();
  REQUIRE(processor.values[static_cast<size_t>(processor.count - 1)] == -6.0f);
}

TEST_CASE(
    "RealtimeEngine leaves a target's value unchanged when its lane clears "
    "and no manual value was ever sent",
    "[engine][realtime]") {
  constexpr int kFrames = 64;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kFrames);

  CaptureProcessor processor;
  engine.automation().bind_target(7, &processor);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  sonare::automation::AutomationLane lane(7);
  lane.set_points({{0.0, -20.0f, sonare::automation::CurveType::Hold}});
  engine.automation().set_lanes({lane});

  std::array<float, kFrames> left{};
  float* io[] = {left.data()};
  engine.process(io, 1, kFrames);
  engine.settle_parameters();
  REQUIRE(processor.count > 0);
  REQUIRE(processor.values[static_cast<size_t>(processor.count - 1)] == -20.0f);

  sonare::automation::AutomationLane emptied(7);
  engine.automation().set_lanes({emptied});
  const int count_before_clear = processor.count;
  engine.process(io, 1, kFrames);
  engine.settle_parameters();

  REQUIRE(processor.count == count_before_clear);  // release found nothing to restore.
  REQUIRE(processor.values[static_cast<size_t>(processor.count - 1)] == -20.0f);
}

TEST_CASE(
    "RealtimeEngine restores a smoothed command's ramp target, not its "
    "in-flight value",
    "[engine][realtime]") {
  constexpr int kFrames = 64;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kFrames);

  CaptureProcessor processor;
  engine.automation().bind_target(7, &processor);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  sonare::rt::Command manual{};
  manual.type = sonare::rt::CommandType::kSetParamSmoothed;
  manual.target_id = 7;
  manual.sample_time = -1;
  manual.arg.f = -40.0f;
  REQUIRE(engine.push_command(manual));

  std::array<float, kFrames> left{};
  float* io[] = {left.data()};
  engine.process(io, 1, kFrames);
  // The base value recorded by apply_command() is the ramp's target
  // (command.arg.f), set at command-apply time -- well before this settle
  // forces the ramp itself to reach it.
  engine.settle_parameters();
  REQUIRE(processor.count > 0);
  REQUIRE(processor.values[static_cast<size_t>(processor.count - 1)] == -40.0f);

  // A lane now drives the same target to a different value.
  sonare::automation::AutomationLane lane(7);
  lane.set_points({{0.0, 5.0f, sonare::automation::CurveType::Hold}});
  engine.automation().set_lanes({lane});
  engine.process(io, 1, kFrames);
  engine.settle_parameters();
  REQUIRE(processor.values[static_cast<size_t>(processor.count - 1)] == 5.0f);

  // Clearing it restores the smoothed command's recorded target.
  sonare::automation::AutomationLane emptied(7);
  engine.automation().set_lanes({emptied});
  engine.process(io, 1, kFrames);
  engine.settle_parameters();
  REQUIRE(processor.values[static_cast<size_t>(processor.count - 1)] == -40.0f);
}

TEST_CASE("RealtimeEngine re-prepare at a new sample rate stays consistent",
          "[engine][realtime][reinit]") {
  static constexpr int kFrames = 128;
  sonare::engine::RealtimeEngine engine;

  // Stable clip backing storage that outlives both prepare cycles.
  std::array<float, kFrames> clip_l{};
  std::array<float, kFrames> clip_r{};
  clip_l.fill(0.25f);
  clip_r.fill(-0.25f);
  const float* clip_channels[] = {clip_l.data(), clip_r.data()};

  auto play_and_check = [&](double sample_rate) {
    engine.prepare(sample_rate, kFrames);
    REQUIRE(engine.max_block_size() == kFrames);

    engine.set_tempo(120.0);
    engine.set_time_signature(4, 4);
    std::vector<sonare::engine::ClipSchedule> clips;
    clips.emplace_back(1u, sonare::engine::ClipAudioBuffer{clip_channels, 2, kFrames}, 0.0, 0, 0,
                       kFrames, false, 1.0f, 0, 0);
    engine.set_clips(std::move(clips));

    sonare::rt::Command play{};
    play.type = sonare::rt::CommandType::kTransportPlay;
    play.sample_time = -1;
    REQUIRE(engine.push_command(play));

    std::array<float, kFrames> left{};
    std::array<float, kFrames> right{};
    float* io[] = {left.data(), right.data()};
    engine.process(io, 2, kFrames);

    for (int i = 0; i < kFrames; ++i) {
      REQUIRE(std::isfinite(left[static_cast<size_t>(i)]));
      REQUIRE(std::isfinite(right[static_cast<size_t>(i)]));
    }
    // Timeline advanced by exactly one block (sample-rate independent in
    // samples) and the render clock matches.
    REQUIRE(engine.transport().render_frame() == kFrames);
    REQUIRE(engine.transport().sample_position() == kFrames);
    REQUIRE(engine.clip_count() == 1);
  };

  play_and_check(48000.0);
  // Re-prepare at a different rate; state must reset cleanly and stay sane.
  play_and_check(44100.0);
  // And back again, to cover both transition directions.
  play_and_check(48000.0);
}

TEST_CASE("TempoMap re-prepare adopts the new sample rate", "[engine][realtime][reinit]") {
  sonare::transport::TempoMap map;
  map.prepare(48000.0);
  map.set_segments({{0.0, 120.0, 0.0}});
  // 120 BPM at 48 kHz -> one quarter note is 24000 samples.
  REQUIRE(map.ppq_to_sample(1.0) == 24000);

  // Re-prepare at 44.1 kHz: the same musical position now maps to fewer
  // samples (44100 * 60 / 120 = 22050).
  map.prepare(44100.0);
  REQUIRE(map.sample_rate() == 44100.0);
  REQUIRE(map.ppq_to_sample(1.0) == 22050);
  REQUIRE(map.ppq_to_sample(0.0) == 0);
}

TEST_CASE("RealtimeEngine schedules clips from the latest tempo snapshot", "[engine][realtime]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 16;
  constexpr int kClipFrames = 4;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(kSr, kBlock);

  std::array<float, kClipFrames> clip_l{1.0f, 0.5f, 0.25f, 0.125f};
  std::array<float, kClipFrames> clip_r{};
  const float* clip_channels[] = {clip_l.data(), clip_r.data()};

  engine.set_tempo(60.0);
  std::vector<sonare::engine::ClipSchedule> clips;
  clips.emplace_back(1u, sonare::engine::ClipAudioBuffer{clip_channels, 2, kClipFrames}, 1.0, 0, 0,
                     kClipFrames, false, 1.0f, 0, 0);
  engine.set_clips(std::move(clips));

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  sonare::rt::Command seek{};
  seek.type = sonare::rt::CommandType::kTransportSeekSample;
  seek.sample_time = -1;
  seek.arg.i = 24000;
  REQUIRE(engine.push_command(seek));

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* io[] = {left.data(), right.data()};
  engine.process(io, 2, kBlock);

  for (float sample : left) {
    REQUIRE(sample == 0.0f);
  }

  seek.arg.i = 48000;
  REQUIRE(engine.push_command(seek));
  left.fill(0.0f);
  right.fill(0.0f);
  engine.process(io, 2, kBlock);

  REQUIRE(left[0] == 1.0f);
  REQUIRE(left[1] == 0.5f);
  REQUIRE(left[2] == 0.25f);
  REQUIRE(left[3] == 0.125f);
}

TEST_CASE("RealtimeEngine rejects invalid tempo without replacing the active map",
          "[engine][realtime][numeric]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 128);
  engine.set_tempo(60.0);
  REQUIRE(engine.sample_at_ppq(1.0) == 48000);

  for (double invalid : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity()}) {
    REQUIRE_THROWS_AS(engine.set_tempo(invalid), sonare::SonareException);
    REQUIRE(engine.sample_at_ppq(1.0) == 48000);
  }
}

TEST_CASE("RealtimeEngine clearing a tempo map reverts to the last single tempo",
          "[engine][realtime]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 128);
  engine.set_tempo(60.0);
  REQUIRE(engine.sample_at_ppq(1.0) == 48000);  // 60 bpm: 1 quarter = 48000 samples

  // Install a piecewise tempo map, then clear it with an empty segment list.
  engine.set_tempo_segments({{0.0, 120.0, 0.0}, {4.0, 90.0, 0.0}});
  engine.set_tempo_segments({});
  // Clearing reverts to the last single tempo (60 -> 48000), not a hardcoded
  // default of 120 bpm (which would give 24000).
  REQUIRE(engine.sample_at_ppq(1.0) == 48000);
}

#if defined(SONARE_WITH_MIXING)
TEST_CASE("RealtimeEngine track lanes are opt-in for clip routing", "[engine][realtime]") {
  constexpr int kBlock = 64;
  sonare::engine::RealtimeEngine legacy;
  sonare::engine::RealtimeEngine tagged;
  legacy.prepare(48000.0, kBlock);
  tagged.prepare(48000.0, kBlock);

  std::array<float, kBlock> clip_l{};
  std::array<float, kBlock> clip_r{};
  clip_l.fill(0.25f);
  clip_r.fill(-0.5f);
  const float* clip_channels[] = {clip_l.data(), clip_r.data()};

  sonare::engine::ClipSchedule legacy_clip{
      1, {clip_channels, 2, kBlock}, 0.0, 0, 0, kBlock, false, 1.0f, 0, 0};
  sonare::engine::ClipSchedule tagged_clip = legacy_clip;
  tagged_clip.track_id = 42;
  legacy.set_clips({legacy_clip});
  tagged.set_clips({tagged_clip});

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(legacy.push_command(play));
  REQUIRE(tagged.push_command(play));

  std::array<float, kBlock> legacy_l{};
  std::array<float, kBlock> legacy_r{};
  std::array<float, kBlock> tagged_l{};
  std::array<float, kBlock> tagged_r{};
  float* legacy_io[] = {legacy_l.data(), legacy_r.data()};
  float* tagged_io[] = {tagged_l.data(), tagged_r.data()};
  legacy.process(legacy_io, 2, kBlock);
  tagged.process(tagged_io, 2, kBlock);

  REQUIRE(tagged_l == legacy_l);
  REQUIRE(tagged_r == legacy_r);
}

TEST_CASE("RealtimeEngine track lanes route clip audio through lane state", "[engine][realtime]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 24;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  std::array<float, kFrames> clip_a_l{};
  std::array<float, kFrames> clip_a_r{};
  std::array<float, kFrames> clip_b_l{};
  std::array<float, kFrames> clip_b_r{};
  clip_a_l.fill(1.0f);
  clip_a_r.fill(1.0f);
  clip_b_l.fill(1.0f);
  clip_b_r.fill(1.0f);
  const float* a[] = {clip_a_l.data(), clip_a_r.data()};
  const float* b[] = {clip_b_l.data(), clip_b_r.data()};

  sonare::engine::ClipSchedule clip_a{1, {a, 2, kFrames}, 0.0, 0, 0, kFrames, false, 1.0f, 0, 0};
  clip_a.track_id = 10;
  sonare::engine::ClipSchedule clip_b{2, {b, 2, kFrames}, 0.0, 0, 0, kFrames, false, 1.0f, 0, 0};
  clip_b.track_id = 20;
  engine.set_clips({clip_a, clip_b});
  REQUIRE(engine.set_track_lanes({{10}, {20}}));

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* io[] = {left.data(), right.data()};
  engine.process(io, 2, kBlock);
  REQUIRE(left.back() == 2.0f);
  REQUIRE(right.back() == 2.0f);

  REQUIRE(engine.track_mixer().set_lane_solo_mute(0, true, false));
  for (int block = 0; block < 4; ++block) {
    left.fill(0.0f);
    right.fill(0.0f);
    engine.process(io, 2, kBlock);
  }
  REQUIRE(left.back() < 1.25f);
  REQUIRE(right.back() < 1.25f);
  REQUIRE(left.back() > 0.75f);
  REQUIRE(right.back() > 0.75f);

  REQUIRE(engine.track_mixer().set_lane_parameter(0, sonare::engine::TrackMixerRuntime::kFaderDb,
                                                  -12.0f));
  for (int block = 0; block < 20; ++block) {
    left.fill(0.0f);
    right.fill(0.0f);
    engine.process(io, 2, kBlock);
  }
  REQUIRE(left.back() < 0.4f);
  REQUIRE(right.back() < 0.4f);
}

TEST_CASE("RealtimeEngine automation lanes drive reserved track mixer faders",
          "[engine][realtime]") {
  // An automation lane targeting the reserved engine namespace (lane 0 fader)
  // must reach the track mixer runtime without any ProcessorBase binding: held
  // at 0 dB before the breakpoint, dropping to -60 dB after it.
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 48;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  std::array<float, kFrames> clip_l{};
  std::array<float, kFrames> clip_r{};
  clip_l.fill(1.0f);
  clip_r.fill(1.0f);
  const float* clip_channels[] = {clip_l.data(), clip_r.data()};
  sonare::engine::ClipSchedule clip{
      1, {clip_channels, 2, kFrames}, 0.0, 0, 0, kFrames, false, 1.0f, 0, 0};
  clip.track_id = 10;
  engine.set_clips({clip});
  REQUIRE(engine.set_track_lanes({{10}}));

  // Breakpoint at sample 1024 (= 4 blocks). 1 ppq = 24000 samples at 120 bpm.
  constexpr double kBreakpointPpq = 1024.0 / 24000.0;
  const uint32_t fader_target =
      engine_lane_param_target(0, sonare::engine::TrackMixerRuntime::kFaderDb);
  sonare::automation::AutomationLane lane(fader_target);
  lane.set_points({{0.0, 0.0f, sonare::automation::CurveType::Hold},
                   {kBreakpointPpq, -60.0f, sonare::automation::CurveType::Hold}});
  engine.automation().set_lanes({lane});

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* io[] = {left.data(), right.data()};

  // Before the breakpoint the lane holds 0 dB: unity clip level passes through.
  for (int block = 0; block < 4; ++block) {
    left.fill(0.0f);
    right.fill(0.0f);
    engine.process(io, 2, kBlock);
  }
  REQUIRE(left.back() > 0.9f);
  REQUIRE(right.back() > 0.9f);
  REQUIRE(engine.automation().unknown_target_count() == 0);

  // After the breakpoint the routed -60 dB fader (smoothed) collapses the lane.
  for (int block = 0; block < 40; ++block) {
    left.fill(0.0f);
    right.fill(0.0f);
    engine.process(io, 2, kBlock);
  }
  REQUIRE(left.back() < 0.2f);
  REQUIRE(right.back() < 0.2f);
  REQUIRE(engine.automation().unknown_target_count() == 0);
}

TEST_CASE("RealtimeEngine settle_parameters snaps lane faders for offline rendering",
          "[engine][realtime]") {
  // Lane fader smoothers only advance while lanes render, so an offline render
  // that seeks and plays a freshly configured engine would capture the first
  // milliseconds with faders still ramping in from 0 dB. Once a stopped block
  // has applied automation at the position and drained queued commands,
  // settle_parameters must snap the smoothers so the very first audible frame
  // renders at the automated value.
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 8;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  std::array<float, kFrames> clip_l{};
  std::array<float, kFrames> clip_r{};
  clip_l.fill(1.0f);
  clip_r.fill(1.0f);
  const float* clip_channels[] = {clip_l.data(), clip_r.data()};
  sonare::engine::ClipSchedule clip{
      1, {clip_channels, 2, kFrames}, 0.0, 0, 0, kFrames, false, 1.0f, 0, 0};
  clip.track_id = 10;
  engine.set_clips({clip});
  REQUIRE(engine.set_track_lanes({{10}}));

  const uint32_t fader_target =
      engine_lane_param_target(0, sonare::engine::TrackMixerRuntime::kFaderDb);
  sonare::automation::AutomationLane lane(fader_target);
  lane.set_points({{0.0, -60.0f, sonare::automation::CurveType::Hold}});
  engine.automation().set_lanes({lane});

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* io[] = {left.data(), right.data()};

  // Priming block with the transport stopped: automation applies at the seek
  // position and pushes -60 dB as the lane fader target, but the smoother's
  // current value has not advanced (nothing rendered through the lane).
  engine.process(io, 2, kBlock);
  engine.settle_parameters();

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  left.fill(0.0f);
  right.fill(0.0f);
  engine.process(io, 2, kBlock);
  // The first audible frame must already sit at -60 dB (1e-3 linear), not at
  // the 0 dB reset value the smoother would otherwise ramp down from.
  REQUIRE(left.front() < 0.01f);
  REQUIRE(right.front() < 0.01f);
  REQUIRE(left.back() < 0.01f);
  REQUIRE(engine.automation().unknown_target_count() == 0);
}

TEST_CASE("RealtimeEngine routes a lane's output through its group bus", "[engine][realtime]") {
  // A lane with output_bus_id sums into that bus (here at -6 dB) instead of
  // the master mix, so the master receives the bus-attenuated signal only.
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 8;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  std::array<float, kFrames> clip_l{};
  std::array<float, kFrames> clip_r{};
  clip_l.fill(1.0f);
  clip_r.fill(1.0f);
  const float* clip_channels[] = {clip_l.data(), clip_r.data()};
  sonare::engine::ClipSchedule clip{
      1, {clip_channels, 2, kFrames}, 0.0, 0, 0, kFrames, false, 1.0f, 0, 0};
  clip.track_id = 10;
  engine.set_clips({clip});

  // Routing to an undeclared bus is rejected; declare the bus first.
  sonare::engine::TrackLaneConfig grouped{10};
  grouped.output_bus_id = 5;
  REQUIRE_FALSE(engine.set_track_lanes({grouped}));
  REQUIRE(engine.set_track_buses({{5, -6.0206f}}));
  REQUIRE(engine.set_track_lanes({grouped}));
  engine.settle_parameters();

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* io[] = {left.data(), right.data()};
  for (int block = 0; block < 4; ++block) {
    left.fill(0.0f);
    right.fill(0.0f);
    engine.process(io, 2, kBlock);
  }
  // Unity clip through the -6 dB group bus: master sits at ~0.5, not 1.0.
  REQUIRE(left.back() > 0.45f);
  REQUIRE(left.back() < 0.55f);
  REQUIRE(right.back() > 0.45f);
  REQUIRE(right.back() < 0.55f);
}

TEST_CASE("RealtimeEngine lane sidechain ducks one lane from another's audio",
          "[engine][realtime]") {
  // A ducking insert on lane 10 keyed from lane 20 must attenuate lane 10
  // while the key lane carries signal, and pass it (near) unity when the key
  // lane is silent.
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 48;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  const auto run = [&](bool key_audible) {
    std::array<float, kFrames> pad{};
    std::array<float, kFrames> key{};
    pad.fill(0.5f);
    key.fill(key_audible ? 1.0f : 0.0f);
    const float* pad_channels[] = {pad.data(), pad.data()};
    const float* key_channels[] = {key.data(), key.data()};
    sonare::engine::ClipSchedule pad_clip{
        1, {pad_channels, 2, kFrames}, 0.0, 0, 0, kFrames, false, 1.0f, 0, 0};
    pad_clip.track_id = 10;
    sonare::engine::ClipSchedule key_clip{
        2, {key_channels, 2, kFrames}, 0.0, 0, 0, kFrames, false, 1.0f, 0, 0};
    key_clip.track_id = 20;
    engine.set_clips({pad_clip, key_clip});
    REQUIRE(engine.set_track_lanes({{10}, {20}}));

    sonare::mixing::api::Strip strip_spec;
    strip_spec.pan_law = 3;
    strip_spec.inserts.push_back(sonare::mixing::api::Insert{
        sonare::mixing::api::InsertSlot::PostFader, "dynamics.duckingProcessor",
        R"({"thresholdDb":-30,"ratio":20,"attackMs":1,"releaseMs":50,"rangeDb":24})"});
    REQUIRE(engine.set_track_strip(10, strip_spec));
    REQUIRE(engine.set_lane_sidechain(10, 0, 20));

    // Mute the key lane's own master contribution so the measurement sees the
    // pad lane only (the sidechain taps the key pre-fader, post-strip).
    REQUIRE(engine.track_mixer().set_lane_parameter(1, sonare::engine::TrackMixerRuntime::kFaderDb,
                                                    -120.0f));
    engine.settle_parameters();

    sonare::rt::Command seek{};
    seek.type = sonare::rt::CommandType::kTransportSeekSample;
    seek.sample_time = -1;
    seek.arg.i = 0;
    REQUIRE(engine.push_command(seek));
    sonare::rt::Command play{};
    play.type = sonare::rt::CommandType::kTransportPlay;
    play.sample_time = -1;
    REQUIRE(engine.push_command(play));

    std::array<float, kBlock> left{};
    std::array<float, kBlock> right{};
    float* io[] = {left.data(), right.data()};
    float level = 0.0f;
    for (int block = 0; block < 40; ++block) {
      left.fill(0.0f);
      right.fill(0.0f);
      engine.process(io, 2, kBlock);
      level = std::abs(left.back());
    }
    sonare::rt::Command stop{};
    stop.type = sonare::rt::CommandType::kTransportStop;
    stop.sample_time = -1;
    REQUIRE(engine.push_command(stop));
    left.fill(0.0f);
    right.fill(0.0f);
    engine.process(io, 2, kBlock);
    REQUIRE(engine.set_lane_sidechain(10, 0, 0));
    return level;
  };

  const float ducked = run(true);
  const float open = run(false);
  REQUIRE(open > 0.4f);
  // 24 dB of range at 20:1 on a key far over threshold: at least ~12 dB down.
  REQUIRE(ducked < open * 0.25f);
}

TEST_CASE("RealtimeEngine commands drive track lane params and solo mute", "[engine][realtime]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 8;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  std::array<float, kFrames> clip_a_l{};
  std::array<float, kFrames> clip_a_r{};
  std::array<float, kFrames> clip_b_l{};
  std::array<float, kFrames> clip_b_r{};
  clip_a_l.fill(1.0f);
  clip_a_r.fill(1.0f);
  clip_b_l.fill(1.0f);
  clip_b_r.fill(1.0f);
  const float* a[] = {clip_a_l.data(), clip_a_r.data()};
  const float* b[] = {clip_b_l.data(), clip_b_r.data()};

  sonare::engine::ClipSchedule clip_a{1, {a, 2, kFrames}, 0.0, 0, 0, kFrames, false, 1.0f, 0, 0};
  clip_a.track_id = 10;
  sonare::engine::ClipSchedule clip_b{2, {b, 2, kFrames}, 0.0, 0, 0, kFrames, false, 1.0f, 0, 0};
  clip_b.track_id = 20;
  engine.set_clips({clip_a, clip_b});
  REQUIRE(engine.set_track_lanes({{10}, {20}}));

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* io[] = {left.data(), right.data()};
  engine.process(io, 2, kBlock);
  REQUIRE(left.back() == 2.0f);

  sonare::rt::Command solo{};
  solo.type = sonare::rt::CommandType::kSetSoloMute;
  solo.target_id = 0;
  solo.sample_time = -1;
  solo.arg.i = 0x2;
  REQUIRE(engine.push_command(solo));

  for (int block = 0; block < 4; ++block) {
    left.fill(0.0f);
    right.fill(0.0f);
    engine.process(io, 2, kBlock);
  }
  REQUIRE(left.back() < 1.25f);
  REQUIRE(left.back() > 0.75f);

  sonare::rt::Command fader{};
  fader.type = sonare::rt::CommandType::kSetParamSmoothed;
  fader.target_id = engine_lane_param_target(0, sonare::engine::TrackMixerRuntime::kFaderDb);
  fader.sample_time = -1;
  fader.arg.f = -12.0f;
  REQUIRE(engine.push_command(fader));

  for (int block = 0; block < 6; ++block) {
    left.fill(0.0f);
    right.fill(0.0f);
    engine.process(io, 2, kBlock);
  }
  REQUIRE(left.back() < 0.45f);
  REQUIRE(right.back() < 0.45f);

  sonare::engine::Telemetry telemetry{};
  while (engine.pop_telemetry(telemetry)) {
    REQUIRE(telemetry.error != sonare::engine::TelemetryErrorCode::kNonQueueableCommand);
  }
}

TEST_CASE("RealtimeEngine lane state follows track id across track lane republish",
          "[engine][realtime]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 24;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  std::array<float, kFrames> clip_a{};
  std::array<float, kFrames> clip_b{};
  clip_a.fill(1.0f);
  clip_b.fill(1.0f);
  const float* a[] = {clip_a.data()};
  const float* b[] = {clip_b.data()};

  sonare::engine::ClipSchedule clip_a_schedule{1,       {a, 1, kFrames}, 0.0,  0, 0,
                                               kFrames, false,           1.0f, 0, 0};
  clip_a_schedule.track_id = 10;
  sonare::engine::ClipSchedule clip_b_schedule{2,       {b, 1, kFrames}, 0.0,  0, 0,
                                               kFrames, false,           1.0f, 0, 0};
  clip_b_schedule.track_id = 20;
  engine.set_clips({clip_a_schedule, clip_b_schedule});
  REQUIRE(engine.set_track_lanes({{10}, {20}}));

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::array<float, kBlock> left{};
  float* io[] = {left.data()};
  engine.process(io, 1, kBlock);
  REQUIRE(left.back() == 2.0f);

  sonare::rt::Command fader{};
  fader.type = sonare::rt::CommandType::kSetParamSmoothed;
  fader.target_id = engine_lane_param_target(0, sonare::engine::TrackMixerRuntime::kFaderDb);
  fader.sample_time = -1;
  fader.arg.f = -12.0f;
  REQUIRE(engine.push_command(fader));

  sonare::rt::Command solo{};
  solo.type = sonare::rt::CommandType::kSetSoloMute;
  solo.target_id = 0;
  solo.sample_time = -1;
  solo.arg.i = 0x2;
  REQUIRE(engine.push_command(solo));

  for (int block = 0; block < 2; ++block) {
    left.fill(0.0f);
    engine.process(io, 1, kBlock);
  }

  REQUIRE(engine.set_track_lanes({{20}, {10}}));
  for (int block = 0; block < 16; ++block) {
    left.fill(0.0f);
    engine.process(io, 1, kBlock);
  }

  REQUIRE(left.back() > 0.20f);
  REQUIRE(left.back() < 0.70f);
}

TEST_CASE("RealtimeEngine track lane smoother survives transport seek", "[engine][realtime]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 16;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  std::array<float, kFrames> clip{};
  clip.fill(1.0f);
  const float* clip_channels[] = {clip.data()};
  sonare::engine::ClipSchedule clip_schedule{
      1, {clip_channels, 1, kFrames}, 0.0, 0, 0, kFrames, false, 1.0f, 0, 0};
  clip_schedule.track_id = 10;
  engine.set_clips({clip_schedule});
  REQUIRE(engine.set_track_lanes({{10}}));

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::array<float, kBlock> left{};
  float* io[] = {left.data()};
  engine.process(io, 1, kBlock);
  REQUIRE(left.back() == 1.0f);

  REQUIRE(engine.track_mixer().set_lane_parameter(0, sonare::engine::TrackMixerRuntime::kFaderDb,
                                                  -12.0f));
  for (int block = 0; block < 8; ++block) {
    left.fill(0.0f);
    engine.process(io, 1, kBlock);
  }
  REQUIRE(left.back() > 0.20f);
  REQUIRE(left.back() < 0.35f);

  sonare::rt::Command seek{};
  seek.type = sonare::rt::CommandType::kTransportSeekSample;
  seek.sample_time = -1;
  seek.arg.i = kBlock;
  REQUIRE(engine.push_command(seek));
  left.fill(0.0f);
  engine.process(io, 1, kBlock);

  REQUIRE(left.front() > 0.20f);
  REQUIRE(left.front() < 0.35f);
  REQUIRE(left.back() > 0.20f);
  REQUIRE(left.back() < 0.35f);
}

TEST_CASE("RealtimeEngine track lane smoother survives loop wrap", "[engine][realtime]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 16;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  std::array<float, kFrames> clip{};
  clip.fill(1.0f);
  const float* clip_channels[] = {clip.data()};
  sonare::engine::ClipSchedule clip_schedule{
      1, {clip_channels, 1, kFrames}, 0.0, 0, 0, kFrames, false, 1.0f, 0, 0};
  clip_schedule.track_id = 10;
  engine.set_clips({clip_schedule});
  REQUIRE(engine.set_track_lanes({{10}}));
  engine.set_tempo(60.0);
  engine.set_loop(0.0, 0.004, true);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::array<float, kBlock> left{};
  float* io[] = {left.data()};
  engine.process(io, 1, kBlock);
  REQUIRE(left.back() == 1.0f);

  REQUIRE(engine.track_mixer().set_lane_parameter(0, sonare::engine::TrackMixerRuntime::kFaderDb,
                                                  -12.0f));
  for (int block = 0; block < 8; ++block) {
    left.fill(0.0f);
    engine.process(io, 1, kBlock);
  }
  REQUIRE(left.back() > 0.20f);
  REQUIRE(left.back() < 0.35f);

  left.fill(0.0f);
  engine.process(io, 1, kBlock);
  REQUIRE(left.front() > 0.20f);
  REQUIRE(left.front() < 0.35f);
  REQUIRE(left.back() > 0.20f);
  REQUIRE(left.back() < 0.35f);
}

TEST_CASE("RealtimeEngine folds a playhead at or past loop_end before rendering",
          "[engine][realtime]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 8;
  constexpr int64_t kLoopLen = 480;  // loop [0, 0.01) ppq at 60 BPM and 48 kHz
  std::array<float, kFrames> clip{};
  for (int i = 0; i < kFrames; ++i) clip[static_cast<size_t>(i)] = static_cast<float>(i + 1);
  const float* clip_channels[] = {clip.data()};

  auto make_engine = [&](sonare::engine::RealtimeEngine& engine) {
    engine.prepare(48000.0, kBlock);
    engine.set_clips({sonare::engine::ClipSchedule{
        1, {clip_channels, 1, kFrames}, 0.0, 0, 0, kFrames, false, 1.0f, 0, 0}});
    engine.set_tempo(60.0);
    sonare::rt::Command play{};
    play.type = sonare::rt::CommandType::kTransportPlay;
    play.sample_time = -1;
    REQUIRE(engine.push_command(play));
  };
  std::array<float, kBlock> left{};
  float* io[] = {left.data()};

  SECTION("a loop enabled behind the playhead") {
    sonare::engine::RealtimeEngine engine;
    make_engine(engine);
    for (int block = 0; block < 4; ++block) engine.process(io, 1, kBlock);
    engine.set_loop(0.0, 0.01, true);
    left.fill(0.0f);
    engine.process(io, 1, kBlock);
    const int64_t start = (4 * kBlock) % kLoopLen;
    CHECK(left.front() == static_cast<float>(start + 1));
    CHECK(left.back() == static_cast<float>((start + kBlock - 1) % kLoopLen + 1));
  }

  SECTION("a seek past loop_end") {
    sonare::engine::RealtimeEngine engine;
    make_engine(engine);
    engine.set_loop(0.0, 0.01, true);
    engine.process(io, 1, kBlock);
    sonare::rt::Command seek{};
    seek.type = sonare::rt::CommandType::kTransportSeekSample;
    seek.sample_time = -1;
    seek.arg.i = 1000;
    REQUIRE(engine.push_command(seek));
    left.fill(0.0f);
    engine.process(io, 1, kBlock);
    const int64_t start = 1000 % kLoopLen;
    CHECK(left.front() == static_cast<float>(start + 1));
    CHECK(left.back() == static_cast<float>((start + kBlock - 1) % kLoopLen + 1));
  }
}

TEST_CASE("RealtimeEngine processes owned track strip specs before lane mix",
          "[engine][realtime]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 4;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  std::array<float, kFrames> clip_a{};
  std::array<float, kFrames> clip_b{};
  clip_a.fill(1.0f);
  clip_b.fill(1.0f);
  const float* a[] = {clip_a.data()};
  const float* b[] = {clip_b.data()};

  sonare::engine::ClipSchedule clip_a_schedule{1,       {a, 1, kFrames}, 0.0,  0, 0,
                                               kFrames, false,           1.0f, 0, 0};
  clip_a_schedule.track_id = 10;
  sonare::engine::ClipSchedule clip_b_schedule{2,       {b, 1, kFrames}, 0.0,  0, 0,
                                               kFrames, false,           1.0f, 0, 0};
  clip_b_schedule.track_id = 20;
  engine.set_clips({clip_a_schedule, clip_b_schedule});
  REQUIRE(engine.set_track_lanes({{10}, {20}}));

  sonare::mixing::api::Strip strip_spec;
  strip_spec.fader_db = -12.0f;
  strip_spec.pan_law = 3;
  REQUIRE(engine.set_track_strip(10, strip_spec));

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::array<float, kBlock> left{};
  float* io[] = {left.data()};
  engine.process(io, 1, kBlock);

  REQUIRE(left.back() > 1.20f);
  REQUIRE(left.back() < 1.40f);
}

TEST_CASE("RealtimeEngine live channel delay edit renders like a fresh build of the same delay",
          "[engine][realtime][pdc]") {
  constexpr int kBlock = 128;
  constexpr int kBlocks = 40;
  constexpr int kFrames = kBlock * kBlocks;
  constexpr int kEditBlock = 4;
  constexpr int kImpulseFrame = 2048;
  constexpr int kDelay = 100;
  // Track 10 carries a latent insert (8-sample lookahead) and a reverb tail, track 20 is plain.
  constexpr uint32_t kLatentTrack = 10;
  constexpr uint32_t kPlainTrack = 20;

  const auto strip_for = [](uint32_t track_id, int delay) {
    sonare::mixing::api::Strip spec;
    spec.channel_delay_samples = delay;
    if (track_id == kLatentTrack) {
      spec.inserts.push_back({sonare::mixing::api::InsertSlot::PreFader, "dynamics.limiter",
                              R"({"thresholdDb":24,"lookaheadMs":0.16666667,"releaseMs":50})"});
      spec.inserts.push_back(
          {sonare::mixing::api::InsertSlot::PostFader, "effects.reverb.plate", R"({"mix":0.5})"});
    }
    return spec;
  };

  // Renders the stereo master with only the lanes in @p sounding carrying the impulse.
  const auto render = [&](uint32_t edited, bool live, bool sound_latent, bool sound_plain) {
    std::vector<float> latent_src(kFrames, 0.0f);
    std::vector<float> plain_src(kFrames, 0.0f);
    if (sound_latent) latent_src[kImpulseFrame] = 1.0f;
    if (sound_plain) plain_src[kImpulseFrame] = 0.5f;
    const float* latent_channels[] = {latent_src.data(), latent_src.data()};
    const float* plain_channels[] = {plain_src.data(), plain_src.data()};

    sonare::engine::RealtimeEngine engine;
    engine.prepare(48000.0, kBlock);
    sonare::engine::ClipSchedule latent_clip{
        1, {latent_channels, 2, kFrames}, 0.0, 0, 0, kFrames, false, 1.0f, 0, 0};
    latent_clip.track_id = kLatentTrack;
    sonare::engine::ClipSchedule plain_clip{
        2, {plain_channels, 2, kFrames}, 0.0, 0, 0, kFrames, false, 1.0f, 0, 0};
    plain_clip.track_id = kPlainTrack;
    engine.set_clips({latent_clip, plain_clip});
    REQUIRE(engine.set_track_lanes({{kLatentTrack}, {kPlainTrack}}));
    for (const uint32_t track : {kLatentTrack, kPlainTrack}) {
      const int delay = (!live && track == edited) ? kDelay : 0;
      REQUIRE(engine.set_track_strip(track, strip_for(track, delay)));
    }

    sonare::rt::Command play{};
    play.type = sonare::rt::CommandType::kTransportPlay;
    play.sample_time = -1;
    REQUIRE(engine.push_command(play));

    std::vector<float> out_l(kFrames, 0.0f);
    std::vector<float> out_r(kFrames, 0.0f);
    for (int block = 0; block < kBlocks; ++block) {
      if (live && block == kEditBlock) {
        REQUIRE(engine.set_track_channel_delay_samples(edited, kDelay));
      }
      float* io[] = {out_l.data() + block * kBlock, out_r.data() + block * kBlock};
      engine.process(io, 2, kBlock);
    }
    return std::make_pair(out_l, out_r);
  };

  for (const uint32_t edited : {kLatentTrack, kPlainTrack}) {
    for (const auto& [sound_latent, sound_plain] :
         {std::pair{true, true}, std::pair{true, false}, std::pair{false, true}}) {
      INFO("edited track " << edited << ", latent lane sounding " << sound_latent
                           << ", plain lane sounding " << sound_plain);
      const auto fresh = render(edited, false, sound_latent, sound_plain);
      const auto live = render(edited, true, sound_latent, sound_plain);
      float peak = 0.0f;
      for (const float v : fresh.first) peak = std::max(peak, std::abs(v));
      REQUIRE(peak > 0.1f);
      const float tolerance = 1.0e-5f * std::max(1.0f, peak);
      for (int i = kEditBlock * kBlock; i < kFrames; ++i) {
        INFO("frame " << i);
        const auto index = static_cast<size_t>(i);
        REQUIRE(std::abs(live.first[index] - fresh.first[index]) <= tolerance);
        REQUIRE(std::abs(live.second[index] - fresh.second[index]) <= tolerance);
      }
    }
  }
}

TEST_CASE("RealtimeEngine channel delay moves its own lane relative to the others",
          "[engine][realtime][pdc]") {
  constexpr int kBlock = 128;
  constexpr int kBlocks = 32;
  constexpr int kFrames = kBlock * kBlocks;
  constexpr int kEditBlock = 4;
  constexpr int kImpulseFrame = 2048;
  constexpr int kDelay = 100;
  // 0.16666667 ms of lookahead is exactly 8 samples at 48 kHz.
  constexpr int kInsertLatency = 8;
  constexpr uint32_t kLatentTrack = 10;
  constexpr uint32_t kPlainTrack = 20;
  enum class Path { kSpec, kSetterBeforeRender, kLive };

  struct Result {
    int onset = -1;
    int latency_q8 = -1;
  };
  // Onset of the master output with only @p sounding carrying an impulse.
  const auto render = [&](uint32_t edited, int delay, Path path, uint32_t sounding) {
    std::vector<float> latent_src(kFrames, 0.0f);
    std::vector<float> plain_src(kFrames, 0.0f);
    (sounding == kLatentTrack ? latent_src : plain_src)[kImpulseFrame] = 1.0f;
    const float* latent_channels[] = {latent_src.data(), latent_src.data()};
    const float* plain_channels[] = {plain_src.data(), plain_src.data()};

    sonare::engine::RealtimeEngine engine;
    engine.prepare(48000.0, kBlock);
    REQUIRE(engine.set_track_lanes({{kLatentTrack}, {kPlainTrack}}));
    for (const uint32_t track : {kLatentTrack, kPlainTrack}) {
      sonare::mixing::api::Strip spec;
      if (path == Path::kSpec && track == edited) spec.channel_delay_samples = delay;
      if (track == kLatentTrack) {
        spec.inserts.push_back({sonare::mixing::api::InsertSlot::PreFader, "dynamics.limiter",
                                R"({"thresholdDb":24,"lookaheadMs":0.16666667,"releaseMs":50})"});
      }
      REQUIRE(engine.set_track_strip(track, spec));
    }
    sonare::engine::ClipSchedule latent_clip{
        1, {latent_channels, 2, kFrames}, 0.0, 0, 0, kFrames, false, 1.0f, 0, 0};
    latent_clip.track_id = kLatentTrack;
    sonare::engine::ClipSchedule plain_clip{
        2, {plain_channels, 2, kFrames}, 0.0, 0, 0, kFrames, false, 1.0f, 0, 0};
    plain_clip.track_id = kPlainTrack;
    engine.set_clips({latent_clip, plain_clip});
    if (path == Path::kSetterBeforeRender) {
      REQUIRE(engine.set_track_channel_delay_samples(edited, delay));
    }

    sonare::rt::Command play{};
    play.type = sonare::rt::CommandType::kTransportPlay;
    play.sample_time = -1;
    REQUIRE(engine.push_command(play));

    std::vector<float> out_l(kFrames, 0.0f);
    std::vector<float> out_r(kFrames, 0.0f);
    for (int block = 0; block < kBlocks; ++block) {
      if (path == Path::kLive && block == kEditBlock) {
        REQUIRE(engine.set_track_channel_delay_samples(edited, delay));
      }
      float* io[] = {out_l.data() + block * kBlock, out_r.data() + block * kBlock};
      engine.process(io, 2, kBlock);
    }
    Result result;
    result.latency_q8 = engine.graph_latency_samples_q8();
    for (int i = 0; i < kFrames; ++i) {
      if (std::abs(out_l[static_cast<size_t>(i)]) > 0.5f) {
        result.onset = i;
        break;
      }
    }
    return result;
  };

  const int baseline = kImpulseFrame + kInsertLatency;
  for (const uint32_t edited : {kLatentTrack, kPlainTrack}) {
    const uint32_t other = edited == kLatentTrack ? kPlainTrack : kLatentTrack;
    for (const Path path : {Path::kSpec, Path::kSetterBeforeRender, Path::kLive}) {
      INFO("edited track " << edited << ", path " << static_cast<int>(path));
      const Result undelayed = render(edited, 0, path, edited);
      REQUIRE(undelayed.onset == baseline);
      const Result moved = render(edited, kDelay, path, edited);
      const Result unmoved = render(edited, kDelay, path, other);
      CHECK(moved.onset == baseline + kDelay);
      CHECK(unmoved.onset == baseline);
      // A channel delay is an alignment choice, not processing latency.
      CHECK(moved.latency_q8 == undelayed.latency_q8);
      CHECK(moved.latency_q8 == kInsertLatency << 8);
    }
  }
}

TEST_CASE("RealtimeEngine processes owned master strip specs after lane mix",
          "[engine][realtime]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 24;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  std::array<float, kFrames> clip_a{};
  std::array<float, kFrames> clip_b{};
  clip_a.fill(1.0f);
  clip_b.fill(1.0f);
  const float* a[] = {clip_a.data()};
  const float* b[] = {clip_b.data()};

  sonare::engine::ClipSchedule clip_a_schedule{1,       {a, 1, kFrames}, 0.0,  0, 0,
                                               kFrames, false,           1.0f, 0, 0};
  clip_a_schedule.track_id = 10;
  sonare::engine::ClipSchedule clip_b_schedule{2,       {b, 1, kFrames}, 0.0,  0, 0,
                                               kFrames, false,           1.0f, 0, 0};
  clip_b_schedule.track_id = 20;
  engine.set_clips({clip_a_schedule, clip_b_schedule});
  REQUIRE(engine.set_track_lanes({{10}, {20}}));

  sonare::mixing::api::Strip master_spec;
  master_spec.fader_db = -12.0f;
  master_spec.pan_law = 3;
  REQUIRE(engine.set_master_strip(master_spec));

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::array<float, kBlock> left{};
  float* io[] = {left.data()};
  for (int block = 0; block < 20; ++block) {
    left.fill(0.0f);
    engine.process(io, 1, kBlock);
  }

  REQUIRE(left.back() > 0.65f);
  REQUIRE(left.back() < 0.80f);
}

TEST_CASE("RealtimeEngine routes reserved master mixer parameters", "[engine][realtime]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 24;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  std::array<float, kFrames> source{};
  source.fill(1.0f);
  const float* channels[] = {source.data()};
  sonare::engine::ClipSchedule clip{1, {channels, 1, kFrames}, 0.0, 0, 0, kFrames, false, 1.0f, 0,
                                    0};
  engine.set_clips({clip});

  sonare::mixing::api::Strip master_spec;
  master_spec.fader_db = 0.0f;
  master_spec.pan = 0.0f;
  master_spec.pan_law = 3;
  REQUIRE(engine.set_master_strip(master_spec));

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::array<float, kBlock> left{};
  float* io[] = {left.data()};
  engine.process(io, 1, kBlock);
  REQUIRE(left.back() > 0.95f);

  sonare::rt::Command fader{};
  fader.type = sonare::rt::CommandType::kSetParamSmoothed;
  fader.target_id = engine_master_param_target(sonare::engine::MixingRuntime::kFaderDb);
  fader.sample_time = -1;
  fader.arg.f = -12.0f;
  REQUIRE(engine.push_command(fader));

  for (int block = 0; block < 12; ++block) {
    left.fill(0.0f);
    engine.process(io, 1, kBlock);
  }
  REQUIRE(left.back() > 0.20f);
  REQUIRE(left.back() < 0.40f);

  sonare::rt::Command pan{};
  pan.type = sonare::rt::CommandType::kSetParam;
  pan.target_id = engine_master_param_target(sonare::engine::MixingRuntime::kPan);
  pan.sample_time = -1;
  pan.arg.f = 0.25f;
  REQUIRE(engine.push_command(pan));
  left.fill(0.0f);
  engine.process(io, 1, kBlock);

  sonare::engine::Telemetry telemetry{};
  while (engine.pop_telemetry(telemetry)) {
    REQUIRE(telemetry.error != sonare::engine::TelemetryErrorCode::kUnknownTarget);
  }
}

TEST_CASE("RealtimeEngine offline pre-roll settles the master fader ramp", "[engine][realtime]") {
  // A master fader change made just before a bounce must not ramp into the
  // bounce's first samples: the pre-roll renders nothing, so it has to snap the
  // master strip's smoother the way it snaps the lane strips'.
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 2;
  std::array<float, kFrames> source{};
  source.fill(1.0f);
  const float* channels[] = {source.data()};

  auto bounce = [&](float fader_db) {
    sonare::engine::RealtimeEngine engine;
    engine.prepare(48000.0, kBlock);
    engine.set_clips({sonare::engine::ClipSchedule{
        1, {channels, 1, kFrames}, 0.0, 0, 0, kFrames, false, 1.0f, 0, 0}});
    sonare::mixing::api::Strip master_spec;
    master_spec.pan_law = 3;
    REQUIRE(engine.set_master_strip(master_spec));

    sonare::rt::Command fader{};
    fader.type = sonare::rt::CommandType::kSetParam;
    fader.target_id = engine_master_param_target(sonare::engine::MixingRuntime::kFaderDb);
    fader.sample_time = -1;
    fader.arg.f = fader_db;
    REQUIRE(engine.push_command(fader));
    sonare::rt::Command play{};
    play.type = sonare::rt::CommandType::kTransportPlay;
    play.sample_time = -1;
    REQUIRE(engine.push_command(play));

    std::array<float, kFrames> out{};
    float* io[] = {out.data()};
    engine.prime_offline_parameters(1, kBlock);
    engine.render_offline(io, 1, kFrames, kBlock);
    return out;
  };

  const auto unity = bounce(0.0f);
  const auto attenuated = bounce(-6.0f);
  const float expected = std::pow(10.0f, -6.0f / 20.0f);
  REQUIRE(unity.front() > 0.5f);
  CHECK(attenuated.front() / unity.front() == Catch::Approx(expected).epsilon(1e-4));
  CHECK(attenuated.back() / unity.back() == Catch::Approx(expected).epsilon(1e-4));
}

TEST_CASE("RealtimeEngine toggles owned master strip insert bypass", "[engine][realtime]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 16;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  std::array<float, kFrames> source{};
  for (int i = 0; i < kFrames; ++i) {
    source[static_cast<size_t>(i)] =
        std::sin(2.0f * 3.14159265358979323846f * 1000.0f * static_cast<float>(i) / 48000.0f);
  }
  const float* channels[] = {source.data()};
  sonare::engine::ClipSchedule clip{1, {channels, 1, kFrames}, 0.0, 0, 0, kFrames, false, 1.0f, 0,
                                    0};
  engine.set_clips({clip});

  sonare::mixing::api::Strip master_spec;
  master_spec.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PreFader, "eq.parametric",
       R"({"band0.type":1,"band0.frequencyHz":1000,"band0.gainDb":12,"band0.enabled":1})"});
  REQUIRE(engine.set_master_strip(master_spec));
  REQUIRE_FALSE(engine.set_master_insert_bypassed(7, true));

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::array<float, kBlock> eq_out{};
  float* io[] = {eq_out.data()};
  for (int block = 0; block < 6; ++block) {
    eq_out.fill(0.0f);
    engine.process(io, 1, kBlock);
  }

  REQUIRE(engine.set_master_insert_bypassed(0, true, true));
  sonare::rt::Command seek{};
  seek.type = sonare::rt::CommandType::kTransportSeekSample;
  seek.arg.i = 0;
  seek.sample_time = -1;
  REQUIRE(engine.push_command(seek));
  std::array<float, kBlock> bypassed_out{};
  io[0] = bypassed_out.data();
  engine.process(io, 1, kBlock);

  auto rms = [](const std::array<float, kBlock>& samples) {
    double sum = 0.0;
    for (float sample : samples) {
      sum += static_cast<double>(sample) * static_cast<double>(sample);
    }
    return std::sqrt(sum / static_cast<double>(samples.size()));
  };
  REQUIRE(rms(eq_out) > rms(bypassed_out) * 1.5);
}

TEST_CASE("RealtimeEngine metronome stays audible but is excluded from output capture",
          "[engine][realtime]") {
  constexpr int kBlock = 128;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  REQUIRE(engine.set_track_lanes({{10}}));
  REQUIRE(engine.track_mixer().set_lane_solo_mute(0, true, false));
  engine.set_metronome_config(sonare::engine::MetronomeConfig{true, 0.25f, 0.75f, 32, 0.0});

  std::array<float, kBlock> captured_l{};
  std::array<float, kBlock> captured_r{};
  float* capture_channels[] = {captured_l.data(), captured_r.data()};
  engine.set_capture_segment({capture_channels, 2, kBlock});
  engine.set_capture_punch(0, kBlock, true);
  engine.set_capture_armed(true);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* io[] = {left.data(), right.data()};
  engine.process(io, 2, kBlock);

  float output_peak = 0.0f;
  float capture_peak = 0.0f;
  for (int i = 0; i < kBlock; ++i) {
    output_peak = std::max(output_peak, std::abs(left[static_cast<size_t>(i)]));
    capture_peak = std::max(capture_peak, std::abs(captured_l[static_cast<size_t>(i)]));
  }
  REQUIRE(output_peak > 0.0f);
  REQUIRE(capture_peak == 0.0f);
  REQUIRE(engine.captured_frames() == kBlock);

  sonare::engine::RealtimeEngine offline;
  offline.prepare(48000.0, kBlock);
  offline.set_metronome_config(sonare::engine::MetronomeConfig{true, 0.25f, 0.75f, 32, 0.0});
  std::array<float, kBlock> bounce_l{};
  std::array<float, kBlock> bounce_r{};
  float* bounce[] = {bounce_l.data(), bounce_r.data()};
  offline.render_offline(bounce, 2, kBlock, kBlock);
  REQUIRE(*std::max_element(bounce_l.begin(), bounce_l.end()) == 0.0f);
  REQUIRE(offline.metronome_config().enabled);
}
#endif

TEST_CASE("RealtimeEngine drains paged clip requests and underrun telemetry",
          "[engine][realtime][clip_pages]") {
  class MissingPagedProvider final : public sonare::engine::ClipPagedAudioProvider {
   public:
    int num_channels() const noexcept override { return 1; }
    int64_t num_samples() const noexcept override { return static_cast<int64_t>(samples.size()); }
    int64_t page_frames() const noexcept override { return 4; }

    bool sample_at(int channel, int64_t sample, float* out) const noexcept override {
      if (channel != 0 || !out || sample < 0 || sample >= num_samples() || sample >= 2) {
        return false;
      }
      *out = samples[static_cast<size_t>(sample)];
      return true;
    }

    std::array<float, 4> samples{1.0f, 2.0f, 3.0f, 4.0f};
  };

  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 4, 16, 16);

  auto provider = std::make_shared<MissingPagedProvider>();
  sonare::engine::ClipSchedule clip{44, {}, 0.0, 0, 0, 4, false, 1.0f, 0, 0};
  clip.page_provider = provider;
  engine.set_clips({clip});

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::array<float, 4> left{};
  float* io[] = {left.data()};
  engine.process(io, 1, 4);

  sonare::engine::ClipPageRequest request{};
  REQUIRE(engine.pop_clip_page_request(request));
  REQUIRE(request.clip_id == 44);
  REQUIRE(request.channel == 0);
  REQUIRE(request.sample == 2);
  REQUIRE_FALSE(engine.pop_clip_page_request(request));

  int underrun_count = 0;
  bool found_underrun = false;
  sonare::engine::Telemetry telemetry{};
  while (engine.pop_telemetry(telemetry)) {
    if (telemetry.type == sonare::engine::TelemetryType::kError &&
        telemetry.error == sonare::engine::TelemetryErrorCode::kClipPageUnderrun &&
        telemetry.value == 44) {
      found_underrun = true;
      ++underrun_count;
    }
  }
  REQUIRE(found_underrun);
  REQUIRE(underrun_count == 1);
  REQUIRE(left[2] == 0.0f);
  REQUIRE(left[3] == 0.0f);
}

TEST_CASE("RealtimeEngine look-ahead prefetch requests pages without reporting an underrun",
          "[engine][realtime][clip_pages]") {
  // Every read inside the block succeeds, so nothing produced silence; only the
  // look-ahead pass reports pages, and those are fetch requests rather than
  // dropouts. Nothing else installs a provider overriding page_resident(), so
  // this is the only coverage of the prefetch path at the engine level.
  class NonResidentProvider final : public sonare::engine::ClipPagedAudioProvider {
   public:
    int num_channels() const noexcept override { return 1; }
    int64_t num_samples() const noexcept override { return 64; }
    int64_t page_frames() const noexcept override { return 4; }

    bool sample_at(int channel, int64_t sample, float* out) const noexcept override {
      if (channel != 0 || !out || sample < 0 || sample >= num_samples()) return false;
      *out = 1.0f;
      return true;
    }

    // Resident for the pages the block actually reads, absent beyond them, so
    // the look-ahead pass has something to report and the reads never fail.
    bool page_resident(int64_t page_index) const noexcept override { return page_index < 1; }
  };

  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 4, 16, 16);
  engine.set_clip_page_prefetch_frames(32);

  auto provider = std::make_shared<NonResidentProvider>();
  sonare::engine::ClipSchedule clip{45, {}, 0.0, 0, 0, 64, false, 1.0f, 0, 0};
  clip.page_provider = provider;
  engine.set_clips({clip});

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::array<float, 4> left{};
  float* io[] = {left.data()};
  engine.process(io, 1, 4);

  // The look-ahead pass reported at least one page, and every request it made
  // is flagged as a prefetch rather than a read miss.
  sonare::engine::ClipPageRequest request{};
  int requests = 0;
  while (engine.pop_clip_page_request(request)) {
    REQUIRE(request.clip_id == 45);
    REQUIRE_FALSE(request.read_miss);
    ++requests;
  }
  REQUIRE(requests > 0);

  // And no dropout was reported, because no read failed.
  sonare::engine::Telemetry telemetry{};
  int underrun_count = 0;
  while (engine.pop_telemetry(telemetry)) {
    if (telemetry.type == sonare::engine::TelemetryType::kError &&
        telemetry.error == sonare::engine::TelemetryErrorCode::kClipPageUnderrun) {
      ++underrun_count;
    }
  }
  REQUIRE(underrun_count == 0);
  // The audio the block did read is intact, confirming nothing was silenced.
  REQUIRE(left[0] == 1.0f);
  REQUIRE(left[3] == 1.0f);
}

TEST_CASE("RealtimeEngine counts paged clip requests dropped by its bounded queue",
          "[engine][realtime][clip_pages]") {
  class AlwaysMissingPagedProvider final : public sonare::engine::ClipPagedAudioProvider {
   public:
    int num_channels() const noexcept override { return 1; }
    int64_t num_samples() const noexcept override { return 4; }
    int64_t page_frames() const noexcept override { return 1; }
    bool sample_at(int, int64_t, float*) const noexcept override { return false; }
  };

  sonare::engine::RealtimeEngine engine;
  // The page-request queue is deliberately only two records deep. Four distinct
  // one-sample pages miss in a single block, so two requests must be observable
  // through the counter rather than disappearing silently.
  engine.prepare(48000.0, 4, 16, 2);

  auto provider = std::make_shared<AlwaysMissingPagedProvider>();
  sonare::engine::ClipSchedule clip{45, {}, 0.0, 0, 0, 4, false, 1.0f, 0, 0};
  clip.page_provider = provider;
  engine.set_clips({clip});

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::array<float, 4> left{};
  float* io[] = {left.data()};
  engine.process(io, 1, 4);

  sonare::engine::ClipPageRequest request{};
  int retained = 0;
  while (engine.pop_clip_page_request(request)) {
    ++retained;
  }
  REQUIRE(retained == 2);
  REQUIRE(engine.clip_page_request_overflow_count() == 2);

  // Re-prepare is a lifecycle reset, including the visible drop counter.
  engine.prepare(48000.0, 4, 16, 2);
  REQUIRE(engine.clip_page_request_overflow_count() == 0);
}

TEST_CASE("RealtimeEngine offline render matches block process", "[engine][realtime][offline]") {
  constexpr int kFrames = 512;
  constexpr int kBlock = 128;

  sonare::engine::RealtimeEngine realtime;
  sonare::engine::RealtimeEngine offline;
  realtime.prepare(48000.0, kBlock);
  offline.prepare(48000.0, kBlock);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(realtime.push_command(play));
  REQUIRE(offline.push_command(play));

  std::array<float, kFrames> rt_left{};
  std::array<float, kFrames> rt_right{};
  std::array<float, kFrames> off_left{};
  std::array<float, kFrames> off_right{};
  for (int i = 0; i < kFrames; ++i) {
    const float value = static_cast<float>(i) / static_cast<float>(kFrames);
    rt_left[static_cast<size_t>(i)] = value;
    rt_right[static_cast<size_t>(i)] = -value;
    off_left[static_cast<size_t>(i)] = value;
    off_right[static_cast<size_t>(i)] = -value;
  }

  for (int offset = 0; offset < kFrames; offset += kBlock) {
    float* channels[] = {rt_left.data() + offset, rt_right.data() + offset};
    realtime.process(channels, 2, kBlock);
  }

  float* offline_channels[] = {off_left.data(), off_right.data()};
  offline.render_offline(offline_channels, 2, kFrames, kBlock);

  REQUIRE(rt_left == off_left);
  REQUIRE(rt_right == off_right);
  REQUIRE(realtime.transport().render_frame() == offline.transport().render_frame());
  REQUIRE(realtime.transport().sample_position() == offline.transport().sample_position());
}

#if defined(SONARE_WITH_MIXING)
TEST_CASE("RealtimeEngine::bind_mixing_strip is not noexcept and binds successfully",
          "[engine][realtime]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  sonare::mixing::ChannelStrip strip;

  // bind_mixing_strip re-prepares the strip on a successful bind, which
  // allocates on the control thread. That allocation may throw under OOM, so
  // the function must NOT be declared noexcept; otherwise a propagating
  // std::bad_alloc would call std::terminate. Document the contract here so a
  // re-added noexcept fails the build.
  static_assert(!noexcept(engine.bind_mixing_strip(&strip)),
                "bind_mixing_strip allocates on the control thread and must not be noexcept");

  // Functional regression: a successful bind still works and the strip is
  // bound into the mixing runtime.
  REQUIRE(engine.bind_mixing_strip(&strip));
  REQUIRE(engine.mixing().strip() == &strip);
}
#endif  // defined(SONARE_WITH_MIXING)

#if defined(SONARE_WITH_ARRANGEMENT)
TEST_CASE("RealtimeEngine reports unprepared when prepare fails partway", "[engine]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 256, 16, 16, 8);

  FailingPrepareInstrument instrument;
  REQUIRE(engine.set_midi_instrument(0, &instrument));
  // Binding to an already-prepared engine prepares the instrument, so the arm
  // below is what makes the NEXT prepare fail rather than this one.
  REQUIRE(instrument.prepare_calls() == 1);
  REQUIRE(engine.prepared_scratch_bytes() > 0);

  // Narrower than the configuration in place, so the scratch left behind is
  // larger than the abandoned configuration would address. That keeps the
  // half-prepared engine inside its own buffers and lets the assertions below
  // observe the state rather than a memory fault.
  instrument.arm();
  REQUIRE_THROWS_AS(engine.prepare(44100.0, 64, 16, 16, 2), sonare::SonareException);

  // A failed prepare leaves the engine unprepared: it holds no scratch, and the
  // configuration it half-adopted is not reported as if it had been committed.
  CAPTURE(engine.prepared_scratch_bytes(), engine.prepared_channels(), engine.sample_rate());
  REQUIRE(engine.prepared_scratch_bytes() == 0);

  // process() must refuse rather than render through resources the abandoned
  // prepare never finished committing.
  std::array<float, 64> left{};
  std::array<float, 64> right{};
  left.fill(0.25f);
  right.fill(-0.25f);
  float* stereo[] = {left.data(), right.data()};
  engine.process(stereo, 2, 64);
  REQUIRE(drained_not_prepared(engine));

  // render_offline() must refuse too, and leave the caller's buffer untouched
  // rather than reporting a completed render of silence.
  std::array<float, 64> offline_left{};
  std::array<float, 64> offline_right{};
  offline_left.fill(7.0f);
  offline_right.fill(7.0f);
  float* offline[] = {offline_left.data(), offline_right.data()};
  engine.render_offline(offline, 2, 64, 64);
  REQUIRE(offline_left[0] == Catch::Approx(7.0f));
  REQUIRE(offline_right[0] == Catch::Approx(7.0f));
  REQUIRE(drained_not_prepared(engine));

  // The half-state usually breaks the recovery, so the point of the contract is
  // that a later prepare still fully commits. Compare against an engine that
  // only ever saw the successful call.
  engine.prepare(44100.0, 64, 16, 16, 2);
  REQUIRE(instrument.prepare_calls() == 3);

  sonare::engine::RealtimeEngine reference;
  FailingPrepareInstrument reference_instrument;
  REQUIRE(reference.set_midi_instrument(0, &reference_instrument));
  reference.prepare(44100.0, 64, 16, 16, 2);

  REQUIRE(engine.prepared_channels() == reference.prepared_channels());
  REQUIRE(engine.sample_rate() == reference.sample_rate());
  REQUIRE(engine.prepared_scratch_bytes() == reference.prepared_scratch_bytes());

  left.fill(0.25f);
  right.fill(-0.25f);
  engine.process(stereo, 2, 64);
  REQUIRE(left[0] == Catch::Approx(0.25f));
  REQUIRE(right[0] == Catch::Approx(-0.25f));
  REQUIRE_FALSE(drained_not_prepared(engine));
}

TEST_CASE("RealtimeEngine does not keep a wider configuration a failed prepare abandoned",
          "[engine]") {
  // The widening direction is the dangerous one: the block size and channel
  // count are adopted at the top of prepare() while the scratch they address is
  // sized later, so an engine that kept them would let process() stride past
  // buffers the abandoned prepare never resized. The engine is only inspected
  // here, never rendered, because rendering is the fault this guards against.
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 64, 16, 16, 2);
  const size_t narrow_scratch_bytes = engine.prepared_scratch_bytes();
  REQUIRE(narrow_scratch_bytes > 0);

  FailingPrepareInstrument instrument;
  REQUIRE(engine.set_midi_instrument(0, &instrument));

  instrument.arm();
  REQUIRE_THROWS_AS(engine.prepare(48000.0, 1024, 16, 16, 16), sonare::SonareException);

  CAPTURE(engine.prepared_channels(), engine.prepared_scratch_bytes(), narrow_scratch_bytes);
  REQUIRE(engine.prepared_scratch_bytes() == 0);
}
#endif  // defined(SONARE_WITH_ARRANGEMENT)

#if defined(SONARE_WITH_MIXING)
TEST_CASE("Master telemetry measures input without an owned master strip",
          "[engine][meter][input_peak]") {
  for (const bool external_strip : {false, true}) {
    CAPTURE(external_strip);
    sonare::mixing::ChannelStripConfig config;
    config.enable_metering = false;
    sonare::mixing::ChannelStrip strip(config);
    sonare::engine::RealtimeEngine engine;
    engine.prepare(48000.0, 64);
    engine.set_input_monitor(true, 1.0f);
    if (external_strip) {
      sonare::mastering::dynamics::CompressorConfig compressor;
      compressor.threshold_db = -30.0f;
      compressor.ratio = 8.0f;
      compressor.attack_ms = 0.0f;
      compressor.detector = sonare::mastering::dynamics::DetectorMode::Peak;
      strip.add_pre_insert(std::make_unique<sonare::mastering::dynamics::Compressor>(compressor));
      REQUIRE(engine.bind_mixing_strip(&strip));
      engine.set_mixing_enabled(true);
    }
    std::array<float, 64> left, right;
    left.fill(0.8f);
    right.fill(0.4f);
    float* io[] = {left.data(), right.data()};
    engine.process(io, 2, 64);
    sonare::engine::MeterTelemetryRecord record;
    bool found = false;
    while (engine.pop_meter_telemetry(record)) {
      if (record.target_id != 0) continue;
      found = true;
      CHECK(record.input_peak_db[0] == Catch::Approx(-1.9382f).margin(0.001f));
      CHECK(record.input_peak_db[1] == Catch::Approx(-7.9588f).margin(0.001f));
      if (external_strip)
        CHECK(record.gain_reduction_db < -10.0f);
      else
        CHECK(record.gain_reduction_db == 0.0f);
    }
    REQUIRE(found);
  }
}
#endif

#if defined(SONARE_WITH_MIXING)
namespace {

std::vector<uint32_t> engine_lane_ids(sonare::engine::RealtimeEngine& engine) {
  std::vector<uint32_t> ids(sonare::engine::TrackMixerRuntime::kMaxTrackLanes);
  ids.resize(engine.track_mixer().copy_lane_track_ids(ids.data(), ids.size()));
  return ids;
}

}  // namespace

TEST_CASE("validate_track_lanes mirrors set_track_lanes refusals without mutating",
          "[engine][mixing][timeline-apply-prereq]") {
  using sonare::engine::TrackBusConfig;
  using sonare::engine::TrackLaneConfig;
  using sonare::engine::TrackMixerRuntime;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  TrackBusConfig bus;
  bus.bus_id = 5;
  REQUIRE(engine.set_track_buses({bus}));
  REQUIRE(engine.set_track_lanes({{10}, {20}}));
  const std::vector<uint32_t> before{10, 20};
  REQUIRE(engine_lane_ids(engine) == before);

  std::vector<TrackLaneConfig> too_many(TrackMixerRuntime::kMaxTrackLanes + 1);
  for (size_t i = 0; i < too_many.size(); ++i) too_many[i].track_id = static_cast<uint32_t>(i + 1);
  REQUIRE(too_many.size() == 33);
  TrackLaneConfig routed;
  routed.track_id = 30;
  routed.output_bus_id = 99;
  const std::vector<std::vector<TrackLaneConfig>> refused{too_many, {{7}, {7}}, {{0}}, {routed}};
  for (const auto& lanes : refused) {
    REQUIRE_FALSE(engine.validate_track_lanes(lanes));
    REQUIRE(engine_lane_ids(engine) == before);
    REQUIRE_FALSE(engine.set_track_lanes(lanes));
    REQUIRE(engine_lane_ids(engine) == before);
  }

  routed.output_bus_id = 5;
  const std::vector<TrackLaneConfig> accepted{{40}, routed};
  REQUIRE(engine.validate_track_lanes(accepted));
  REQUIRE(engine_lane_ids(engine) == before);
  REQUIRE(engine.set_track_lanes(accepted));
  const std::vector<uint32_t> after{40, 30};
  REQUIRE(engine_lane_ids(engine) == after);
}

TEST_CASE("validate_track_strip accepts a valid strip and rejects an unacceptable EQ",
          "[engine][mixing][timeline-apply-prereq]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  REQUIRE(engine.set_track_lanes({{10}}));
  const bool mixing_before = engine.mixing_enabled();

  sonare::mixing::api::Strip good;
  REQUIRE(engine.validate_track_strip(good));

  sonare::mixing::api::Strip bad;
  sonare::mastering::eq::EqBand band;
  band.type = sonare::mastering::eq::EqBandType::TiltShelf;
  band.enabled = true;
  bad.eq.bands.push_back(band);
  REQUIRE_FALSE(engine.validate_track_strip(bad));
  REQUIRE_FALSE(engine.set_track_strip(10, bad));

  // Validation bound nothing: the lane set and mixing state are untouched.
  const std::vector<uint32_t> lanes{10};
  REQUIRE(engine_lane_ids(engine) == lanes);
  REQUIRE(engine.mixing_enabled() == mixing_before);
}
#endif

#if defined(SONARE_WITH_ARRANGEMENT)
TEST_CASE("prepare_midi_clips stages without publishing and publish equals set_midi_clips",
          "[engine][midi][timeline-apply-prereq]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 64);

  sonare::midi::MidiClipSchedule clip;
  clip.id = 1;
  clip.length_samples = 1000;
  sonare::midi::MidiEvent note;
  note.render_frame = 10;
  note.ump = sonare::midi::make_midi1_note_on(0, 0, 60, 100);
  clip.events = {note};

  sonare::engine::RealtimeEngine direct;
  direct.prepare(48000.0, 64);
  direct.set_midi_clips({clip});
  REQUIRE(direct.midi_clip_count() == 1);

  engine.set_midi_clips({clip});
  REQUIRE(engine.midi_clip_count() == 1);

  // An unpreparable SysEx (null payload, nonzero size) throws and leaves the published set alone.
  sonare::midi::MidiClipSchedule broken = clip;
  broken.id = 2;
  sonare::midi::MidiEvent sysex;
  sysex.render_frame = 20;
  sysex.ump = sonare::midi::make_sysex_handle(0, 1);
  sysex.sysex_payload = nullptr;
  sysex.sysex_payload_size = 4;
  broken.events.push_back(sysex);
  REQUIRE_THROWS_AS(engine.prepare_midi_clips({clip, broken}), sonare::SonareException);
  REQUIRE(engine.midi_clip_count() == 1);
  REQUIRE_THROWS_AS(engine.set_midi_clips({clip, broken}), sonare::SonareException);
  REQUIRE(engine.midi_clip_count() == 1);

  // Staging alone publishes nothing; publishing lands the same set set_midi_clips would.
  sonare::midi::MidiClipSchedule second = clip;
  second.id = 3;
  auto prepared = engine.prepare_midi_clips({clip, second});
  REQUIRE(engine.midi_clip_count() == 1);
  engine.publish_midi_clips(std::move(prepared));
  direct.set_midi_clips({clip, second});
  REQUIRE(engine.midi_clip_count() == 2);
  REQUIRE(engine.midi_clip_count() == direct.midi_clip_count());
}
#endif
