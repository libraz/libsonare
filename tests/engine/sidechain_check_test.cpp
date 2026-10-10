/// @file sidechain_check_test.cpp
/// @brief Sidechain binding queries agree with the setters and change nothing.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/realtime_engine.h"
#include "engine/track_mixer.h"
#include "mixing/alignment_delay.h"
#include "mixing/api/scene.h"
#include "mixing/channel_strip.h"
#include "rt/command.h"

namespace {

using sonare::engine::RealtimeEngine;
using sonare::engine::SidechainRefusal;
using sonare::engine::SidechainSourceKind;
using sonare::engine::TrackBusConfig;
using sonare::engine::TrackLaneConfig;
using sonare::mixing::api::Bus;
using sonare::mixing::api::InsertSlot;
using sonare::mixing::api::Strip;

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 256;
constexpr int kFrames = kBlock * 16;
constexpr int kMaxDelay = sonare::mixing::kMaxAlignmentDelaySamples;
constexpr auto kTrack = SidechainSourceKind::Track;
constexpr auto kBus = SidechainSourceKind::Bus;
const auto kBadKind = static_cast<SidechainSourceKind>(7);
constexpr unsigned int kTableCapacity =
    static_cast<unsigned int>(sonare::engine::TrackMixerRuntime::kMaxSidechainBindings);

class ReportedLatencyProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit ReportedLatencyProcessor(int samples) : samples_(samples) {}
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  int latency_samples() const noexcept override { return samples_; }

 private:
  int samples_;
};

const char* const kDucker =
    R"({"thresholdDb":-20,"ratio":20,"attackMs":0.05,"releaseMs":80,"rangeDb":30})";
// A limiter far above the signal with 1 ms lookahead: a pure 48-sample delay.
const char* const kLatentLimiter = R"({"thresholdDb":24,"releaseMs":50,"lookaheadMs":1})";

Bus ducker_bus(uint32_t id, bool latent_prefix) {
  Bus bus;
  bus.id = std::to_string(id);
  if (latent_prefix)
    bus.inserts.push_back({InsertSlot::PreFader, "dynamics.limiter", kLatentLimiter});
  bus.inserts.push_back({InsertSlot::PreFader, "dynamics.duckingProcessor", kDucker});
  return bus;
}

Bus latent_bus(uint32_t id) {
  Bus bus;
  bus.id = std::to_string(id);
  bus.inserts.push_back({InsertSlot::PreFader, "dynamics.limiter", kLatentLimiter});
  return bus;
}

Strip ducker_master(bool latent_prefix) {
  Strip master;
  master.id = "master";
  if (latent_prefix) {
    master.inserts.push_back({InsertSlot::PreFader, "dynamics.limiter", kLatentLimiter});
  }
  master.inserts.push_back({InsertSlot::PreFader, "dynamics.duckingProcessor", kDucker});
  return master;
}

TrackBusConfig bus_config(uint32_t id, uint32_t output = 0) {
  TrackBusConfig config;
  config.bus_id = id;
  config.output_bus_id = output;
  return config;
}

TrackLaneConfig lane_to(uint32_t track, uint32_t output_bus) {
  TrackLaneConfig lane{track};
  lane.output_bus_id = output_bus;
  return lane;
}

struct Sources {
  std::array<std::vector<float>, 6> planes;
  std::array<std::array<const float*, 2>, 3> tracks{};
  Sources() {
    for (size_t p = 0; p < planes.size(); ++p) {
      planes[p].resize(kFrames);
      const float step = 0.011f + 0.007f * static_cast<float>(p);
      for (int n = 0; n < kFrames; ++n) {
        planes[p][static_cast<size_t>(n)] = 0.4f * std::sin(step * static_cast<float>(n));
      }
    }
    for (size_t t = 0; t < tracks.size(); ++t) {
      tracks[t] = {planes[2 * t].data(), planes[2 * t + 1].data()};
    }
  }
};

enum class Target { kLane, kBus, kMaster };
enum class Existing { kNone, kReplace, kUnbind };

struct Row {
  int number;
  Target target;
  Existing existing;
  const char* condition;
  uint32_t target_id;  // track id or bus id; unused for the master
  unsigned int insert_index;
  SidechainSourceKind kind;
  uint32_t source_id;
  SidechainRefusal expected;
};

