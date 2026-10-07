/// @file engine_tail_test.cpp
/// @brief RealtimeEngine tail and latency queries against hand-computed values.
///
/// Each expected value is composed in the test from a standalone instance of
/// the same processor (its own tail_samples() / latency), never read back from
/// the engine.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <climits>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "graph/graph.h"
#include "rt/processor_base.h"

namespace {

class TailProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit TailProcessor(int tail) : tail_(tail) {}
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  int tail_samples() const noexcept override { return tail_; }

 private:
  int tail_ = 0;
};

}  // namespace

TEST_CASE("Graph tail upper bound is the saturating serial sum of every node",
          "[engine][engine_tail][graph]") {
  sonare::graph::Graph graph;
  REQUIRE(graph.tail_samples_upper_bound() == 0);
  REQUIRE(graph.add_node("a", std::make_unique<TailProcessor>(700), 1));
  REQUIRE(graph.add_node("b", std::make_unique<TailProcessor>(50), 1));
  REQUIRE(graph.add_node("c", std::make_unique<TailProcessor>(0), 1));
  // Parallel branches still add: the bound is a sum over nodes, not a path maximum.
  REQUIRE(graph.tail_samples_upper_bound() == 750);

  REQUIRE(graph.add_node("unbounded", std::make_unique<TailProcessor>(INT_MAX), 1));
  REQUIRE(graph.tail_samples_upper_bound() == INT_MAX);
}

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_FX) && defined(SONARE_WITH_ARRANGEMENT)

#include "engine/realtime_engine.h"
#include "engine/track_mixer.h"
#include "mastering/api/insert_factory.h"
#include "midi/instrument.h"
#include "mixing/api/scene.h"

namespace {

using sonare::engine::RealtimeEngine;
using sonare::engine::TrackBusConfig;
using sonare::engine::TrackLaneConfig;
using sonare::mixing::api::Bus;
using sonare::mixing::api::Insert;
using sonare::mixing::api::InsertSlot;
using sonare::mixing::api::Strip;

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 256;
constexpr uint32_t kTrack = 10;
constexpr uint32_t kBusA = 1;
constexpr uint32_t kBusB = 2;

constexpr const char* kPlate = "effects.reverb.plate";
constexpr const char* kPlateParams = R"({"decaySec":2.0,"dryWet":0.5})";
constexpr const char* kDelay = "effects.delay.stereo";
constexpr const char* kDelayParams =
    R"({"feedback":0.5,"delayTimeLMs":37,"delayTimeRMs":53,"dryWet":0.5})";
constexpr const char* kLimiter = "dynamics.limiter";
constexpr const char* kLimiterParams = R"({"thresholdDb":-6,"lookaheadMs":2,"releaseMs":50})";

std::unique_ptr<sonare::rt::ProcessorBase> standalone(const char* name, const char* params) {
  auto processor = sonare::mastering::api::make_insert(name, params);
  REQUIRE(processor != nullptr);
  processor->prepare(kSampleRate, kBlock);
  return processor;
}

int standalone_tail(const char* name, const char* params) {
  return standalone(name, params)->tail_samples();
}

int standalone_latency_q8(const char* name, const char* params) {
  return standalone(name, params)->latency_samples_q8();
}

Insert insert_of(const char* name, const char* params) {
  return Insert{InsertSlot::PreFader, name, params};
}

Bus bus_with(uint32_t id, const char* name, const char* params) {
  Bus bus;
  bus.id = std::to_string(id);
  bus.inserts.push_back(insert_of(name, params));
  return bus;
}

Strip strip_delay(int channel_delay_samples) {
  Strip strip;
  strip.id = "s";
  strip.channel_delay_samples = channel_delay_samples;
  return strip;
}

TrackLaneConfig::Send send_to(uint32_t bus_id) {
  TrackLaneConfig::Send send;
  send.bus_id = bus_id;
  return send;
}

TrackBusConfig bus_config(uint32_t id, uint32_t output_bus_id = 0) {
  TrackBusConfig config{id, 0.0f, sonare::ChannelLayout::Stereo};
  config.output_bus_id = output_bus_id;
  return config;
}

class TailInstrument final : public sonare::midi::MidiInstrument {
 public:
  explicit TailInstrument(int tail) : tail_(tail) {}
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  void on_event(uint32_t, const sonare::midi::MidiEvent&) noexcept override {}
  int tail_samples() const noexcept override { return tail_; }

