#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
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

}  // namespace

// Exercises the lock-free control->audio hand-off end-to-end: one thread runs
// the audio render loop while a second thread concurrently pushes commands and
// invokes the control-thread setters that publish snapshots (clips, tempo,
// markers, automation). Asserts no crash/UB and finite, sane output.
TEST_CASE("RealtimeEngine survives concurrent control mutation while processing",
          "[engine][realtime][concurrency]") {
  constexpr int kFrames = 128;
  constexpr int kBlocks = 5000;

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
    for (int i = 0; i < kBlocks; ++i) {
      sonare::rt::Command seek{};
      seek.type = sonare::rt::CommandType::kTransportSeekSample;
      seek.sample_time = -1;
      seek.arg.i = (i % 2 == 0) ? 0 : kFrames;
      engine.push_command(seek);

      engine.set_tempo(100.0 + static_cast<double>(i % 80));
      engine.set_time_signature(4, 4);

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
          {{0.0, 0.0f, sonare::automation::CurveType::kLinear},
           {1.0, static_cast<float>((i % 10) * 0.1f), sonare::automation::CurveType::kLinear}});
      engine.automation().set_lanes({lane});
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
