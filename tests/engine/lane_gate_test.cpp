/// @file lane_gate_test.cpp
/// @brief Lane gate snapping, pre-fader sends under mute and solo, and solo-safe lanes.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "engine/track_mixer.h"
#include "mixing/channel_strip.h"
#include "rt/param_smoother.h"
#include "rt/processor_base.h"

namespace {

using sonare::engine::TrackLaneConfig;
using sonare::engine::TrackMixerRuntime;
using sonare::mixing::SendTiming;

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 256;
// 200 ms at 48 kHz in whole blocks.
constexpr int kBlocks200ms = 38;
// The opening ramp needs longer than the closing one to land within the snap threshold.
constexpr int kBlocksSettled = 100;
// Mirrors the mixer's private lane gate snap threshold (-120 dB).
constexpr float kSnap = 1e-6f;

using Plane = std::vector<float>;

float tone_of(uint32_t track, int channel, int64_t frame) {
  const float step =
      0.013f * static_cast<float>(track % 7 + 1) + 0.019f * static_cast<float>(channel);
  return 0.4f * std::sin(step * static_cast<float>(frame));
}

// Records the sidechain key a strip insert was handed (channel 0).
class KeyProbe final : public sonare::rt::ProcessorBase {
 public:
  void prepare(double, int) override {}
  void process(float* const*, int, int num_samples) override {
    for (int i = 0; i < num_samples; ++i) {
      key.push_back(static_cast<size_t>(i) < pending_.size() ? pending_[static_cast<size_t>(i)]
                                                             : 0.0f);
    }
  }
  void reset() override {}
  void set_sidechain(const float* const* channels, int num_channels, int num_samples) override {
    pending_.assign(static_cast<size_t>(num_samples), 0.0f);
    if (channels != nullptr && num_channels > 0 && channels[0] != nullptr) {
      pending_.assign(channels[0], channels[0] + num_samples);
    }
  }
  void clear_sidechain() override { pending_.clear(); }
  std::vector<float> key;

 private:
  std::vector<float> pending_;
};

// Track 10 owns a pre-fader send into bus 1; track 20 is a plain lane.
// @p silent_tracks render as zeros.
struct Rig {
  TrackMixerRuntime mixer;
  sonare::mixing::ChannelStrip strip10;
  sonare::mixing::ChannelStrip strip20;
  std::array<float, kBlock> monitor_l{};
  std::array<float, kBlock> monitor_r{};
  float* monitor_ptrs[2] = {monitor_l.data(), monitor_r.data()};
  Plane last_monitor;
  int64_t frame = 0;

  explicit Rig(bool with_send = true, bool track20_present = true) {
    mixer.prepare(kSampleRate, kBlock);
    REQUIRE(mixer.set_buses({{1, 0.0f}}));
    REQUIRE(mixer.bind_track_strip(10, &strip10));
    REQUIRE(mixer.bind_track_strip(20, &strip20));
    TrackLaneConfig lane{10};
    if (with_send) lane.sends.push_back({1, 0.0f, true, SendTiming::PreFader});
    std::vector<TrackLaneConfig> lanes{lane};
    if (track20_present) lanes.push_back(TrackLaneConfig{20});
    REQUIRE(mixer.set_track_lanes(lanes));
    mixer.set_monitor_bus(monitor_ptrs, 2);
  }

