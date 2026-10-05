#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/realtime_engine.h"
#include "engine/track_mixer.h"
#include "mixing/api/scene.h"
#include "mixing/downmix.h"
#include "rt/command.h"
#include "support/alloc_guard.h"
#include "util/db.h"

namespace {

using sonare::ChannelLayout;
using sonare::engine::SidechainSourceKind;
using sonare::engine::TrackBusConfig;
using sonare::engine::TrackLaneConfig;
using sonare::engine::TrackMixerRuntime;
using sonare::mixing::SendTiming;
using sonare::mixing::api::Bus;
using sonare::mixing::api::InsertSlot;
using sonare::mixing::api::Strip;

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 256;

using Planes = std::vector<std::vector<float>>;
using Signal = float (*)(uint32_t track, int channel, int64_t frame);

float tone(uint32_t track, int channel, int64_t frame) {
  const float step =
      0.013f * static_cast<float>(track % 7 + 1) + 0.019f * static_cast<float>(channel);
  return 0.4f * std::sin(step * static_cast<float>(frame));
}

// Silent until frame 1024, then a tone: the onset is where a keyed insert has
// to react on the same sample as the program it ducks.
float burst(uint32_t, int channel, int64_t frame) {
  return frame < 1024 ? 0.0f : 0.5f * std::sin(0.13f * static_cast<float>(frame + channel));
}

float impulse(uint32_t, int, int64_t frame) { return frame == 0 ? 0.25f : 0.0f; }

// Mixes every track in @p tracks through its lane for @p blocks blocks.
Planes render(TrackMixerRuntime& mixer, const std::vector<uint32_t>& tracks, int channels,
              int blocks, Signal signal = tone) {
  Planes out(static_cast<size_t>(channels));
  std::array<std::array<float, kBlock>, 2> source{};
  std::vector<std::array<float, kBlock>> io(static_cast<size_t>(channels));
  std::vector<float*> io_ptrs(static_cast<size_t>(channels));
  for (int c = 0; c < channels; ++c)
    io_ptrs[static_cast<size_t>(c)] = io[static_cast<size_t>(c)].data();
  for (int block = 0; block < blocks; ++block) {
    for (auto& plane : io) plane.fill(0.0f);
    REQUIRE(mixer.begin_source_mix(channels, kBlock));
    bool any = false;
    for (uint32_t track : tracks) {
      for (int ch = 0; ch < 2; ++ch) {
        for (int i = 0; i < kBlock; ++i) {
          source[static_cast<size_t>(ch)][static_cast<size_t>(i)] =
              signal(track, ch, static_cast<int64_t>(block) * kBlock + i);
        }
      }
      float* src[] = {source[0].data(), source[1].data()};
      bool routed = false;
      REQUIRE(mixer.mix_source_into_lane(track, src, io_ptrs.data(), channels, kBlock, routed));
      any = any || routed;
    }
    if (any) mixer.finish_source_mix(io_ptrs.data(), channels, kBlock);
    for (int c = 0; c < channels; ++c) {
      out[static_cast<size_t>(c)].insert(out[static_cast<size_t>(c)].end(),
                                         io[static_cast<size_t>(c)].begin(),
                                         io[static_cast<size_t>(c)].end());
    }
  }
  return out;
}

float peak(const Planes& planes) {
  float value = 0.0f;
  for (const auto& plane : planes) {
    for (float s : plane) value = std::max(value, std::abs(s));
  }
  return value;
}

double rms_db(const std::vector<float>& plane, size_t from = 0) {
  REQUIRE(from < plane.size());
  double sum = 0.0;
  for (size_t i = from; i < plane.size(); ++i) sum += static_cast<double>(plane[i]) * plane[i];
  const double mean = sum / static_cast<double>(plane.size() - from);
  return 10.0 * std::log10(std::max(mean, 1.0e-30));
}

// max|a-b| <= 1e-6 * max(1, max|a|), the agreement bound of the routing tests.
bool agree(const Planes& a, const Planes& b) {
  if (a.size() != b.size()) return false;
  float diff = 0.0f;
  for (size_t c = 0; c < a.size(); ++c) {
    if (a[c].size() != b[c].size()) return false;
    for (size_t i = 0; i < a[c].size(); ++i) diff = std::max(diff, std::abs(a[c][i] - b[c][i]));
  }
  return diff <= 1.0e-6f * std::max(1.0f, peak(a));
}

TrackBusConfig bus_config(uint32_t id, float gain_db = 0.0f, uint32_t output = 0,
                          std::vector<TrackLaneConfig::Send> sends = {},
                          ChannelLayout layout = ChannelLayout::Stereo) {
  TrackBusConfig config;
  config.bus_id = id;
  config.gain_db = gain_db;
  config.layout = layout;
  config.output_bus_id = output;
  config.sends = std::move(sends);
  return config;
}

TrackLaneConfig lane_to(uint32_t track, uint32_t output_bus) {
  TrackLaneConfig lane{track};
  lane.output_bus_id = output_bus;
  return lane;
}

TrackLaneConfig::Send send_to(uint32_t bus, SendTiming timing, float level_db = 0.0f,
                              bool enabled = true) {
  TrackLaneConfig::Send send;
  send.bus_id = bus;
  send.level_db = level_db;
  send.enabled = enabled;
  send.timing = timing;
  return send;
}

Bus ducker_bus(uint32_t id, int extra_leading_inserts = 0) {
  Bus bus;
  bus.id = std::to_string(id);
  for (int i = 0; i < extra_leading_inserts; ++i) {
    bus.inserts.push_back({InsertSlot::PreFader, "dynamics.limiter",
                           R"({"thresholdDb":24,"lookaheadMs":0,"releaseMs":50})"});
  }
  bus.inserts.push_back(
      {InsertSlot::PreFader, "dynamics.duckingProcessor",
       R"({"thresholdDb":-20,"ratio":20,"attackMs":0.05,"releaseMs":80,"rangeDb":30})"});
  return bus;
}

Bus latent_bus(uint32_t id, float lookahead_ms) {
  Bus bus;
  bus.id = std::to_string(id);
  bus.inserts.push_back(
      {InsertSlot::PreFader, "dynamics.limiter",
       R"({"thresholdDb":24,"releaseMs":50,"lookaheadMs":)" + std::to_string(lookahead_ms) + "}"});
  return bus;
}

Planes scaled(const Planes& planes, float gain) {
  Planes out = planes;
  for (auto& plane : out) {
    for (float& s : plane) s *= gain;
  }
  return out;
}

// The dry per-track signal as the lane stage would sum it (no strip, centred).
Planes dry(uint32_t track, int blocks, Signal signal = tone) {
  Planes out(2);
  for (int ch = 0; ch < 2; ++ch) {
    for (int64_t n = 0; n < static_cast<int64_t>(blocks) * kBlock; ++n) {
      out[static_cast<size_t>(ch)].push_back(signal(track, ch, n));
    }
  }
  return out;
}

Planes minus(const Planes& a, const Planes& b) {
  Planes out = a;
  for (size_t c = 0; c < out.size(); ++c) {
    for (size_t i = 0; i < out[c].size(); ++i) out[c][i] -= b[c][i];
  }
  return out;
}

}  // namespace