bool ceiling(const Row& row) { return std::string(row.condition) == "alignment_ceiling"; }
bool table_full(const Row& row) { return std::string(row.condition) == "table_full"; }
// Row 2 needs a lane edge 30 -> 20 for a replace of (30, 0) to close a cycle.
bool cycle_edge(const Row& row) {
  return row.target == Target::kLane && std::string(row.condition) == "cycle";
}

// One engine plus the strips it borrows.
//
// Default rig: lanes 10 and 30 direct, 20 -> bus 1 -> bus 2 -> master; buses 1
// and 2 carry a ducker on insert 0, bus 3 has no insert; the master a ducker.
// Ceiling rig: lane 20's strip reports kMaxDelay - 64 samples, lane 30's 128
// ahead of insert 1; buses 1 and 2 (prefixed ducker) each add 48 samples and go
// straight to the master; the master ducker sits behind a 48-sample prefix. The
// rig itself plans within the ceiling, and each ceiling query pushes one path
// past it.
struct Rig {
  std::unique_ptr<RealtimeEngine> engine = std::make_unique<RealtimeEngine>();
  sonare::mixing::ChannelStrip huge;
  sonare::mixing::ChannelStrip latent_target;
  // Default rig: a strip per lane, with as many inserts as the table has slots.
  std::array<sonare::mixing::ChannelStrip, 3> lane_strips;

  Rig(const Row& row, const Sources& sources) {
    RealtimeEngine& e = *engine;
    e.prepare(kSampleRate, kBlock);
    std::vector<sonare::engine::ClipSchedule> clips;
    const std::array<uint32_t, 3> track_ids{10, 20, 30};
    for (size_t t = 0; t < track_ids.size(); ++t) {
      sonare::engine::ClipSchedule clip{static_cast<uint32_t>(t + 1),
                                        {sources.tracks[t].data(), 2, kFrames},
                                        0.0,
                                        0,
                                        0,
                                        kFrames,
                                        false,
                                        1.0f,
                                        0,
                                        0};
      clip.track_id = track_ids[t];
      clips.push_back(clip);
    }
    e.set_clips(clips);
    const bool high = ceiling(row);
    if (high) {
      REQUIRE(e.set_track_buses({bus_config(1), bus_config(2)}));
      REQUIRE(e.set_track_lanes({TrackLaneConfig{10}, TrackLaneConfig{20}, TrackLaneConfig{30}}));
      huge.add_pre_insert(std::make_unique<ReportedLatencyProcessor>(kMaxDelay - 64));
      latent_target.add_pre_insert(std::make_unique<ReportedLatencyProcessor>(128));
      latent_target.add_pre_insert(std::make_unique<ReportedLatencyProcessor>(0));
      REQUIRE(e.bind_track_strip(20, &huge));
      REQUIRE(e.bind_track_strip(30, &latent_target));
      REQUIRE(e.set_bus_strip(1, latent_bus(1)));
      REQUIRE(e.set_bus_strip(2, ducker_bus(2, true)));
      REQUIRE(e.set_master_strip(ducker_master(true)));
    } else {
      REQUIRE(e.set_track_buses({bus_config(1, 2), bus_config(2), bus_config(3)}));
      REQUIRE(e.set_track_lanes({TrackLaneConfig{10}, lane_to(20, 1), TrackLaneConfig{30}}));
      for (size_t t = 0; t < track_ids.size(); ++t) {
        for (unsigned int i = 0; i < kTableCapacity; ++i) {
          lane_strips[t].add_pre_insert(std::make_unique<ReportedLatencyProcessor>(0));
        }
        REQUIRE(e.bind_track_strip(track_ids[t], &lane_strips[t]));
      }
      REQUIRE(e.set_bus_strip(1, ducker_bus(1, false)));
      REQUIRE(e.set_bus_strip(2, ducker_bus(2, false)));
      REQUIRE(e.set_master_strip(ducker_master(false)));
    }
    if (row.existing != Existing::kNone) {
      switch (row.target) {
        case Target::kLane:
          REQUIRE(e.set_lane_sidechain(30, high ? 1 : 0, 10));
          break;
        case Target::kBus:
          REQUIRE(e.set_bus_sidechain(2, 0, kTrack, 10));
          break;
        case Target::kMaster:
          REQUIRE(e.set_master_sidechain(0, kTrack, 10));
          break;
      }
    }
    if (cycle_edge(row)) REQUIRE(e.set_lane_sidechain(20, 0, 30));
    if (table_full(row)) {
      for (unsigned int i = 0; i < kTableCapacity; ++i) {
        REQUIRE(e.set_lane_sidechain(10, i, 20));
      }
    }
    sonare::rt::Command play{};
    play.type = sonare::rt::CommandType::kTransportPlay;
    play.sample_time = -1;
    REQUIRE(e.push_command(play));
  }

