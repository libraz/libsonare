#include "engine/realtime_engine.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "engine/clip_player.h"
#include "engine/engine_controller.h"
#include "transport/tempo_map.h"

namespace {

template <size_t N>
void fill_signal(std::array<float, N>& left, std::array<float, N>& right) {
  for (size_t i = 0; i < N; ++i) {
    left[i] = static_cast<float>(i) * 0.01f;
    right[i] = -static_cast<float>(i) * 0.02f;
  }
}

class CaptureProcessor final : public sonare::rt::ProcessorBase {
 public:
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  bool set_parameter(unsigned int param_id, float value) override {
    params[static_cast<size_t>(count)] = param_id;
    values[static_cast<size_t>(count)] = value;
    ++count;
    return true;
  }

  std::array<unsigned int, 8> params{};
  std::array<float, 8> values{};
  int count = 0;
};

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
  lane.set_points({{0.0, 0.0f, sonare::automation::CurveType::kLinear},
                   {64.0 / 24000.0, 0.5f, sonare::automation::CurveType::kLinear},
                   {128.0 / 24000.0, 1.0f, sonare::automation::CurveType::kLinear}});
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
