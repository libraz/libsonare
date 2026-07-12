#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

#include "engine/clip_player.h"
#include "engine/track_mixer.h"
#include "mixing/channel_strip.h"
#include "rt/processor_base.h"

namespace {

class AutomatableGainProcessor final : public sonare::rt::ProcessorBase {
 public:
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    for (int ch = 0; ch < num_channels; ++ch) {
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] *= gain_;
      }
    }
  }
  void reset() override {}
  bool set_parameter(unsigned int param_id, float value) override {
    if (param_id != 0 || !std::isfinite(value)) return false;
    gain_ = value;
    return true;
  }
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override {
    return param_id == 0;
  }
  std::vector<sonare::rt::ParamDescriptor> parameter_descriptors() const override {
    return {{"gain", 0}};
  }

 private:
  float gain_ = 1.0f;
};

sonare::engine::ClipSchedule clip_for_track(uint32_t clip_id, uint32_t track_id,
                                            const float* const* samples, int channels, int frames) {
  sonare::engine::ClipSchedule clip{
      clip_id, {samples, channels, frames}, 0.0, 0, 0, frames, false, 1.0f, 0, 0};
  clip.track_id = track_id;
  return clip;
}

bool all_finite(const float* data, int count) noexcept {
  for (int i = 0; i < count; ++i) {
    if (!std::isfinite(data[i])) return false;
  }
  return true;
}

}  // namespace

// The pan setters and resolve_track_insert_param are documented as safe to call
// during playback (glitch-free atomics / read-only resolution enqueued through
// the command queue). Before those paths became read-only they called
// acquire_lanes() -- the audio thread's single-consumer side of the lane
// RtPublisher -- and rewrote lane_states_ concurrently with the render thread's
// per-block remap, so this test tripped RtPublisher's debug single-consumer
// assert and raced the lane table. It must run clean (and data-race-free under
// ThreadSanitizer).
TEST_CASE("TrackMixerRuntime pan and insert-param resolution run concurrently with rendering",
          "[engine][track_mixer][concurrency]") {
  constexpr int kBlock = 128;
  constexpr int kControlIterations = 4000;
  constexpr int kMaxBlocks = 400000;

  std::array<float, kBlock> source{};
  source.fill(0.5f);
  const float* clip_channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, kBlock);
  player.set_clips({clip_for_track(1, 10, clip_channels, 1, kBlock)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, kBlock);
  REQUIRE(mixer.set_track_lanes({{10}}));

  sonare::mixing::ChannelStrip strip;
  strip.add_pre_insert(std::make_unique<AutomatableGainProcessor>());
  REQUIRE(mixer.bind_track_strip(10, &strip));

  std::atomic<bool> control_done{false};
  std::atomic<bool> bad_output{false};
  std::atomic<bool> control_failed{false};

  std::thread audio([&] {
    std::array<float, kBlock> out_l{};
    std::array<float, kBlock> out_r{};
    float* out[] = {out_l.data(), out_r.data()};
    int blocks = 0;
    while (!control_done.load(std::memory_order_acquire) && blocks < kMaxBlocks) {
      out_l.fill(0.0f);
      out_r.fill(0.0f);
      if (!mixer.render_clips(player, out, 2, kBlock, 0) || !all_finite(out_l.data(), kBlock) ||
          !all_finite(out_r.data(), kBlock)) {
        bad_output.store(true, std::memory_order_relaxed);
        break;
      }
      ++blocks;
    }
  });

  for (int i = 0; i < kControlIterations; ++i) {
    const float pan = (i % 2 == 0) ? -0.5f : 0.5f;
    if (!mixer.set_track_pan(10, pan) ||
        !mixer.set_track_pan_law(10, sonare::mixing::PanLaw::Const3dB) ||
        !mixer.set_track_dual_pan(10, -0.25f, 0.25f)) {
      control_failed.store(true, std::memory_order_relaxed);
      break;
    }
    size_t lane_index = 99;
    unsigned int param_id = 99;
    if (!mixer.resolve_track_insert_param(10, 0, "gain", &lane_index, &param_id) ||
        lane_index != 0 || param_id != 0) {
      control_failed.store(true, std::memory_order_relaxed);
      break;
    }
  }
  control_done.store(true, std::memory_order_release);
  audio.join();

  REQUIRE_FALSE(bad_output.load());
  REQUIRE_FALSE(control_failed.load());
}

// Locks in the read-only control-thread resolution semantics: strips resolve
// through the control-thread binding table (external binds, owned strips, and
// send-seeded strips alike), an explicit nullptr unbind hides the strip, the
// resolved lane index is the track's position in the published lane config,
// and bindings survive a lane republish.
TEST_CASE("TrackMixerRuntime resolves control-thread strips without audio lane state",
          "[engine][track_mixer]") {
  constexpr int kBlock = 64;
  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, kBlock);
  REQUIRE(mixer.set_buses({{1, 0.0f, sonare::ChannelLayout::Stereo}}));

  // Lane 10 carries a send, so configure_lane_sends seeds an owned strip for it.
  sonare::engine::TrackLaneConfig lane10{10};
  lane10.sends.push_back({1, 0.0f, true, sonare::mixing::SendTiming::PostFader});
  REQUIRE(mixer.set_track_lanes({lane10, {20}}));

  sonare::mixing::ChannelStrip strip20;
  strip20.add_pre_insert(std::make_unique<AutomatableGainProcessor>());
  REQUIRE(mixer.bind_track_strip(20, &strip20));

  // Send-seeded owned strip resolves for the pan setters.
  REQUIRE(mixer.set_track_pan(10, 0.25f));

  // Externally bound strip resolves; the lane index is the position in the
  // published lane config (track 20 is second).
  size_t lane_index = 99;
  unsigned int param_id = 99;
  REQUIRE(mixer.resolve_track_insert_param(20, 0, "gain", &lane_index, &param_id));
  REQUIRE(lane_index == 1u);
  REQUIRE(param_id == 0u);

  // A track bound to a strip but absent from the published lane config still
  // reaches the pan setters, but cannot resolve an insert param (there is no
  // lane index the audio thread could apply it against).
  sonare::mixing::ChannelStrip strip30;
  strip30.add_pre_insert(std::make_unique<AutomatableGainProcessor>());
  REQUIRE(mixer.bind_track_strip(30, &strip30));
  REQUIRE(mixer.set_track_pan(30, -0.5f));
  REQUIRE_FALSE(mixer.resolve_track_insert_param(30, 0, "gain", &lane_index, &param_id));

  // Unknown track ids fail everywhere.
  REQUIRE_FALSE(mixer.set_track_pan(99, 0.0f));
  REQUIRE_FALSE(mixer.resolve_track_insert_param(99, 0, "gain", &lane_index, &param_id));

  // Republish with the lane order swapped: the binding survives and the
  // resolved index follows the new snapshot position.
  REQUIRE(mixer.set_track_lanes({{20}, lane10}));
  REQUIRE(mixer.resolve_track_insert_param(20, 0, "gain", &lane_index, &param_id));
  REQUIRE(lane_index == 0u);

  // An explicit nullptr unbind hides the strip from every resolution path.
  REQUIRE(mixer.bind_track_strip(20, nullptr));
  REQUIRE_FALSE(mixer.set_track_pan(20, 0.0f));
  REQUIRE_FALSE(mixer.resolve_track_insert_param(20, 0, "gain", &lane_index, &param_id));
}