 private:
  int tail_ = 0;
};

void prepare(RealtimeEngine& engine) { engine.prepare(kSampleRate, kBlock); }

}  // namespace

TEST_CASE("Engine tail: channel delay alone", "[engine][engine_tail]") {
  RealtimeEngine engine;
  prepare(engine);
  REQUIRE(engine.tail_samples() == 0);
  REQUIRE(engine.set_track_lanes({TrackLaneConfig{kTrack}}));
  REQUIRE(engine.set_track_strip(kTrack, strip_delay(300)));
  // A held-back channel delay is owed as tail: 300.
  CHECK(engine.tail_samples() == 300);
}

TEST_CASE("Engine tail: a send to a long-reverb bus", "[engine][engine_tail]") {
  const int plate = standalone_tail(kPlate, kPlateParams);
  REQUIRE(plate > 0);

  RealtimeEngine engine;
  prepare(engine);
  REQUIRE(engine.set_track_buses({bus_config(kBusA)}));
  TrackLaneConfig lane{kTrack};
  lane.sends.push_back(send_to(kBusA));
  REQUIRE(engine.set_track_lanes({lane}));
  REQUIRE(engine.set_track_strip(kTrack, strip_delay(100)));
  REQUIRE(engine.set_bus_strip(kBusA, bus_with(kBusA, kPlate, kPlateParams)));
  // Lane strip (100) in series with the reverb bus, whose output is the master (0).
  CHECK(engine.tail_samples() == 100 + plate);
}

TEST_CASE("Engine tail: two buses in series", "[engine][engine_tail]") {
  const int delay = standalone_tail(kDelay, kDelayParams);
  const int plate = standalone_tail(kPlate, kPlateParams);
  REQUIRE(delay > 0);
  REQUIRE(plate > 0);

  RealtimeEngine engine;
  prepare(engine);
  // A routes into B, declared B-first so the order has to follow the edge.
  REQUIRE(engine.set_track_buses({bus_config(kBusB), bus_config(kBusA, kBusB)}));
  TrackLaneConfig lane{kTrack};
  lane.output_bus_id = kBusA;
  REQUIRE(engine.set_track_lanes({lane}));
  REQUIRE(engine.set_bus_strip(kBusA, bus_with(kBusA, kDelay, kDelayParams)));
  REQUIRE(engine.set_bus_strip(kBusB, bus_with(kBusB, kPlate, kPlateParams)));
  CHECK(engine.tail_samples() == delay + plate);
}

TEST_CASE("Engine tail: the longer of two parallel sends", "[engine][engine_tail]") {
  const int delay = standalone_tail(kDelay, kDelayParams);
  const int plate = standalone_tail(kPlate, kPlateParams);
  REQUIRE(delay > 0);
  REQUIRE(plate > 0);
  REQUIRE(delay != plate);

  RealtimeEngine engine;
  prepare(engine);
  REQUIRE(engine.set_track_buses({bus_config(kBusA), bus_config(kBusB)}));
  TrackLaneConfig lane{kTrack};
  lane.sends.push_back(send_to(kBusA));
  lane.sends.push_back(send_to(kBusB));
  REQUIRE(engine.set_track_lanes({lane}));
  REQUIRE(engine.set_bus_strip(kBusA, bus_with(kBusA, kDelay, kDelayParams)));
  REQUIRE(engine.set_bus_strip(kBusB, bus_with(kBusB, kPlate, kPlateParams)));
  CHECK(engine.tail_samples() == std::max(delay, plate));
}