  // Renders @p blocks blocks; returns the final block's left master plane.
  Plane run(int blocks, bool silent10 = false, bool silent20 = false) {
    Plane out;
    for (int b = 0; b < blocks; ++b) {
      std::array<std::array<float, kBlock>, 2> io{};
      float* io_ptrs[] = {io[0].data(), io[1].data()};
      monitor_l.fill(0.0f);
      monitor_r.fill(0.0f);
      REQUIRE(mixer.begin_source_mix(2, kBlock));
      for (uint32_t track : {10u, 20u}) {
        if ((track == 10 && silent10) || (track == 20 && silent20)) {
          std::array<std::array<float, kBlock>, 2> zero{};
          float* src[] = {zero[0].data(), zero[1].data()};
          bool routed = false;
          REQUIRE(mixer.mix_source_into_lane(track, src, io_ptrs, 2, kBlock, routed));
          continue;
        }
        std::array<std::array<float, kBlock>, 2> source{};
        for (int ch = 0; ch < 2; ++ch) {
          for (int i = 0; i < kBlock; ++i) {
            source[static_cast<size_t>(ch)][static_cast<size_t>(i)] = tone_of(track, ch, frame + i);
          }
        }
        float* src[] = {source[0].data(), source[1].data()};
        bool routed = false;
        REQUIRE(mixer.mix_source_into_lane(track, src, io_ptrs, 2, kBlock, routed));
      }
      mixer.finish_source_mix(io_ptrs, 2, kBlock);
      frame += kBlock;
      out.assign(io[0].begin(), io[0].end());
      last_monitor.assign(monitor_l.begin(), monitor_l.end());
    }
    return out;
  }
};

bool all_zero(const Plane& p) {
  return std::all_of(p.begin(), p.end(), [](float s) { return s == 0.0f; });
}

float peak_of(const Plane& p) {
  float v = 0.0f;
  for (float s : p) v = std::max(v, std::abs(s));
  return v;
}

}  // namespace

TEST_CASE("A muted lane's master and pre-fader send contributions are exactly zero after 200 ms",
          "[engine][lane_gate]") {
  Rig rig;
  REQUIRE(peak_of(rig.run(4)) > 0.1f);
  // Control: the send is audible, so the muted silence below is the gate.
  Rig no_send(false);
  REQUIRE(peak_of(rig.run(1)) != peak_of(no_send.run(5)));

  REQUIRE(rig.mixer.set_lane_solo_mute(0, false, true));
  rig.run(kBlocks200ms);
  // Track 20 is silenced so only lane 0 (direct + pre-fader send) could remain.
  const Plane out = rig.run(1, false, true);
  CHECK(all_zero(out));
}

TEST_CASE("Unmuting returns the lane gate to exactly unity", "[engine][lane_gate]") {
  Rig muted;
  Rig twin;
  REQUIRE(muted.mixer.set_lane_solo_mute(0, false, true));
  muted.run(kBlocks200ms);
  twin.run(kBlocks200ms);
  REQUIRE(muted.mixer.set_lane_solo_mute(0, false, false));
  // Align the signal clocks, then let the opening ramp finish.
  muted.run(kBlocksSettled);
  twin.run(kBlocksSettled);
  const Plane a = muted.run(1);
  const Plane b = twin.run(1);
  REQUIRE(peak_of(a) > 0.1f);
  CHECK(a == b);
}

TEST_CASE("Another lane's solo silences this lane's pre-fader send", "[engine][lane_gate]") {
  Rig soloed;
  REQUIRE(soloed.mixer.set_lane_solo_mute(1, true, false));
  soloed.run(kBlocks200ms);
  const Plane got = soloed.run(1);
  // Reference: the soloed lane alone (lane 0 silent), no solo set.
  Rig alone;
  alone.run(kBlocks200ms, true);
  const Plane ref = alone.run(1, true);
  REQUIRE(peak_of(ref) > 0.1f);
  CHECK(got.size() == ref.size());
  // Same frame offset on both rigs, so lane 20's tone lines up sample for sample.
  CHECK(got == ref);
}

TEST_CASE("A solo-safe lane stays audible, sends included, under another lane's solo",
          "[engine][lane_gate]") {
  Rig safe;
  safe.strip10.set_solo_safe(true);
  REQUIRE(safe.mixer.set_lane_solo_mute(1, true, false));
  safe.run(kBlocks200ms);
  const Plane got = safe.run(1);

  Rig reference;  // no solo anywhere
  reference.run(kBlocks200ms);
  const Plane ref = reference.run(1);
  REQUIRE(peak_of(ref) > 0.1f);
  CHECK(got == ref);

  Rig unsafe;
  REQUIRE(unsafe.mixer.set_lane_solo_mute(1, true, false));
  unsafe.run(kBlocks200ms);
  CHECK(unsafe.run(1) != ref);
}