TEST_CASE("Track mixer folds a surround bus into a narrower master by downmix",
          "[track_mixer_routing]") {
  constexpr int kBlocks = 4;
  const auto configure = [](TrackMixerRuntime& mixer) {
    mixer.prepare(kSampleRate, kBlock);
    REQUIRE(mixer.set_buses({bus_config(1, 0.0f, 0, {}, ChannelLayout::FivePointOne)}));
    REQUIRE(mixer.set_track_lanes({lane_to(10, 1)}));
    Strip spec;
    spec.id = "10";
    spec.surround_pan.azimuth = -60.0f;  // between L and Ls, feeding C/Ls as well
    REQUIRE(mixer.set_track_strip(10, spec));
    mixer.settle_smoothers();
  };
  TrackMixerRuntime wide;
  TrackMixerRuntime narrow;
  configure(wide);
  configure(narrow);
  const Planes six = render(wide, {10}, 6, kBlocks);
  const Planes two = render(narrow, {10}, 2, kBlocks);

  Planes expected(2, std::vector<float>(six[0].size()));
  std::array<const float*, 6> in{};
  for (size_t c = 0; c < 6; ++c) in[c] = six[c].data();
  float* out[] = {expected[0].data(), expected[1].data()};
  sonare::mixing::downmix(ChannelLayout::FivePointOne, ChannelLayout::Stereo, in.data(), out,
                          six[0].size());
  REQUIRE(rms_db(expected[0]) > -40.0);
  // The surround planes carry part of the signal, so a front-pair truncation
  // would lose it.
  REQUIRE(peak({six[4]}) > 0.01f);
  CHECK(agree(expected, two));
}

TEST_CASE("Track mixer puts a stereo bus on the front pair of a wider master",
          "[track_mixer_routing]") {
  TrackMixerRuntime mixer;
  mixer.prepare(kSampleRate, kBlock);
  REQUIRE(mixer.set_buses({bus_config(1)}));
  REQUIRE(mixer.set_track_lanes({lane_to(10, 1)}));
  const Planes six = render(mixer, {10}, 6, 2);
  REQUIRE(rms_db(six[0]) > -40.0);
  for (size_t c = 2; c < 6; ++c) {
    CHECK(peak({six[c]}) == 0.0f);
  }
}

TEST_CASE("Track mixer routes a bus output into another bus", "[track_mixer_routing]") {
  constexpr int kBlocks = 3;
  TrackMixerRuntime mixer;
  mixer.prepare(kSampleRate, kBlock);
  // Declared destination-first, so the processing order has to follow the edge.
  REQUIRE(mixer.set_buses({bus_config(2, -3.0f), bus_config(1, -6.0f, 2)}));
  REQUIRE(mixer.set_track_lanes({lane_to(10, 1)}));
  mixer.settle_smoothers();
  const Planes out = render(mixer, {10}, 2, kBlocks);
  const Planes expected =
      scaled(dry(10, kBlocks), sonare::db_to_linear(-6.0f) * sonare::db_to_linear(-3.0f));
  REQUIRE(rms_db(expected[0]) > -40.0);
  CHECK(agree(expected, out));
}

TEST_CASE("Track mixer taps a pre-fader bus send before gain_db and a post one after",
          "[track_mixer_routing]") {
  constexpr int kBlocks = 3;
  const float g1 = sonare::db_to_linear(-12.0f);
  const auto run = [&](std::vector<TrackLaneConfig::Send> sends) {
    TrackMixerRuntime mixer;
    mixer.prepare(kSampleRate, kBlock);
    REQUIRE(mixer.set_buses({bus_config(1, -12.0f, 0, std::move(sends)), bus_config(2)}));
    REQUIRE(mixer.set_track_lanes({lane_to(10, 1)}));
    mixer.settle_smoothers();
    return render(mixer, {10}, 2, kBlocks);
  };
  const Planes x = dry(10, kBlocks);
  REQUIRE(rms_db(x[0]) > -40.0);
  CHECK(agree(scaled(x, g1 + 1.0f), run({send_to(2, SendTiming::PreFader)})));
  CHECK(agree(scaled(x, 2.0f * g1), run({send_to(2, SendTiming::PostFader)})));
  const float half = sonare::db_to_linear(-6.0f);
  CHECK(agree(scaled(x, g1 + half), run({send_to(2, SendTiming::PreFader, -6.0f)})));
  // A disabled send sits at the floor, like a disabled lane send.
  const float floor = sonare::db_to_linear(sonare::constants::kFloorDb);
  CHECK(agree(scaled(x, g1 + floor * g1), run({send_to(2, SendTiming::PostFader, 0.0f, false)})));
}

