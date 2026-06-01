#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "automation/automation_lane.h"
#include "engine/clip_player.h"
#include "engine/realtime_engine.h"
#include "rt/command.h"
#include "transport/marker.h"
#include "transport/tempo_map.h"

namespace {

bool all_finite(const float* data, int count) noexcept {
  for (int i = 0; i < count; ++i) {
    if (!std::isfinite(data[i])) return false;
  }
  return true;
}

// Exercises the lock-free control->audio hand-off end-to-end: one thread runs
// the audio render loop while a second thread concurrently pushes commands and
// invokes the control-thread setters that publish snapshots (clips, tempo,
// markers, automation). The control loop is paced (20 us/iteration) so it never
// laps the bounded RtSnapshot retention window regardless of how aggressively
// the OS deschedules the audio thread under oversubscribed parallel runs.
// Asserts no crash/UB and finite, sane output.
static void run_concurrent_mutation(int blocks) {
  constexpr int kFrames = 128;

  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kFrames);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  // Stable backing storage for clip audio that outlives every published clip
  // set (the control thread keeps re-publishing pointers into these buffers).
  std::array<float, kFrames> clip_l{};
  std::array<float, kFrames> clip_r{};
  clip_l.fill(0.1f);
  clip_r.fill(-0.1f);
  const float* clip_channels[] = {clip_l.data(), clip_r.data()};

  std::atomic<bool> done{false};
  std::atomic<bool> bad_output{false};

  std::thread control([&] {
    for (int i = 0; i < blocks; ++i) {
      sonare::rt::Command seek{};
      seek.type = sonare::rt::CommandType::kTransportSeekSample;
      seek.sample_time = -1;
      seek.arg.i = (i % 2 == 0) ? 0 : kFrames;
      engine.push_command(seek);

      engine.set_tempo(100.0 + static_cast<double>(i % 80));
      engine.set_time_signature(4, 4);
      engine.set_param_smoothing_ms(static_cast<float>(i % 50));

      std::vector<sonare::engine::ClipSchedule> clips;
      clips.emplace_back(static_cast<uint32_t>(i % 4 + 1),
                         sonare::engine::ClipAudioBuffer{clip_channels, 2, kFrames}, 0.0,
                         static_cast<int64_t>(i % kFrames), 0, kFrames, (i % 2 == 0), 0.5f, 8, 8);
      engine.set_clips(std::move(clips));

      std::vector<sonare::transport::Marker> markers;
      sonare::transport::Marker marker{};
      marker.id = static_cast<uint32_t>(i % 7 + 1);
      marker.ppq = static_cast<double>(i % 16);
      markers.push_back(marker);
      engine.set_markers(std::move(markers));

      sonare::automation::AutomationLane lane(11);
      lane.set_points(
          {{0.0, 0.0f, sonare::automation::CurveType::Linear},
           {1.0, static_cast<float>((i % 10) * 0.1f), sonare::automation::CurveType::Linear}});
      engine.automation().set_lanes({lane});

      // Pace the control thread so it stays well within the 64-generation
      // RtSnapshot retention window even if the audio thread is descheduled.
      std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
    done.store(true, std::memory_order_release);
  });

  std::array<float, kFrames> left{};
  std::array<float, kFrames> right{};
  float* io[] = {left.data(), right.data()};
  int blocks_processed = 0;
  while (!done.load(std::memory_order_acquire)) {
    engine.process(io, 2, kFrames);
    ++blocks_processed;
    if (!all_finite(left.data(), kFrames) || !all_finite(right.data(), kFrames)) {
      bad_output.store(true, std::memory_order_relaxed);
      break;
    }
  }
  // A final block after the control thread stopped publishing.
  engine.process(io, 2, kFrames);

  control.join();

  REQUIRE_FALSE(bad_output.load(std::memory_order_relaxed));
  REQUIRE(blocks_processed > 0);
  // Engine stays responsive: the render clock advanced for every block.
  REQUIRE(engine.transport().render_frame() ==
          static_cast<int64_t>(blocks_processed + 1) * kFrames);
  REQUIRE(all_finite(left.data(), kFrames));
  REQUIRE(all_finite(right.data(), kFrames));
}

}  // namespace

TEST_CASE("RealtimeEngine survives concurrent control mutation while processing",
          "[engine][realtime][concurrency]") {
  run_concurrent_mutation(256);
}

#if defined(SONARE_WITH_MIXING)
#include "engine/monitor_runtime.h"
#include "mixing/channel_strip.h"

// TSan-friendly smoke test: one thread renders audio while another toggles
// transport loop state and monitor mute/solo flags. Asserts no crash/UB and
// that final atomic state reads back consistently. Bounded and joinable.
TEST_CASE("RealtimeEngine handles concurrent loop and monitor toggles while processing",
          "[engine][realtime][concurrency][monitor]") {
  constexpr int kFrames = 128;
  constexpr int kIterations = 2000;

  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kFrames);

  sonare::mixing::ChannelStrip strip_a({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  sonare::mixing::ChannelStrip strip_b({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip_a.prepare(48000.0, kFrames);
  strip_b.prepare(48000.0, kFrames);
  REQUIRE(engine.add_monitor_strip(&strip_a));
  REQUIRE(engine.add_monitor_strip(&strip_b));
  engine.set_monitoring_enabled(true);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::atomic<bool> done{false};
  std::atomic<bool> bad_output{false};

  std::thread control([&] {
    for (int i = 0; i < kIterations; ++i) {
      engine.set_loop(0.0, static_cast<double>(1 + (i % 8)), (i % 2 == 0));
      engine.monitor().set_mute(0, (i % 3 == 0));
      engine.monitor().set_solo(1, (i % 5 == 0));
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
    // Deterministic terminal state for the read-back assertions below.
    engine.set_loop(0.0, 4.0, true);
    engine.monitor().set_mute(0, false);
    engine.monitor().set_solo(1, true);
    done.store(true, std::memory_order_release);
  });

  std::array<float, kFrames> left{};
  std::array<float, kFrames> right{};
  float* io[] = {left.data(), right.data()};
  int blocks_processed = 0;
  while (!done.load(std::memory_order_acquire)) {
    engine.process(io, 2, kFrames);
    ++blocks_processed;
    if (!all_finite(left.data(), kFrames) || !all_finite(right.data(), kFrames)) {
      bad_output.store(true, std::memory_order_relaxed);
      break;
    }
  }
  // Drain any toggles published after the last process() above.
  engine.process(io, 2, kFrames);

  control.join();

  REQUIRE_FALSE(bad_output.load(std::memory_order_relaxed));
  REQUIRE(blocks_processed > 0);
  REQUIRE(all_finite(left.data(), kFrames));
  REQUIRE(all_finite(right.data(), kFrames));
  // Final state reads back exactly as the control thread last published it.
  REQUIRE_FALSE(engine.monitor().muted(0));
  REQUIRE(engine.monitor().soloed(1));
}
#endif  // SONARE_WITH_MIXING

// Hidden ([.stress]) long-running soak: same paced body with many more blocks.
// Not run by the default CI/ctest pass; invoke explicitly via the [.stress] tag.
TEST_CASE("RealtimeEngine concurrent control mutation soak",
          "[engine][realtime][concurrency][.stress]") {
  run_concurrent_mutation(20000);
}