  SidechainRefusal query(const Row& row) const {
    switch (row.target) {
      case Target::kLane:
        return engine->can_set_lane_sidechain(row.target_id, row.insert_index, row.source_id);
      case Target::kBus:
        return engine->can_set_bus_sidechain(row.target_id, row.insert_index, row.kind,
                                             row.source_id);
      case Target::kMaster:
        return engine->can_set_master_sidechain(row.insert_index, row.kind, row.source_id);
    }
    return SidechainRefusal::kNone;
  }

  bool set(const Row& row) {
    switch (row.target) {
      case Target::kLane:
        return engine->set_lane_sidechain(row.target_id, row.insert_index, row.source_id);
      case Target::kBus:
        return engine->set_bus_sidechain(row.target_id, row.insert_index, row.kind, row.source_id);
      case Target::kMaster:
        return engine->set_master_sidechain(row.insert_index, row.kind, row.source_id);
    }
    return false;
  }

  // Reported latency, PDC storage, and verdicts that read the binding table
  // (lane cycles over 10/20/30, a bus key from bus 1, a master key).
  std::vector<int> fingerprint() {
    sonare::engine::TrackMixerRuntime& mixer = engine->track_mixer();
    const uint64_t generation = mixer.pdc_storage_generation();
    std::vector<int> out{engine->graph_latency_samples_q8(), mixer.latency_samples_q8(),
                         static_cast<int>(generation & 0x7fffffffu),
                         static_cast<int>(generation >> 31)};
    for (uint32_t target : {10u, 20u, 30u}) {
      for (uint32_t source : {10u, 20u, 30u}) {
        out.push_back(static_cast<int>(engine->can_set_lane_sidechain(target, 0, source)));
      }
    }
    out.push_back(static_cast<int>(engine->can_set_bus_sidechain(2, 0, kBus, 1)));
    out.push_back(static_cast<int>(engine->can_set_master_sidechain(0, kTrack, 30)));
    return out;
  }

  // Interleaved stereo output of @p blocks blocks: hears every key binding
  // through the duckers.
  std::vector<float> render(int blocks) {
    std::vector<float> out;
    std::array<float, kBlock> left{};
    std::array<float, kBlock> right{};
    float* io[] = {left.data(), right.data()};
    for (int block = 0; block < blocks; ++block) {
      left.fill(0.0f);
      right.fill(0.0f);
      engine->process(io, 2, kBlock);
      out.insert(out.end(), left.begin(), left.end());
      out.insert(out.end(), right.begin(), right.end());
    }
    return out;
  }
};