TEST_CASE("Track mixer refuses undeclared, self-referencing and cyclic bus routing",
          "[track_mixer_routing]") {
  TrackMixerRuntime mixer;
  TrackMixerRuntime control;
  for (TrackMixerRuntime* m : {&mixer, &control}) {
    m->prepare(kSampleRate, kBlock);
    REQUIRE(m->set_buses({bus_config(1, -3.0f, 2), bus_config(2)}));
    REQUIRE(m->set_track_lanes({lane_to(10, 1), lane_to(20, 2)}));
    REQUIRE(m->set_bus_strip(2, ducker_bus(2)));
    // Bus 1 keys bus 2: an edge 1 -> 2 on top of the output edge.
    REQUIRE(m->set_bus_sidechain(2, 0, SidechainSourceKind::Bus, 1));
  }
  const auto pre = send_to(1, SendTiming::PreFader);
  CHECK_FALSE(mixer.set_buses({bus_config(1, 0.0f, 1), bus_config(2)}));           // self output
  CHECK_FALSE(mixer.set_buses({bus_config(1, 0.0f, 9), bus_config(2)}));           // unknown output
  CHECK_FALSE(mixer.set_buses({bus_config(1, 0.0f, 2), bus_config(2, 0.0f, 1)}));  // cycle
  CHECK_FALSE(mixer.set_buses({bus_config(1, 0.0f, 0, {pre}), bus_config(2)}));    // self send
  CHECK_FALSE(
      mixer.set_buses({bus_config(1, 0.0f, 0, {send_to(9, SendTiming::PreFader)}), bus_config(2)}));
  CHECK_FALSE(mixer.set_buses({bus_config(1), bus_config(2, 0.0f, 0, {pre, pre})}));
  CHECK_FALSE(mixer.set_buses({bus_config(1), bus_config(2, 0.0f, 0, {pre})}));  // send + key
  CHECK_FALSE(mixer.set_buses({bus_config(1), bus_config(2, 0.0f, 1)}));         // output + key
  // Keys: self, unknown source, unknown bus, out-of-range insert, cycle.
  CHECK_FALSE(mixer.set_bus_sidechain(2, 0, SidechainSourceKind::Bus, 2));
  CHECK_FALSE(mixer.set_bus_sidechain(2, 0, SidechainSourceKind::Bus, 9));
  CHECK_FALSE(mixer.set_bus_sidechain(2, 0, SidechainSourceKind::Track, 99));
  CHECK_FALSE(mixer.set_bus_sidechain(9, 0, SidechainSourceKind::Bus, 1));
  CHECK_FALSE(mixer.set_bus_sidechain(2, 1, SidechainSourceKind::Bus, 1));
  REQUIRE(mixer.set_bus_strip(1, ducker_bus(1)));
  REQUIRE(control.set_bus_strip(1, ducker_bus(1)));
  CHECK_FALSE(mixer.set_bus_sidechain(1, 0, SidechainSourceKind::Bus, 2));
  CHECK_FALSE(mixer.set_bus_sidechain(1, 0, static_cast<SidechainSourceKind>(7), 20));
  mixer.settle_smoothers();
  control.settle_smoothers();
  const Planes a = render(mixer, {10, 20}, 2, 4);
  const Planes b = render(control, {10, 20}, 2, 4);
  REQUIRE(rms_db(b[0]) > -40.0);
  CHECK(agree(b, a));
}

TEST_CASE("Track mixer drops a bus key whose source bus is retired", "[track_mixer_routing]") {
  const auto rig = [](TrackMixerRuntime& m, bool bind, bool retire) {
    m.prepare(kSampleRate, kBlock);
    REQUIRE(m.set_buses({bus_config(1), bus_config(2)}));
    REQUIRE(m.set_track_lanes({lane_to(10, 1), lane_to(20, 2)}));
    REQUIRE(m.set_bus_strip(2, ducker_bus(2)));
    if (bind) REQUIRE(m.set_bus_sidechain(2, 0, SidechainSourceKind::Bus, 1));
    // Lane 10 leaves bus 1 first: a bus a lane still uses cannot be retired.
    REQUIRE(m.set_track_lanes({lane_to(20, 2)}));
    if (retire) REQUIRE(m.set_buses({bus_config(2)}));
    REQUIRE(m.set_buses({bus_config(1), bus_config(2)}));
    REQUIRE(m.set_track_lanes({lane_to(10, 1), lane_to(20, 2)}));
    m.settle_smoothers();
  };
  TrackMixerRuntime retired;
  TrackMixerRuntime never;
  TrackMixerRuntime kept;
  rig(retired, true, true);
  rig(never, false, false);
  rig(kept, true, false);
  const Planes a = render(retired, {10, 20}, 2, 6);
  const Planes b = render(never, {10, 20}, 2, 6);
  const Planes c = render(kept, {10, 20}, 2, 6);
  CHECK(agree(b, a));
  CHECK_FALSE(agree(b, c));  // non-vacuity: the kept key ducks
}

TEST_CASE("Track mixer drops a bus key whose insert disappears", "[track_mixer_routing]") {
  // Index 1 of the scene inserts order is the ducker.
  TrackMixerRuntime mixer;
  TrackMixerRuntime control;
  for (TrackMixerRuntime* m : {&mixer, &control}) {
    m->prepare(kSampleRate, kBlock);
    REQUIRE(m->set_buses({bus_config(1), bus_config(2)}));
    REQUIRE(m->set_track_lanes({lane_to(10, 1), lane_to(20, 2)}));
    REQUIRE(m->set_bus_strip(2, ducker_bus(2, 1)));
  }
  REQUIRE(mixer.set_bus_sidechain(2, 1, SidechainSourceKind::Bus, 1));
  CHECK_FALSE(mixer.set_bus_sidechain(2, 2, SidechainSourceKind::Bus, 1));
  TrackMixerRuntime keyed;
  keyed.prepare(kSampleRate, kBlock);
  REQUIRE(keyed.set_buses({bus_config(1), bus_config(2)}));
  REQUIRE(keyed.set_track_lanes({lane_to(10, 1), lane_to(20, 2)}));
  REQUIRE(keyed.set_bus_strip(2, ducker_bus(2, 1)));
  REQUIRE(keyed.set_bus_sidechain(2, 1, SidechainSourceKind::Bus, 1));
  keyed.settle_smoothers();
  control.settle_smoothers();
  CHECK_FALSE(agree(render(control, {10, 20}, 2, 6), render(keyed, {10, 20}, 2, 6)));

  // Rebuilding bus 2 with the ducker alone leaves index 1 out of range.
  REQUIRE(mixer.set_bus_strip(2, ducker_bus(2)));
  REQUIRE(control.set_bus_strip(2, ducker_bus(2)));
  // Rebuilding it back does not resurrect the dropped binding.
  REQUIRE(mixer.set_bus_strip(2, ducker_bus(2, 1)));
  REQUIRE(control.set_bus_strip(2, ducker_bus(2, 1)));
  mixer.settle_smoothers();
  control.settle_smoothers();
  CHECK(agree(render(control, {10, 20}, 2, 6), render(mixer, {10, 20}, 2, 6)));
}

TEST_CASE("Track mixer aligns a two-deep latent bus chain with its parallel paths",
          "[track_mixer_routing][pdc]") {
  // A (48 samples of lookahead) outputs into B (24 samples), B into the master.
  // Lane 10 is dry, lane 20 enters A, lane 30 enters B directly.
  constexpr int kTotal = 72;
  TrackMixerRuntime mixer;
  mixer.prepare(kSampleRate, kBlock);
  REQUIRE(mixer.set_buses({bus_config(1, 0.0f, 2), bus_config(2)}));
  REQUIRE(mixer.set_track_lanes({TrackLaneConfig{10}, lane_to(20, 1), lane_to(30, 2)}));
  REQUIRE(mixer.set_bus_strip(1, latent_bus(1, 1.0f)));
  REQUIRE(mixer.set_bus_strip(2, latent_bus(2, 0.5f)));
  CHECK(mixer.latency_samples_q8() == (kTotal << 8));
  const Planes out = render(mixer, {10, 20, 30}, 1, 1, impulse);
  CHECK(std::abs(out[0][kTotal] - 0.75f) < 1.0e-3f);
  double early = 0.0;
  for (int i = 0; i < kTotal; ++i) early += static_cast<double>(out[0][i]) * out[0][i];
  CHECK(early < 1.0e-8);
}

