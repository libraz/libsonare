#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

#include "engine/clip_player.h"
#include "engine/track_mixer.h"
#include "mixing/channel_strip.h"
#include "mixing/panner.h"
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
  bool set_parameter_impl(unsigned int param_id, float value) override {
    if (param_id != 0) return false;
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

// The bus pan setters write panner atomics (and the control-side retained spec,
// which the render never reads), so they are safe while a bus renders.
TEST_CASE("TrackMixerRuntime bus pan setters run concurrently with rendering",
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
  REQUIRE(mixer.set_buses({{1, 0.0f, sonare::ChannelLayout::Stereo}}));
  sonare::engine::TrackLaneConfig lane{10};
  lane.output_bus_id = 1;
  REQUIRE(mixer.set_track_lanes({lane}));

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
    const auto mode =
        (i % 3 == 0) ? sonare::mixing::PanMode::DualPan : sonare::mixing::PanMode::Balance;
    if (!mixer.set_bus_pan(1, pan) || !mixer.set_bus_pan_law(1, sonare::mixing::PanLaw::Const3dB) ||
        !mixer.set_bus_pan_mode(1, mode) || !mixer.set_bus_dual_pan(1, -0.25f, 0.25f)) {
      control_failed.store(true, std::memory_order_relaxed);
      break;
    }
  }
  control_done.store(true, std::memory_order_release);
  audio.join();

  REQUIRE_FALSE(bad_output.load());
  REQUIRE_FALSE(control_failed.load());
}

// set_lane_sidechain (control thread) publishes the whole binding table through
// a seqlock, and the audio thread takes one copy of it per block. Toggling a
// binding on/off while the render thread spins must stay data-race-free
// (ThreadSanitizer) and never observe a count that outruns its binding.
TEST_CASE("TrackMixerRuntime sidechain binding toggles run concurrently with rendering",
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
  // Lane 10 hosts the keyed insert; lane 20 is the sidechain source.
  REQUIRE(mixer.set_track_lanes({{10}, {20}}));

  sonare::mixing::ChannelStrip strip10;
  strip10.add_pre_insert(std::make_unique<AutomatableGainProcessor>());
  REQUIRE(mixer.bind_track_strip(10, &strip10));
  sonare::mixing::ChannelStrip strip20;
  strip20.add_pre_insert(std::make_unique<AutomatableGainProcessor>());
  REQUIRE(mixer.bind_track_strip(20, &strip20));

  std::atomic<bool> control_done{false};
  std::atomic<bool> bad_output{false};

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
    // Add then drop the binding; the atomic count crosses 0<->1 every pass.
    mixer.set_lane_sidechain(10, 0, 20);
    mixer.set_lane_sidechain(10, 0, 0);
  }
  control_done.store(true, std::memory_order_release);
  audio.join();

  REQUIRE_FALSE(bad_output.load());
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

namespace {

// Records the first key sample each block delivered (NaN when unkeyed) and can
// run a hook from inside its own process() -- a point in the middle of the
// mixer's per-block pass over the binding table.
class KeyProbeProcessor final : public sonare::rt::ProcessorBase {
 public:
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {
    observed_ = pending_;
    pending_ = std::numeric_limits<float>::quiet_NaN();
    if (hook_) {
      auto hook = std::move(hook_);
      hook_ = nullptr;
      hook();
    }
  }
  void reset() override {}
  void set_sidechain(const float* const* channels, int num_channels, int num_samples) override {
    pending_ = (num_channels > 0 && num_samples > 0 && channels[0] != nullptr)
                   ? channels[0][0]
                   : std::numeric_limits<float>::quiet_NaN();
  }
  void clear_sidechain() override { pending_ = std::numeric_limits<float>::quiet_NaN(); }

  float observed() const noexcept { return observed_; }
  void arm(std::function<void()> hook) { hook_ = std::move(hook); }

 private:
  float pending_ = std::numeric_limits<float>::quiet_NaN();
  float observed_ = std::numeric_limits<float>::quiet_NaN();
  std::function<void()> hook_;
};

bool keyed_by(float observed, float expected) noexcept {
  return std::isfinite(observed) && std::fabs(observed - expected) < 1e-6f;
}

// Three constant-valued source lanes (20/30/40) render first; lanes 10/11/12
// each key insert 0 from one of them, so a key's value names its source.
struct KeyedMixerFixture {
  static constexpr int kBlock = 64;
  std::array<std::array<float, kBlock>, 3> source{};
  std::array<const float*, 3> source_planes{};
  sonare::engine::ClipPlayer player;
  sonare::engine::TrackMixerRuntime mixer;
  std::array<sonare::mixing::ChannelStrip, 3> strips;
  std::array<KeyProbeProcessor*, 3> probes{};