TEST_CASE("PFL and the sidechain key are still produced while the lane is muted",
          "[engine][lane_gate]") {
  Rig muted;
  Rig twin;
  muted.mixer.set_lane_monitor_mode(0, sonare::engine::TrackMonitorMode::kPfl);
  twin.mixer.set_lane_monitor_mode(0, sonare::engine::TrackMonitorMode::kPfl);
  REQUIRE(muted.mixer.set_lane_solo_mute(0, false, true));
  muted.run(kBlocks200ms);
  twin.run(kBlocks200ms);
  muted.run(1);
  twin.run(1);
  REQUIRE(peak_of(twin.last_monitor) > 0.1f);
  CHECK(muted.last_monitor == twin.last_monitor);

  // Source lane 20 muted, destination lane 30 keyed from it.
  const auto key_of = [](bool mute_source) {
    TrackMixerRuntime mixer;
    mixer.prepare(kSampleRate, kBlock);
    auto* probe = new KeyProbe();
    sonare::mixing::ChannelStrip dest;
    dest.add_pre_insert(std::unique_ptr<sonare::rt::ProcessorBase>(probe));
    REQUIRE(mixer.bind_track_strip(30, &dest));
    REQUIRE(mixer.set_track_lanes({TrackLaneConfig{20}, TrackLaneConfig{30}}));
    REQUIRE(mixer.set_lane_sidechain(30, 0, 20));
    if (mute_source) REQUIRE(mixer.set_lane_solo_mute(0, false, true));
    int64_t frame = 0;
    for (int b = 0; b < kBlocks200ms + 2; ++b) {
      std::array<std::array<float, kBlock>, 2> io{};
      float* io_ptrs[] = {io[0].data(), io[1].data()};
      REQUIRE(mixer.begin_source_mix(2, kBlock));
      for (uint32_t track : {20u, 30u}) {
        std::array<std::array<float, kBlock>, 2> source{};
        for (int ch = 0; ch < 2; ++ch) {
          for (int i = 0; i < kBlock; ++i) {
            source[static_cast<size_t>(ch)][static_cast<size_t>(i)] =
                track == 20 ? tone_of(track, ch, frame + i) : 0.0f;
          }
        }
        float* src[] = {source[0].data(), source[1].data()};
        bool routed = false;
        REQUIRE(mixer.mix_source_into_lane(track, src, io_ptrs, 2, kBlock, routed));
      }
      mixer.finish_source_mix(io_ptrs, 2, kBlock);
      frame += kBlock;
    }
    return probe->key;
  };
  const std::vector<float> muted_key = key_of(true);
  const std::vector<float> open_key = key_of(false);
  REQUIRE(peak_of(open_key) > 0.1f);
  CHECK(muted_key == open_key);
}

TEST_CASE("Snapping reaches an exact zero that a plain smoother does not", "[engine][lane_gate]") {
  constexpr int kSamples = 9600;  // 200 ms at 48 kHz
  sonare::rt::ParamSmoother plain(1.0f, 10.0f, kSampleRate);
  sonare::rt::ParamSmoother snapping(1.0f, 10.0f, kSampleRate);
  plain.set_target(0.0f);
  snapping.set_target(0.0f);
  float p = 1.0f;
  float s = 1.0f;
  for (int i = 0; i < kSamples; ++i) {
    p = plain.process();
    s = snapping.process_snapping(kSnap);
  }
  CHECK(p != 0.0f);
  CHECK(s == 0.0f);

  // Upward: the gate lands on exactly 1.0f.
  snapping.set_target(1.0f);
  for (int i = 0; i < 3 * kSamples; ++i) s = snapping.process_snapping(kSnap);
  CHECK(s == 1.0f);
}