TEST_CASE("Track mixer aligns a key taken from a latent bus with the keyed bus input",
          "[track_mixer_routing][pdc]") {
  // Lanes 20 and 30 carry the same burst: 20 into the latent bus A (its output
  // floored, so only its key is heard), 30 into B, which A keys. An aligned key
  // ducks B from the burst onset; a key 48 samples late lets the onset through.
  // Lane 30's fader keeps the program itself under the ducker's threshold.
  const auto rig = [](TrackMixerRuntime& m, bool bind) {
    m.prepare(kSampleRate, kBlock);
    REQUIRE(m.set_buses({bus_config(1, sonare::constants::kFloorDb), bus_config(2)}));
    REQUIRE(m.set_track_lanes({lane_to(20, 1), lane_to(30, 2)}));
    REQUIRE(m.set_bus_strip(1, latent_bus(1, 1.0f)));
    REQUIRE(m.set_bus_strip(2, ducker_bus(2)));
    REQUIRE(m.set_lane_parameter(1, TrackMixerRuntime::kFaderDb, -24.0f));
    if (bind) REQUIRE(m.set_bus_sidechain(2, 0, SidechainSourceKind::Bus, 1));
    m.settle_smoothers();
  };
  TrackMixerRuntime keyed;
  TrackMixerRuntime unkeyed;
  rig(keyed, true);
  rig(unkeyed, false);
  CHECK(keyed.latency_samples() == 48);
  const Planes k = render(keyed, {20, 30}, 2, 8, burst);
  const Planes u = render(unkeyed, {20, 30}, 2, 8, burst);
  size_t onset = 0;
  while (onset < u[0].size() && std::abs(u[0][onset]) < 1.0e-3f) ++onset;
  REQUIRE(onset < u[0].size());
  double keyed_energy = 0.0;
  double unkeyed_energy = 0.0;
  for (size_t i = onset + 8; i < onset + 40; ++i) {
    keyed_energy += static_cast<double>(k[0][i]) * k[0][i];
    unkeyed_energy += static_cast<double>(u[0][i]) * u[0][i];
  }
  CHECK(keyed_energy < 0.25 * unkeyed_energy);
}

TEST_CASE("Track mixer keys a bus insert ahead of the source's engine-only fader",
          "[track_mixer_routing]") {
  constexpr int kBlocks = 12;
  // Bus 2 carries lane 30 through a ducker; the key is lane 20 (straight to the
  // master) or bus 1 (lane 20 inside it). Lane 20's contribution is removed
  // from the master so the keyed bus alone is compared.
  const auto run = [&](SidechainSourceKind kind, float source_fader_db, bool bind) {
    TrackMixerRuntime m;
    m.prepare(kSampleRate, kBlock);
    const bool via_bus = kind == SidechainSourceKind::Bus;
    REQUIRE(m.set_buses({bus_config(1, via_bus ? source_fader_db : 0.0f), bus_config(2)}));
    REQUIRE(m.set_track_lanes({via_bus ? lane_to(20, 1) : TrackLaneConfig{20}, lane_to(30, 2)}));
    REQUIRE(m.set_bus_strip(2, ducker_bus(2)));
    // The program sits under the ducker's threshold; only the key crosses it.
    REQUIRE(m.set_lane_parameter(1, TrackMixerRuntime::kFaderDb, -24.0f));
    if (!via_bus) {
      REQUIRE(m.set_lane_parameter(0, TrackMixerRuntime::kFaderDb, source_fader_db));
    }
    if (bind) REQUIRE(m.set_bus_sidechain(2, 0, kind, via_bus ? 1u : 20u));
    m.settle_smoothers();
    const Planes master = render(m, {20, 30}, 2, kBlocks);
    return minus(master, scaled(dry(20, kBlocks), sonare::db_to_linear(source_fader_db)));
  };
  for (SidechainSourceKind kind : {SidechainSourceKind::Track, SidechainSourceKind::Bus}) {
    const Planes unkeyed = run(kind, 0.0f, false);
    const Planes keyed = run(kind, 0.0f, true);
    const Planes keyed_quiet_source = run(kind, -12.0f, true);
    const size_t settle = static_cast<size_t>(4 * kBlock);
    REQUIRE(rms_db(unkeyed[0], settle) > -40.0);
    CHECK(rms_db(unkeyed[0], settle) - rms_db(keyed[0], settle) >= 1.0);
    // Removing the source contribution leaves float residue near 1e-7.
    Planes a = keyed;
    Planes b = keyed_quiet_source;
    float diff = 0.0f;
    for (size_t c = 0; c < 2; ++c) {
      for (size_t i = 0; i < a[c].size(); ++i) diff = std::max(diff, std::abs(a[c][i] - b[c][i]));
    }
    CHECK(diff <= 1.0e-6f);
  }
}

TEST_CASE("Track mixer caps the sidechain binding table at 32", "[track_mixer_routing]") {
  TrackMixerRuntime mixer;
  mixer.prepare(kSampleRate, kBlock);
  REQUIRE(mixer.set_buses({bus_config(1), bus_config(2)}));
  REQUIRE(mixer.set_track_lanes({TrackLaneConfig{10}, lane_to(20, 2)}));
  REQUIRE(mixer.set_bus_strip(2, ducker_bus(2)));
  for (unsigned int i = 0; i < 31; ++i) REQUIRE(mixer.set_lane_sidechain(10, i, 20));
  REQUIRE(mixer.set_bus_sidechain(2, 0, SidechainSourceKind::Bus, 1));
  CHECK_FALSE(mixer.set_lane_sidechain(10, 31, 20));
  // Replacing an existing binding's source needs no new entry.
  CHECK(mixer.set_bus_sidechain(2, 0, SidechainSourceKind::Track, 10));
  REQUIRE(mixer.set_lane_sidechain(10, 0, 0));
  CHECK(mixer.set_lane_sidechain(10, 31, 20));
}