// The seed-182 pairwise rows over (target, existing, condition). Each verdict
// follows the setter's checks in order; an unbind of a missing binding is kNone.
const std::array<Row, 27> kRows{{
    {1, Target::kBus, Existing::kUnbind, "invalid_target", 9, 0, kTrack, 0,
     SidechainRefusal::kInvalidTarget},
    {2, Target::kLane, Existing::kReplace, "cycle", 30, 0, kTrack, 20, SidechainRefusal::kCycle},
    {3, Target::kMaster, Existing::kNone, "invalid_source_kind", 0, 0, kBadKind, 20,
     SidechainRefusal::kInvalidSourceKind},
    {4, Target::kBus, Existing::kReplace, "insert_out_of_range", 2, 5, kTrack, 20,
     SidechainRefusal::kInsertOutOfRange},
    {5, Target::kBus, Existing::kReplace, "invalid_source_kind", 2, 0, kBadKind, 20,
     SidechainRefusal::kInvalidSourceKind},
    {6, Target::kBus, Existing::kNone, "table_full", 2, 0, kTrack, 10,
     SidechainRefusal::kTableFull},
    {7, Target::kMaster, Existing::kReplace, "undeclared_source", 0, 0, kTrack, 99,
     SidechainRefusal::kUndeclaredSource},
    {8, Target::kBus, Existing::kNone, "cycle", 1, 0, kBus, 2, SidechainRefusal::kCycle},
    {9, Target::kMaster, Existing::kUnbind, "ok", 0, 0, kTrack, 0, SidechainRefusal::kNone},
    {10, Target::kBus, Existing::kUnbind, "ok", 2, 0, kTrack, 0, SidechainRefusal::kNone},
    {11, Target::kMaster, Existing::kUnbind, "insert_out_of_range", 0, 3, kTrack, 0,
     SidechainRefusal::kInsertOutOfRange},
    {12, Target::kLane, Existing::kNone, "ok", 30, 0, kTrack, 20, SidechainRefusal::kNone},
    {13, Target::kLane, Existing::kUnbind, "invalid_target", 0, 0, kTrack, 0,
     SidechainRefusal::kInvalidTarget},
    {14, Target::kLane, Existing::kReplace, "alignment_ceiling", 30, 1, kTrack, 20,
     SidechainRefusal::kPlanRefused},
    {15, Target::kLane, Existing::kReplace, "self_key", 30, 0, kTrack, 30,
     SidechainRefusal::kSelfKey},
    {16, Target::kBus, Existing::kNone, "undeclared_source", 2, 0, kTrack, 99,
     SidechainRefusal::kUndeclaredSource},
    {17, Target::kMaster, Existing::kReplace, "ok", 0, 0, kBus, 1, SidechainRefusal::kNone},
    {18, Target::kMaster, Existing::kNone, "insert_out_of_range", 0, 3, kTrack, 20,
     SidechainRefusal::kInsertOutOfRange},
    {19, Target::kBus, Existing::kNone, "alignment_ceiling", 2, 1, kBus, 1,
     SidechainRefusal::kPlanRefused},
    {20, Target::kLane, Existing::kNone, "table_full", 30, 0, kTrack, 20,
     SidechainRefusal::kTableFull},
    {21, Target::kBus, Existing::kNone, "self_key", 2, 0, kBus, 2, SidechainRefusal::kSelfKey},
    {22, Target::kBus, Existing::kNone, "invalid_target", 9, 0, kTrack, 20,
     SidechainRefusal::kInvalidTarget},
    {23, Target::kBus, Existing::kReplace, "invalid_target", 9, 0, kTrack, 20,
     SidechainRefusal::kInvalidTarget},
    {24, Target::kMaster, Existing::kNone, "alignment_ceiling", 0, 1, kTrack, 10,
     SidechainRefusal::kPlanRefused},
    {25, Target::kMaster, Existing::kNone, "table_full", 0, 0, kTrack, 20,
     SidechainRefusal::kTableFull},
    {26, Target::kLane, Existing::kNone, "undeclared_source", 30, 0, kTrack, 99,
     SidechainRefusal::kUndeclaredSource},
    {27, Target::kLane, Existing::kReplace, "insert_out_of_range", 30, kTableCapacity, kTrack, 20,
     SidechainRefusal::kInsertOutOfRange},
}};

}  // namespace

TEST_CASE("RealtimeEngine sidechain queries match the setters and change nothing",
          "[engine][sidechain_check]") {
  const Sources sources;
  for (const Row& row : kRows) {
    INFO("row " << row.number << " (" << row.condition << ")");
    Rig subject(row, sources);
    Rig control(row, sources);
    const std::vector<int> before = subject.fingerprint();
    REQUIRE(before == control.fingerprint());

    const SidechainRefusal verdict = subject.query(row);
    CHECK(verdict == row.expected);
    CHECK(subject.fingerprint() == before);
    CHECK(subject.render(4) == control.render(4));

    const bool accepted = subject.set(row);
    CHECK(accepted == (verdict == SidechainRefusal::kNone));
    if (!accepted) {
      CHECK(subject.fingerprint() == control.fingerprint());
      CHECK(subject.render(4) == control.render(4));
    }
  }
}

TEST_CASE("RealtimeEngine sidechain table rows exercise every refusal reason",
          "[engine][sidechain_check]") {
  std::array<bool, 9> seen{};
  for (const Row& row : kRows) seen[static_cast<size_t>(row.expected)] = true;
  for (size_t reason = 0; reason < seen.size(); ++reason) {
    INFO("reason " << reason);
    CHECK(seen[reason]);
  }
}