TEST_CASE("Engine tail: instrument tail in series with the track mixer", "[engine][engine_tail]") {
  RealtimeEngine engine;
  prepare(engine);
  TailInstrument instrument(5000);
  REQUIRE(engine.set_midi_instrument(kTrack, &instrument));
  // Instrument alone: its own tail.
  CHECK(engine.tail_samples() == 5000);

  REQUIRE(engine.set_track_lanes({TrackLaneConfig{kTrack}}));
  REQUIRE(engine.set_track_strip(kTrack, strip_delay(250)));
  CHECK(engine.tail_samples() == 5000 + 250);
  engine.set_midi_instrument(kTrack, nullptr);
}

TEST_CASE("Engine tail: an unbounded instrument gives INT_MAX", "[engine][engine_tail]") {
  RealtimeEngine engine;
  prepare(engine);
  TailInstrument instrument(INT_MAX);
  REQUIRE(engine.set_midi_instrument(kTrack, &instrument));
  REQUIRE(engine.set_track_lanes({TrackLaneConfig{kTrack}}));
  REQUIRE(engine.set_track_strip(kTrack, strip_delay(250)));
  CHECK(engine.tail_samples() == INT_MAX);
  engine.set_midi_instrument(kTrack, nullptr);
}

#if defined(SONARE_WITH_GRAPH)
TEST_CASE("Engine tail: one graph node", "[engine][engine_tail]") {
  auto graph = std::make_unique<sonare::graph::Graph>();
  REQUIRE(graph->add_node("in", std::make_unique<TailProcessor>(0), 1));
  REQUIRE(graph->add_node("fx", std::make_unique<TailProcessor>(900), 1));
  REQUIRE(graph->add_node("out", std::make_unique<TailProcessor>(0), 1));
  REQUIRE(graph->connect({"in", 0, "fx", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph->connect({"fx", 0, "out", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph->compile());
  graph->prepare(kSampleRate, kBlock);

  RealtimeEngine engine;
  prepare(engine);
  REQUIRE(engine.swap_graph(std::move(graph), "in", "out", 1));
  CHECK(engine.tail_samples() == 900);
}
#endif

TEST_CASE("Engine tail: a master insert, only while mixing is enabled", "[engine][engine_tail]") {
  const int plate = standalone_tail(kPlate, kPlateParams);
  REQUIRE(plate > 0);

  RealtimeEngine engine;
  prepare(engine);
  Strip master;
  master.id = "master";
  master.inserts.push_back(insert_of(kPlate, kPlateParams));
  REQUIRE(engine.set_master_strip(master));
  REQUIRE(engine.mixing_enabled());
  CHECK(engine.tail_samples() == plate);

  engine.set_mixing_enabled(false);
  CHECK(engine.tail_samples() == 0);
}

TEST_CASE("Engine graph latency: lane, bus and master inserts add", "[engine][engine_tail]") {
  const int limiter_q8 = standalone_latency_q8(kLimiter, kLimiterParams);
  REQUIRE(limiter_q8 > 0);

  RealtimeEngine engine;
  prepare(engine);
  REQUIRE(engine.set_track_buses({bus_config(kBusA)}));
  TrackLaneConfig latent{kTrack};
  latent.output_bus_id = kBusA;
  // A second lane without inserts is only delay-compensated to the first.
  TrackLaneConfig plain{kTrack + 1};
  plain.output_bus_id = kBusA;
  REQUIRE(engine.set_track_lanes({latent, plain}));
  REQUIRE(engine.graph_latency_samples_q8() == 0);

  Strip lane_strip = strip_delay(0);
  lane_strip.inserts.push_back(insert_of(kLimiter, kLimiterParams));
  REQUIRE(engine.set_track_strip(kTrack, lane_strip));
  CHECK(engine.graph_latency_samples_q8() == limiter_q8);

  REQUIRE(engine.set_bus_strip(kBusA, bus_with(kBusA, kLimiter, kLimiterParams)));
  CHECK(engine.graph_latency_samples_q8() == 2 * limiter_q8);

  Strip master;
  master.id = "master";
  master.inserts.push_back(insert_of(kLimiter, kLimiterParams));
  REQUIRE(engine.set_master_strip(master));
  CHECK(engine.graph_latency_samples_q8() == 3 * limiter_q8);
}

#endif