TEST_CASE("Track mixer renders bus routing, sends and keys without allocating",
          "[track_mixer_routing][rt]") {
  TrackMixerRuntime mixer;
  mixer.prepare(kSampleRate, kBlock);
  REQUIRE(
      mixer.set_buses({bus_config(1, -3.0f, 2, {send_to(3, SendTiming::PreFader)}),
                       bus_config(2, 0.0f, 0, {}, ChannelLayout::FivePointOne), bus_config(3)}));
  REQUIRE(mixer.set_track_lanes({lane_to(10, 1), lane_to(20, 3)}));
  REQUIRE(mixer.set_bus_strip(1, latent_bus(1, 1.0f)));
  REQUIRE(mixer.set_bus_strip(3, ducker_bus(3)));
  REQUIRE(mixer.set_bus_sidechain(3, 0, SidechainSourceKind::Bus, 1));
  (void)render(mixer, {10, 20}, 2, 1);
  std::array<std::array<float, kBlock>, 2> source{};
  source[0].fill(0.1f);
  source[1].fill(-0.1f);
  std::array<std::array<float, kBlock>, 2> io{};
  float* src[] = {source[0].data(), source[1].data()};
  float* out[] = {io[0].data(), io[1].data()};
  sonare::test::AllocationGuard guard;
  REQUIRE(mixer.begin_source_mix(2, kBlock));
  bool routed = false;
  REQUIRE(mixer.mix_source_into_lane(10, src, out, 2, kBlock, routed));
  REQUIRE(mixer.mix_source_into_lane(20, src, out, 2, kBlock, routed));
  mixer.finish_source_mix(out, 2, kBlock);
  CHECK(guard.count() == 0);
}

namespace {

constexpr int kEngineFrames = kBlock * 24;

struct EngineSources {
  std::array<std::vector<float>, 4> planes;
  std::array<const float*, 2> program{};
  std::array<const float*, 2> key{};
  EngineSources() {
    for (auto& plane : planes) plane.resize(kEngineFrames);
    for (int n = 0; n < kEngineFrames; ++n) {
      // The program sits under the master ducker's threshold; the key crosses it.
      planes[0][static_cast<size_t>(n)] = 0.08f * tone(10, 0, n);
      planes[1][static_cast<size_t>(n)] = 0.08f * tone(10, 1, n);
      planes[2][static_cast<size_t>(n)] = tone(20, 0, n);
      planes[3][static_cast<size_t>(n)] = tone(20, 1, n);
    }
    program = {planes[0].data(), planes[1].data()};
    key = {planes[2].data(), planes[3].data()};
  }
};

Strip ducker_master() {
  Strip master;
  master.id = "master";
  master.inserts.push_back(
      {InsertSlot::PreFader, "dynamics.duckingProcessor",
       R"({"thresholdDb":-20,"ratio":20,"attackMs":0.05,"releaseMs":80,"rangeDb":30})"});
  return master;
}

// Track 10 (program) is direct; track 20 (key) runs through bus 1 at -30 dB,
// so the master ducker hears it only through its key.
void start_engine(sonare::engine::RealtimeEngine& engine, const EngineSources& sources) {
  engine.prepare(kSampleRate, kBlock);
  sonare::engine::ClipSchedule program{
      1, {sources.program.data(), 2, kEngineFrames}, 0.0, 0, 0, kEngineFrames, false, 1.0f, 0, 0};
  program.track_id = 10;
  sonare::engine::ClipSchedule key{
      2, {sources.key.data(), 2, kEngineFrames}, 0.0, 0, 0, kEngineFrames, false, 1.0f, 0, 0};
  key.track_id = 20;
  engine.set_clips({program, key});
  REQUIRE(engine.set_track_buses({bus_config(1, -30.0f)}));
  REQUIRE(engine.set_track_lanes({TrackLaneConfig{10}, lane_to(20, 1)}));
  REQUIRE(engine.set_master_strip(ducker_master()));
}

std::vector<float> run_engine(sonare::engine::RealtimeEngine& engine) {
  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));
  std::vector<float> left_out;
  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* io[] = {left.data(), right.data()};
  for (int block = 0; block < kEngineFrames / kBlock - 2; ++block) {
    left.fill(0.0f);
    right.fill(0.0f);
    engine.process(io, 2, kBlock);
    left_out.insert(left_out.end(), left.begin(), left.end());
  }
  return left_out;
}

}  // namespace

TEST_CASE("RealtimeEngine keys a master insert from a track or a bus", "[track_mixer_routing]") {
  const EngineSources sources;
  const size_t settle = static_cast<size_t>(4 * kBlock);
  auto unkeyed_ptr = std::make_unique<sonare::engine::RealtimeEngine>();
  sonare::engine::RealtimeEngine& unkeyed = *unkeyed_ptr;
  start_engine(unkeyed, sources);
  const std::vector<float> reference = run_engine(unkeyed);
  REQUIRE(rms_db(reference, settle) > -40.0);

  auto by_track_ptr = std::make_unique<sonare::engine::RealtimeEngine>();
  sonare::engine::RealtimeEngine& by_track = *by_track_ptr;
  start_engine(by_track, sources);
  REQUIRE(by_track.set_master_sidechain(0, SidechainSourceKind::Track, 20));
  CHECK(rms_db(reference, settle) - rms_db(run_engine(by_track), settle) >= 1.0);

  auto by_bus_ptr = std::make_unique<sonare::engine::RealtimeEngine>();
  sonare::engine::RealtimeEngine& by_bus = *by_bus_ptr;
  start_engine(by_bus, sources);
  REQUIRE(by_bus.set_master_sidechain(0, SidechainSourceKind::Bus, 1));
  const std::vector<float> bus_keyed = run_engine(by_bus);
  CHECK(rms_db(reference, settle) - rms_db(bus_keyed, settle) >= 1.0);

  // Refusals: out-of-range insert, undeclared sources, bad kind.
  auto refused_ptr = std::make_unique<sonare::engine::RealtimeEngine>();
  sonare::engine::RealtimeEngine& refused = *refused_ptr;
  start_engine(refused, sources);
  CHECK_FALSE(refused.set_master_sidechain(1, SidechainSourceKind::Bus, 1));
  CHECK_FALSE(refused.set_master_sidechain(0, SidechainSourceKind::Bus, 9));
  CHECK_FALSE(refused.set_master_sidechain(0, SidechainSourceKind::Track, 99));
  CHECK_FALSE(refused.set_master_sidechain(0, static_cast<SidechainSourceKind>(2), 1));
  CHECK_FALSE(refused.set_bus_sidechain(1, 0, SidechainSourceKind::Track, 10));  // no insert
  const std::vector<float> after_refusal = run_engine(refused);
  CHECK(agree({reference}, {after_refusal}));

  // A master rebuild keeps the binding at its index; clearing removes it.
  auto rebuilt_ptr = std::make_unique<sonare::engine::RealtimeEngine>();
  sonare::engine::RealtimeEngine& rebuilt = *rebuilt_ptr;
  start_engine(rebuilt, sources);
  REQUIRE(rebuilt.set_master_sidechain(0, SidechainSourceKind::Bus, 1));
  Strip changed = ducker_master();
  changed.fader_db = 0.0f;
  changed.inserts[0].params_json =
      R"({"thresholdDb":-20,"ratio":20,"attackMs":0.05,"releaseMs":80,"rangeDb":30.0})";
  REQUIRE(rebuilt.set_master_strip(changed));
  CHECK(agree({bus_keyed}, {run_engine(rebuilt)}));

  auto cleared_ptr = std::make_unique<sonare::engine::RealtimeEngine>();
  sonare::engine::RealtimeEngine& cleared = *cleared_ptr;
  start_engine(cleared, sources);
  REQUIRE(cleared.set_master_sidechain(0, SidechainSourceKind::Bus, 1));
  REQUIRE(cleared.set_master_sidechain(0, SidechainSourceKind::Bus, 0));
  CHECK(agree({reference}, {run_engine(cleared)}));
}