  KeyedMixerFixture() {
    const std::array<float, 3> values{0.2f, 0.3f, 0.4f};
    std::vector<sonare::engine::ClipSchedule> clips;
    for (size_t i = 0; i < 3; ++i) {
      source[i].fill(values[i]);
      source_planes[i] = source[i].data();
      clips.push_back(clip_for_track(static_cast<uint32_t>(i + 1),
                                     static_cast<uint32_t>(20 + 10 * i), &source_planes[i], 1,
                                     kBlock));
    }
    player.prepare(48000.0, kBlock);
    player.set_clips(clips);
    mixer.prepare(48000.0, kBlock);
  }

  bool configure() {
    if (!mixer.set_track_lanes({{20}, {30}, {40}, {10}, {11}, {12}})) return false;
    for (size_t i = 0; i < 3; ++i) {
      auto probe = std::make_unique<KeyProbeProcessor>();
      probes[i] = probe.get();
      strips[i].add_pre_insert(std::move(probe));
      const auto keyed = static_cast<uint32_t>(10 + i);
      if (!mixer.bind_track_strip(keyed, &strips[i])) return false;
      if (!mixer.set_lane_sidechain(keyed, 0, static_cast<uint32_t>(20 + 10 * i))) return false;
    }
    return true;
  }

  bool render() {
    std::array<float, kBlock> out_l{};
    std::array<float, kBlock> out_r{};
    float* out[] = {out_l.data(), out_r.data()};
    return mixer.render_clips(player, out, 2, kBlock, 0);
  }
};

}  // namespace

// Removing binding 1 of 3 compacts binding 2 into its slot. Landing that removal
// in the middle of a block (from lane 10's insert, after lane 10's key and before
// lane 11's) must leave the whole block on the table it started with; the next
// block sees the compacted table whole.
TEST_CASE("TrackMixerRuntime sidechain removal publishes the compacted table as one snapshot",
          "[engine][track_mixer][concurrency]") {
  KeyedMixerFixture fixture;
  REQUIRE(fixture.configure());

  REQUIRE(fixture.render());
  CHECK(keyed_by(fixture.probes[0]->observed(), 0.2f));
  CHECK(keyed_by(fixture.probes[1]->observed(), 0.3f));
  CHECK(keyed_by(fixture.probes[2]->observed(), 0.4f));

  bool removed = false;
  fixture.probes[0]->arm([&] { removed = fixture.mixer.set_lane_sidechain(11, 0, 0); });
  REQUIRE(fixture.render());
  REQUIRE(removed);
  CHECK(keyed_by(fixture.probes[0]->observed(), 0.2f));
  CHECK(keyed_by(fixture.probes[1]->observed(), 0.3f));
  CHECK(keyed_by(fixture.probes[2]->observed(), 0.4f));

  REQUIRE(fixture.render());
  CHECK(keyed_by(fixture.probes[0]->observed(), 0.2f));
  CHECK(std::isnan(fixture.probes[1]->observed()));
  CHECK(keyed_by(fixture.probes[2]->observed(), 0.4f));
}

// Removing and re-adding a non-last binding compacts the table on every pass
// while the render thread runs. Every key a probe receives must come from its
// own source; a binding assembled from two table states would key a lane from
// another lane's source.
TEST_CASE("TrackMixerRuntime sidechain compaction never misroutes a key under rendering",
          "[engine][track_mixer][concurrency]") {
  constexpr int kControlIterations = 4000;
  constexpr int kMaxBlocks = 400000;
  KeyedMixerFixture fixture;
  REQUIRE(fixture.configure());

  std::atomic<bool> control_done{false};
  std::atomic<bool> misrouted{false};
  std::thread audio([&] {
    const std::array<float, 3> expected{0.2f, 0.3f, 0.4f};
    int blocks = 0;
    while (!control_done.load(std::memory_order_acquire) && blocks < kMaxBlocks) {
      if (!fixture.render()) {
        misrouted.store(true, std::memory_order_relaxed);
        break;
      }
      for (size_t i = 0; i < 3; ++i) {
        const float observed = fixture.probes[i]->observed();
        if (!std::isnan(observed) && !keyed_by(observed, expected[i])) {
          misrouted.store(true, std::memory_order_relaxed);
        }
      }
      ++blocks;
    }
  });

  for (int i = 0; i < kControlIterations; ++i) {
    const auto keyed = static_cast<uint32_t>(10 + (i % 2));
    fixture.mixer.set_lane_sidechain(keyed, 0, 0);
    fixture.mixer.set_lane_sidechain(keyed, 0, keyed == 10 ? 20 : 30);
  }
  control_done.store(true, std::memory_order_release);
  audio.join();

  REQUIRE_FALSE(misrouted.load());
}