TEST_CASE("Track mixer refuses a bus list that drops a bus a lane still uses",
          "[track_mixer_routing]") {
  TrackMixerRuntime mixer;
  TrackMixerRuntime control;
  for (TrackMixerRuntime* m : {&mixer, &control}) {
    m->prepare(kSampleRate, kBlock);
    REQUIRE(m->set_buses({bus_config(1), bus_config(2, -6.0f), bus_config(3, -3.0f)}));
    TrackLaneConfig lane = lane_to(10, 1);
    lane.sends.push_back(send_to(3, SendTiming::PostFader));
    REQUIRE(m->set_track_lanes({lane}));
    m->settle_smoothers();
  }
  CHECK_FALSE(mixer.set_buses({bus_config(1), bus_config(2)}));  // lane send target
  CHECK_FALSE(mixer.set_buses({bus_config(2), bus_config(3)}));  // lane output target
  const Planes a = render(mixer, {10}, 2, 4);
  const Planes b = render(control, {10}, 2, 4);
  REQUIRE(rms_db(b[0]) > -40.0);
  CHECK(a == b);
}

TEST_CASE("Track mixer aligns a send from a latent bus with the dry path",
          "[track_mixer_routing][pdc]") {
  // A (48 samples) outputs to the master and post-sends to B; lane 10 is dry.
  constexpr int kLatency = 48;
  TrackMixerRuntime mixer;
  mixer.prepare(kSampleRate, kBlock);
  REQUIRE(mixer.set_buses(
      {bus_config(1, 0.0f, 0, {send_to(2, SendTiming::PostFader)}), bus_config(2)}));
  REQUIRE(mixer.set_track_lanes({TrackLaneConfig{10}, lane_to(20, 1)}));
  REQUIRE(mixer.set_bus_strip(1, latent_bus(1, 1.0f)));
  CHECK(mixer.latency_samples() == kLatency);
  const Planes out = render(mixer, {10, 20}, 1, 1, impulse);
  CHECK(std::abs(out[0][kLatency] - 0.75f) < 1.0e-3f);
  double early = 0.0;
  for (int i = 0; i < kLatency; ++i) early += static_cast<double>(out[0][i]) * out[0][i];
  CHECK(early < 1.0e-8);
}

TEST_CASE("Track mixer folds a 7.1 bus into a 5.1 bus by downmix", "[track_mixer_routing]") {
  constexpr int kBlocks = 4;
  const auto configure = [](TrackMixerRuntime& m, std::vector<TrackBusConfig> buses) {
    m.prepare(kSampleRate, kBlock);
    REQUIRE(m.set_buses(std::move(buses)));
    REQUIRE(m.set_track_lanes({lane_to(10, 1)}));
    Strip spec;
    spec.id = "10";
    spec.surround_pan.azimuth = -90.0f;  // the left side speaker
    REQUIRE(m.set_track_strip(10, spec));
    m.settle_smoothers();
  };
  TrackMixerRuntime direct;
  TrackMixerRuntime folded;
  configure(direct, {bus_config(1, 0.0f, 0, {}, ChannelLayout::SevenPointOne)});
  configure(folded, {bus_config(1, 0.0f, 2, {}, ChannelLayout::SevenPointOne),
                     bus_config(2, 0.0f, 0, {}, ChannelLayout::FivePointOne)});
  const Planes eight = render(direct, {10}, 8, kBlocks);
  const Planes six = render(folded, {10}, 6, kBlocks);
  Planes expected(6, std::vector<float>(eight[0].size()));
  std::array<const float*, 8> in{};
  std::array<float*, 6> out{};
  for (size_t c = 0; c < 8; ++c) in[c] = eight[c].data();
  for (size_t c = 0; c < 6; ++c) out[c] = expected[c].data();
  sonare::mixing::downmix(ChannelLayout::SevenPointOne, ChannelLayout::FivePointOne, in.data(),
                          out.data(), eight[0].size());
  REQUIRE(peak({eight[6], eight[7]}) > 0.01f);
  CHECK(agree(expected, six));
}

TEST_CASE("Track mixer renders a mono bus as a stereo one", "[track_mixer_routing]") {
  const auto run = [](ChannelLayout layout) {
    TrackMixerRuntime m;
    m.prepare(kSampleRate, kBlock);
    REQUIRE(m.set_buses({bus_config(1, -3.0f, 2, {}, layout), bus_config(2)}));
    REQUIRE(m.set_track_lanes({lane_to(10, 1)}));
    m.settle_smoothers();
    return render(m, {10}, 2, 3);
  };
  const Planes stereo = run(ChannelLayout::Stereo);
  REQUIRE(rms_db(stereo[1]) > -40.0);
  CHECK(run(ChannelLayout::Mono) == stereo);
}

TEST_CASE("Track mixer refuses pan and width on a surround bus strip", "[track_mixer_routing]") {
  TrackMixerRuntime mixer;
  mixer.prepare(kSampleRate, kBlock);
  REQUIRE(
      mixer.set_buses({bus_config(1, 0.0f, 0, {}, ChannelLayout::FivePointOne), bus_config(2)}));
  Bus wide;
  wide.id = "1";
  wide.width = 1.5f;
  CHECK_FALSE(mixer.set_bus_strip(1, wide));
  wide.id = "2";
  CHECK(mixer.set_bus_strip(2, wide));
  // A stereo bus holding a width cannot be re-declared as surround either.
  CHECK_FALSE(mixer.set_buses({bus_config(1, 0.0f, 0, {}, ChannelLayout::FivePointOne),
                               bus_config(2, 0.0f, 0, {}, ChannelLayout::FivePointOne)}));
}

TEST_CASE("RealtimeEngine master strip leaves a surround output's image alone",
          "[track_mixer_routing]") {
  const EngineSources sources;
  const auto run = [&](float width, float pan, int channels) {
    auto engine = std::make_unique<sonare::engine::RealtimeEngine>();
    engine->prepare(kSampleRate, kBlock);
    sonare::engine::ClipSchedule clip{
        1, {sources.key.data(), 2, kEngineFrames}, 0.0, 0, 0, kEngineFrames, false, 1.0f, 0, 0};
    engine->set_clips({clip});
    Strip master;
    master.id = "master";
    master.width = width;
    master.pan = pan;
    REQUIRE(engine->set_master_strip(master));
    sonare::rt::Command play{};
    play.type = sonare::rt::CommandType::kTransportPlay;
    play.sample_time = -1;
    REQUIRE(engine->push_command(play));
    Planes out(static_cast<size_t>(channels));
    std::vector<std::array<float, kBlock>> io(static_cast<size_t>(channels));
    std::vector<float*> ptrs;
    for (auto& plane : io) ptrs.push_back(plane.data());
    for (int block = 0; block < 8; ++block) {
      for (auto& plane : io) plane.fill(0.0f);
      engine->process(ptrs.data(), channels, kBlock);
      for (int c = 0; c < channels; ++c) {
        out[static_cast<size_t>(c)].insert(out[static_cast<size_t>(c)].end(),
                                           io[static_cast<size_t>(c)].begin(),
                                           io[static_cast<size_t>(c)].end());
      }
    }
    return out;
  };
  const Planes plain = run(1.0f, 0.0f, 6);
  REQUIRE(rms_db(plain[0]) > -40.0);
  CHECK(run(1.5f, 0.5f, 6) == plain);
  // Non-vacuity: the same strip moves a stereo output.
  CHECK_FALSE(run(1.5f, 0.5f, 2) == run(1.0f, 0.0f, 2));
}

TEST_CASE("RealtimeEngine keys the master from a latent bus on the master timebase",
          "[track_mixer_routing][pdc]") {
  const EngineSources sources;
  const size_t settle = static_cast<size_t>(4 * kBlock);
  const auto run = [&](bool bind) {
    auto engine = std::make_unique<sonare::engine::RealtimeEngine>();
    start_engine(*engine, sources);
    REQUIRE(engine->set_bus_strip(1, latent_bus(1, 1.0f)));
    if (bind) REQUIRE(engine->set_master_sidechain(0, SidechainSourceKind::Bus, 1));
    CHECK(engine->graph_latency_samples_q8() == (48 << 8));
    return run_engine(*engine);
  };
  const std::vector<float> unkeyed = run(false);
  REQUIRE(rms_db(unkeyed, settle) > -40.0);
  CHECK(rms_db(unkeyed, settle) - rms_db(run(true), settle) >= 1.0);
}

namespace {

constexpr int kMaxOrderBlock = 512;

// Track 20 carries the key (a burst), every other track a program tone.
float keyed_program(uint32_t track, int channel, int64_t frame) {
  return track == 20 ? burst(track, channel, frame) : tone(track, channel, frame);
}

float late_impulse(uint32_t, int, int64_t frame) { return frame == 300 ? 0.25f : 0.0f; }

float constant_by_track(uint32_t track, int, int64_t) { return 0.01f * static_cast<float>(track); }

Strip ducker_strip() {
  Strip strip;
  strip.inserts.push_back(
      {InsertSlot::PreFader, "dynamics.duckingProcessor",
       R"({"thresholdDb":-20,"ratio":20,"attackMs":0.05,"releaseMs":80,"rangeDb":30})"});
  return strip;
}

Strip latent_strip(float lookahead_ms) {
  Strip strip;
  strip.inserts.push_back(
      {InsertSlot::PreFader, "dynamics.limiter",
       R"({"thresholdDb":24,"releaseMs":50,"lookaheadMs":)" + std::to_string(lookahead_ms) + "}"});
  return strip;
}

// Records channel 0 of the audio it processes and of the key it was handed.
class TapProbe final : public sonare::rt::ProcessorBase {
 public:
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    for (int i = 0; i < num_samples; ++i) {
      input.push_back(num_channels > 0 ? channels[0][i] : 0.0f);
      key.push_back(static_cast<size_t>(i) < pending_.size() ? pending_[static_cast<size_t>(i)]
                                                             : 0.0f);
    }
    keyed_last_block = !pending_.empty();
  }
  void reset() override {}
  void set_sidechain(const float* const* channels, int num_channels, int num_samples) override {
    pending_.assign(static_cast<size_t>(num_samples), 0.0f);
    if (channels != nullptr && num_channels > 0 && channels[0] != nullptr) {
      pending_.assign(channels[0], channels[0] + num_samples);
    }
  }
  void clear_sidechain() override { pending_.clear(); }
  std::vector<float> input;
  std::vector<float> key;
  bool keyed_last_block = false;

 private:
  std::vector<float> pending_;
};

// Renders @p frames through begin_block / mix_source_into_lane / finish_block
// (or the source-mix finish) in blocks of @p block frames.
Planes render_blocks(TrackMixerRuntime& mixer, const std::vector<uint32_t>& tracks, int frames,
                     int block, Signal signal, bool source_mix = false) {
  REQUIRE(block <= kMaxOrderBlock);
  Planes out(2);
  std::array<std::array<float, kMaxOrderBlock>, 2> source{};
  std::array<std::array<float, kMaxOrderBlock>, 2> io{};
  float* io_ptrs[] = {io[0].data(), io[1].data()};
  for (int start = 0; start < frames; start += block) {
    for (auto& plane : io) plane.fill(0.0f);
    REQUIRE(mixer.begin_block(2, block));
    for (uint32_t track : tracks) {
      for (int ch = 0; ch < 2; ++ch) {
        for (int i = 0; i < block; ++i) {
          source[static_cast<size_t>(ch)][static_cast<size_t>(i)] =
              signal(track, ch, static_cast<int64_t>(start) + i);
        }
      }
      float* src[] = {source[0].data(), source[1].data()};
      bool routed = false;
      REQUIRE(mixer.mix_source_into_lane(track, src, io_ptrs, 2, block, routed));
    }
    if (source_mix) {
      mixer.finish_source_mix(io_ptrs, 2, block);
    } else {
      mixer.finish_block(io_ptrs, 2, block, start);
    }
    for (size_t c = 0; c < 2; ++c) {
      out[c].insert(out[c].end(), io[c].begin(), io[c].begin() + block);
    }
  }
  return out;
}

size_t argmax_abs(const std::vector<float>& plane) {
  size_t best = 0;
  for (size_t i = 1; i < plane.size(); ++i) {
    if (std::abs(plane[i]) > std::abs(plane[best])) best = i;
  }
  return best;
}

}  // namespace

TEST_CASE("Track mixer lane sidechain is independent of lane order and block size",
          "[track_mixer_routing][lane-sidechain-order]") {
  // Lane 30 ducks from lane 20's burst; lane 20 is muted, so the master holds
  // lane 30's output alone. Swapping the lane order or the block size must not
  // move a single sample of it.
  constexpr int kFrames = 4096;
  const auto run = [&](bool source_first, int block, bool source_mix) {
    TrackMixerRuntime m;
    m.prepare(kSampleRate, kMaxOrderBlock);
    const std::vector<uint32_t> order =
        source_first ? std::vector<uint32_t>{20, 30} : std::vector<uint32_t>{30, 20};
    REQUIRE(m.set_track_lanes({TrackLaneConfig{order[0]}, TrackLaneConfig{order[1]}}));
    REQUIRE(m.set_track_strip(30, ducker_strip()));
    REQUIRE(m.set_lane_sidechain(30, 0, 20));
    REQUIRE(m.set_lane_solo_mute(source_first ? 0 : 1, false, true));
    m.settle_smoothers();
    return render_blocks(m, order, kFrames, block, keyed_program, source_mix);
  };
  for (bool source_mix : {false, true}) {
    INFO("source_mix " << source_mix);
    const Planes reference = run(true, 128, source_mix);
    // Non-vacuity: the key moves the destination.
    TrackMixerRuntime unkeyed;
    unkeyed.prepare(kSampleRate, kMaxOrderBlock);
    REQUIRE(unkeyed.set_track_lanes({TrackLaneConfig{20}, TrackLaneConfig{30}}));
    REQUIRE(unkeyed.set_track_strip(30, ducker_strip()));
    REQUIRE(unkeyed.set_lane_solo_mute(0, false, true));
    unkeyed.settle_smoothers();
    const Planes dry_out =
        render_blocks(unkeyed, {20, 30}, kFrames, 128, keyed_program, source_mix);
    REQUIRE(rms_db(dry_out[0], 2048) - rms_db(reference[0], 2048) >= 1.0);
    for (bool source_first : {true, false}) {
      for (int block : {64, 128, 512}) {
        INFO("source_first " << source_first << " block " << block);
        const Planes got = run(source_first, block, source_mix);
        CHECK(std::equal(got.begin(), got.end(), reference.begin(), reference.end()));
      }
    }
  }
}

TEST_CASE("Track mixer lane sidechain key lands on the destination strip input sample",
          "[track_mixer_routing][lane-sidechain-order][pdc]") {
  // Both lanes carry the same impulse; the source's strip delays it by 48
  // samples. The key and the destination's own input must peak together.
  constexpr int kFrames = 1024;
  for (bool source_first : {true, false}) {
    INFO("source_first " << source_first);
    TrackMixerRuntime m;
    m.prepare(kSampleRate, kMaxOrderBlock);
    const std::vector<uint32_t> order =
        source_first ? std::vector<uint32_t>{20, 30} : std::vector<uint32_t>{30, 20};
    REQUIRE(m.set_track_lanes({TrackLaneConfig{order[0]}, TrackLaneConfig{order[1]}}));
    REQUIRE(m.set_track_strip(20, latent_strip(1.0f)));
    auto* probe = new TapProbe();
    sonare::mixing::ChannelStrip strip;
    strip.add_pre_insert(std::unique_ptr<sonare::rt::ProcessorBase>(probe));
    REQUIRE(m.bind_track_strip(30, &strip));
    REQUIRE(m.set_lane_sidechain(30, 0, 20));
    CHECK(m.latency_samples() == 48);
    m.settle_smoothers();
    const Planes out = render_blocks(m, order, kFrames, 64, late_impulse);
    REQUIRE(probe->input.size() == static_cast<size_t>(kFrames));
    CHECK(argmax_abs(probe->input) == 348);
    CHECK(argmax_abs(probe->key) == argmax_abs(probe->input));
    // Both lanes leave the mixer on the common 48-sample timebase.
    CHECK(argmax_abs(out[0]) == 348);
    // Unkeyed, the widest strip still sets the latency.
    REQUIRE(m.set_lane_sidechain(30, 0, 0));
    CHECK(m.latency_samples() == 48);
  }
}

TEST_CASE("Track mixer refuses a self-keyed or cyclic lane sidechain",
          "[track_mixer_routing][lane-sidechain-order]") {
  TrackMixerRuntime m;
  m.prepare(kSampleRate, kBlock);
  REQUIRE(m.set_track_lanes({TrackLaneConfig{10}, TrackLaneConfig{20}, TrackLaneConfig{30}}));
  std::array<TapProbe*, 3> probes{};
  std::array<sonare::mixing::ChannelStrip, 3> strips;
  for (size_t i = 0; i < 3; ++i) {
    auto probe = std::make_unique<TapProbe>();
    probes[i] = probe.get();
    strips[i].add_pre_insert(std::move(probe));
    REQUIRE(m.bind_track_strip(static_cast<uint32_t>(10 * (i + 1)), &strips[i]));
  }
  // The most recent block's key value names its source (0.01 x track id).
  const auto keys = [&] {
    (void)render_blocks(m, {10, 20, 30}, kBlock, kBlock, constant_by_track);
    std::array<float, 3> values{};
    for (size_t i = 0; i < 3; ++i) {
      values[i] = probes[i]->keyed_last_block ? probes[i]->key.back() : -1.0f;
    }
    return values;
  };

  CHECK_FALSE(m.set_lane_sidechain(10, 0, 10));
  REQUIRE(m.set_lane_sidechain(10, 0, 20));
  CHECK_FALSE(m.set_lane_sidechain(20, 0, 10));
  REQUIRE(m.set_lane_sidechain(20, 0, 30));
  CHECK_FALSE(m.set_lane_sidechain(30, 0, 10));
  // A replacement that would close a cycle keeps the binding it replaces.
  CHECK_FALSE(m.set_lane_sidechain(20, 0, 10));
  const std::array<float, 3> after = keys();
  CHECK(std::abs(after[0] - 0.2f) < 1.0e-3f);
  CHECK(std::abs(after[1] - 0.3f) < 1.0e-3f);
  CHECK(after[2] == -1.0f);

  // Cycles are judged on track ids, whether or not the lanes exist.
  REQUIRE(m.set_lane_sidechain(40, 0, 50));
  CHECK_FALSE(m.set_lane_sidechain(50, 0, 40));
  CHECK_FALSE(m.set_lane_sidechain(40, 1, 40));
}
