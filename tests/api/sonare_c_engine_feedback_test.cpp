/// @file sonare_c_engine_feedback_test.cpp
/// @brief C ABI for prime, processor reset, sidechain queries, tail and latency.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <set>
#include <string>
#include <vector>

#include "sonare_c_engine_test_helpers.h"

namespace {

constexpr int kBlock = 256;
constexpr int kFrames = kBlock * 8;
constexpr SonareError kInvalid = SONARE_ERROR_INVALID_PARAMETER;

SonareRealtimeEngine* make_prepared(size_t command_capacity = 64) {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, kBlock, command_capacity, 64) == SONARE_OK);
  return engine;
}

const std::vector<float>& dc_source() {
  static const std::vector<float> source(kFrames, 1.0f);
  return source;
}

// One DC clip per track, prepared mono.
SonareRealtimeEngine* make_dc_engine(std::initializer_list<uint32_t> track_ids) {
  SonareRealtimeEngine* engine = make_prepared();
  static const float* channels[] = {dc_source().data()};
  std::vector<SonareEngineClip> clips;
  uint32_t clip_id = 1;
  for (uint32_t track_id : track_ids) {
    SonareEngineClip clip{};
    clip.id = clip_id++;
    clip.track_id = track_id;
    clip.channels = channels;
    clip.num_channels = 1;
    clip.num_samples = kFrames;
    clip.length_samples = kFrames;
    clip.gain = 1.0f;
    clips.push_back(clip);
  }
  REQUIRE(sonare_engine_set_clips(engine, clips.data(), clips.size()) == SONARE_OK);
  return engine;
}

}  // namespace

TEST_CASE("sonare_engine_reset_processor_state validates and queues", "[c_api][engine][feedback]") {
  CHECK(sonare_engine_reset_processor_state(nullptr, -1) == kInvalid);

  SonareRealtimeEngine* engine = make_prepared();
  CHECK(sonare_engine_reset_processor_state(engine, -1) == SONARE_OK);
  CHECK(sonare_engine_reset_processor_state(engine, 0) == SONARE_OK);
  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_reset_processor_state reports a full command queue",
          "[c_api][engine][feedback]") {
  // A tiny queue is reachable cheaply: nothing drains it between calls.
  SonareRealtimeEngine* engine = make_prepared(4);
  SonareError last = SONARE_OK;
  for (int i = 0; i < 4096 && last == SONARE_OK; ++i) {
    last = sonare_engine_reset_processor_state(engine, -1);
  }
  CHECK(last == SONARE_ERROR_OUT_OF_MEMORY);
  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_prime_offline_parameters validates its arguments",
          "[c_api][engine][feedback]") {
  CHECK(sonare_engine_prime_offline_parameters(nullptr, 1, kBlock) == kInvalid);

  SonareRealtimeEngine* fresh = nullptr;
  REQUIRE(sonare_engine_create(&fresh) == SONARE_OK);
  CHECK(sonare_engine_prime_offline_parameters(fresh, 1, kBlock) == SONARE_ERROR_INVALID_STATE);
  sonare_engine_destroy(fresh);

  SonareRealtimeEngine* engine = make_prepared();
  CHECK(sonare_engine_prime_offline_parameters(engine, 0, kBlock) == kInvalid);
  CHECK(sonare_engine_prime_offline_parameters(engine, -1, kBlock) == kInvalid);
  CHECK(sonare_engine_prime_offline_parameters(engine, 1, 0) == kInvalid);
  CHECK(sonare_engine_prime_offline_parameters(engine, 1, -4) == kInvalid);
  CHECK(sonare_engine_prime_offline_parameters(engine, 99, kBlock) == kInvalid);
  CHECK(sonare_engine_prime_offline_parameters(engine, 1, kBlock) == SONARE_OK);
  CHECK(sonare_engine_prime_offline_parameters(engine, 2, kBlock) == SONARE_OK);
  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine tail and graph latency queries", "[c_api][engine][feedback]") {
  int value = -1;
  CHECK(sonare_engine_tail_samples(nullptr, &value) == kInvalid);
  CHECK(sonare_engine_graph_latency_samples_q8(nullptr, &value) == kInvalid);

  SonareRealtimeEngine* engine = make_prepared();
  CHECK(sonare_engine_tail_samples(engine, nullptr) == kInvalid);
  CHECK(sonare_engine_graph_latency_samples_q8(engine, nullptr) == kInvalid);

  CHECK(sonare_engine_tail_samples(engine, &value) == SONARE_OK);
  CHECK(value == engine->engine.tail_samples());

  engine->engine.set_graph_latency_samples_q8(1234);
  value = -1;
  CHECK(sonare_engine_graph_latency_samples_q8(engine, &value) == SONARE_OK);
  CHECK(value == 1234);
  CHECK(value == engine->engine.graph_latency_samples_q8());
  sonare_engine_destroy(engine);
}

#if defined(SONARE_WITH_MIXING)
namespace {

constexpr const char* kLimiterBusJson =
    R"({"version":1,"strips":[],"buses":[{"id":"1","inserts":[{"slot":"pre","processor":"dynamics.limiter","params":"{\"thresholdDb\":24,\"lookaheadMs\":0,\"releaseMs\":50}"}]}],"connections":[]})";
constexpr const char* kDuckerBusJson =
    R"({"version":1,"strips":[],"buses":[{"id":"1","inserts":[{"slot":"pre","processor":"dynamics.limiter","params":"{\"thresholdDb\":24,\"lookaheadMs\":0,\"releaseMs\":50}"}]},{"id":"2","inserts":[{"slot":"pre","processor":"dynamics.duckingProcessor","params":"{\"thresholdDb\":-20,\"ratio\":20,\"attackMs\":0.05,\"releaseMs\":80,\"rangeDb\":30}"}]}],"connections":[]})";
constexpr const char* kDuckerMasterJson =
    R"({"version":1,"strips":[{"id":"master","inserts":[{"slot":"pre","processor":"dynamics.duckingProcessor","params":"{\"thresholdDb\":-20,\"ratio\":20,\"attackMs\":0.05,\"releaseMs\":80,\"rangeDb\":30}"}]}],"buses":[],"connections":[]})";
constexpr const char* kDuckerTrackJson =
    R"({"version":1,"strips":[{"id":"track-10","inserts":[{"slot":"pre","processor":"dynamics.duckingProcessor","params":{"thresholdDb":-20,"ratio":20,"attackMs":0.05,"releaseMs":80,"rangeDb":30}}]}],"buses":[],"connections":[]})";
constexpr const char* kDuckerTrack30Json =
    R"({"version":1,"strips":[{"id":"track-30","inserts":[{"slot":"pre","processor":"dynamics.duckingProcessor","params":{"thresholdDb":-20,"ratio":20,"attackMs":0.05,"releaseMs":80,"rangeDb":30}}]}],"buses":[],"connections":[]})";

// Tracks 10 and 30 into buses 2 and 1; bus 1 has a limiter, bus 2 a ducker,
// the master a ducker, and tracks 10 and 30 a ducker insert each.
SonareRealtimeEngine* make_keyed_rig() {
  SonareRealtimeEngine* engine = make_dc_engine({10, 30});
  SonareEngineBus buses[] = {{1, 0.0f, SONARE_CHANNEL_LAYOUT_STEREO, 0, nullptr, 0},
                             {2, 0.0f, SONARE_CHANNEL_LAYOUT_STEREO, 0, nullptr, 0}};
  REQUIRE(sonare_engine_set_track_buses(engine, buses, 2) == SONARE_OK);
  SonareEngineTrackLane lanes[] = {{10, nullptr, 0, 2, SONARE_CHANNEL_LAYOUT_STEREO},
                                   {30, nullptr, 0, 1, SONARE_CHANNEL_LAYOUT_STEREO}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes, 2) == SONARE_OK);
  REQUIRE(sonare_engine_set_bus_strip_json(engine, 1, kLimiterBusJson) == SONARE_OK);
  REQUIRE(sonare_engine_set_bus_strip_json(engine, 2, kDuckerBusJson) == SONARE_OK);
  REQUIRE(sonare_engine_set_master_strip_json(engine, kDuckerMasterJson) == SONARE_OK);
  REQUIRE(sonare_engine_set_track_strip_json(engine, 10, kDuckerTrackJson) == SONARE_OK);
  REQUIRE(sonare_engine_set_track_strip_json(engine, 30, kDuckerTrack30Json) == SONARE_OK);
  return engine;
}

}  // namespace

TEST_CASE("sonare_engine can_set_*_sidechain validate pointers", "[c_api][engine][feedback]") {
  SonareRealtimeEngine* engine = make_keyed_rig();
  int refusal = -1;
  CHECK(sonare_engine_can_set_lane_sidechain(nullptr, 10, 0, 30, &refusal) == kInvalid);
  CHECK(sonare_engine_can_set_lane_sidechain(engine, 10, 0, 30, nullptr) == kInvalid);
  CHECK(sonare_engine_can_set_bus_sidechain(nullptr, 2, 0, SONARE_SIDECHAIN_SOURCE_BUS, 1,
                                            &refusal) == kInvalid);
  CHECK(sonare_engine_can_set_bus_sidechain(engine, 2, 0, SONARE_SIDECHAIN_SOURCE_BUS, 1,
                                            nullptr) == kInvalid);
  CHECK(sonare_engine_can_set_master_sidechain(nullptr, 0, SONARE_SIDECHAIN_SOURCE_TRACK, 30,
                                               &refusal) == kInvalid);
  CHECK(sonare_engine_can_set_master_sidechain(engine, 0, SONARE_SIDECHAIN_SOURCE_TRACK, 30,
                                               nullptr) == kInvalid);
  CHECK(refusal == -1);
  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine can_set_*_sidechain agree with the setters and cover the refusals",
          "[c_api][engine][feedback]") {
  SonareRealtimeEngine* engine = make_keyed_rig();
  std::set<int> seen;

  const auto lane = [&](uint32_t track, unsigned int insert, uint32_t source) {
    int refusal = -1;
    REQUIRE(sonare_engine_can_set_lane_sidechain(engine, track, insert, source, &refusal) ==
            SONARE_OK);
    seen.insert(refusal);
    return refusal;
  };
  const auto bus = [&](uint32_t id, unsigned int insert, int kind, uint32_t source) {
    int refusal = -1;
    REQUIRE(sonare_engine_can_set_bus_sidechain(engine, id, insert, kind, source, &refusal) ==
            SONARE_OK);
    seen.insert(refusal);
    return refusal;
  };
  const auto master = [&](unsigned int insert, int kind, uint32_t source) {
    int refusal = -1;
    REQUIRE(sonare_engine_can_set_master_sidechain(engine, insert, kind, source, &refusal) ==
            SONARE_OK);
    seen.insert(refusal);
    return refusal;
  };
  const auto ok = [](SonareError e) { return e == SONARE_OK; };

  // Lane keys.
  CHECK(lane(0, 0, 30) == SONARE_SIDECHAIN_REFUSAL_INVALID_TARGET);
  CHECK(lane(10, 0, 99) == SONARE_SIDECHAIN_REFUSAL_UNDECLARED_SOURCE);
  CHECK(lane(10, 1, 30) == SONARE_SIDECHAIN_REFUSAL_INSERT_OUT_OF_RANGE);
  CHECK(lane(10, 0, 10) == SONARE_SIDECHAIN_REFUSAL_SELF_KEY);
  CHECK(lane(10, 0, 30) == SONARE_SIDECHAIN_REFUSAL_NONE);
  CHECK(ok(sonare_engine_set_lane_sidechain(engine, 0, 0, 30)) == false);
  CHECK(ok(sonare_engine_set_lane_sidechain(engine, 10, 0, 99)) == false);
  CHECK(ok(sonare_engine_set_lane_sidechain(engine, 10, 1, 30)) == false);
  CHECK(ok(sonare_engine_set_lane_sidechain(engine, 10, 0, 10)) == false);
  CHECK(ok(sonare_engine_set_lane_sidechain(engine, 10, 0, 30)));
  // Track 10 is now keyed from track 30, so the reverse key closes a cycle.
  CHECK(lane(30, 0, 10) == SONARE_SIDECHAIN_REFUSAL_CYCLE);
  CHECK(ok(sonare_engine_set_lane_sidechain(engine, 30, 0, 10)) == false);

  // Bus keys.
  CHECK(bus(9, 0, SONARE_SIDECHAIN_SOURCE_TRACK, 10) == SONARE_SIDECHAIN_REFUSAL_INVALID_TARGET);
  CHECK(bus(1, 1, SONARE_SIDECHAIN_SOURCE_TRACK, 30) ==
        SONARE_SIDECHAIN_REFUSAL_INSERT_OUT_OF_RANGE);
  CHECK(bus(1, 0, SONARE_SIDECHAIN_SOURCE_TRACK, 99) == SONARE_SIDECHAIN_REFUSAL_UNDECLARED_SOURCE);
  CHECK(bus(1, 0, 2, 30) == SONARE_SIDECHAIN_REFUSAL_INVALID_SOURCE_KIND);
  CHECK(bus(1, 0, -1, 30) == SONARE_SIDECHAIN_REFUSAL_INVALID_SOURCE_KIND);
  CHECK(bus(1, 0, 256, 30) == SONARE_SIDECHAIN_REFUSAL_INVALID_SOURCE_KIND);
  CHECK(bus(1, 0, SONARE_SIDECHAIN_SOURCE_BUS, 1) == SONARE_SIDECHAIN_REFUSAL_SELF_KEY);
  CHECK(bus(2, 0, SONARE_SIDECHAIN_SOURCE_BUS, 1) == SONARE_SIDECHAIN_REFUSAL_NONE);
  CHECK(ok(sonare_engine_set_bus_sidechain(engine, 1, 0, 2, 30)) == false);
  CHECK(ok(sonare_engine_set_bus_sidechain(engine, 1, 0, SONARE_SIDECHAIN_SOURCE_BUS, 1)) == false);
  CHECK(ok(sonare_engine_set_bus_sidechain(engine, 2, 0, SONARE_SIDECHAIN_SOURCE_BUS, 1)));
  // Bus 2 is now keyed from bus 1, so the reverse key would close a cycle.
  CHECK(bus(1, 0, SONARE_SIDECHAIN_SOURCE_BUS, 2) == SONARE_SIDECHAIN_REFUSAL_CYCLE);
  CHECK(ok(sonare_engine_set_bus_sidechain(engine, 1, 0, SONARE_SIDECHAIN_SOURCE_BUS, 2)) == false);

  // Master keys.
  CHECK(master(1, SONARE_SIDECHAIN_SOURCE_TRACK, 30) ==
        SONARE_SIDECHAIN_REFUSAL_INSERT_OUT_OF_RANGE);
  CHECK(master(0, SONARE_SIDECHAIN_SOURCE_TRACK, 99) == SONARE_SIDECHAIN_REFUSAL_UNDECLARED_SOURCE);
  CHECK(master(0, 2, 30) == SONARE_SIDECHAIN_REFUSAL_INVALID_SOURCE_KIND);
  CHECK(master(0, -1, 30) == SONARE_SIDECHAIN_REFUSAL_INVALID_SOURCE_KIND);
  CHECK(master(0, SONARE_SIDECHAIN_SOURCE_BUS, 1) == SONARE_SIDECHAIN_REFUSAL_NONE);
  CHECK(ok(sonare_engine_set_master_sidechain(engine, 1, SONARE_SIDECHAIN_SOURCE_TRACK, 30)) ==
        false);
  CHECK(ok(sonare_engine_set_master_sidechain(engine, 0, 2, 30)) == false);
  CHECK(ok(sonare_engine_set_master_sidechain(engine, 0, SONARE_SIDECHAIN_SOURCE_BUS, 1)));

  for (int value :
       {SONARE_SIDECHAIN_REFUSAL_NONE, SONARE_SIDECHAIN_REFUSAL_INVALID_TARGET,
        SONARE_SIDECHAIN_REFUSAL_INSERT_OUT_OF_RANGE, SONARE_SIDECHAIN_REFUSAL_UNDECLARED_SOURCE,
        SONARE_SIDECHAIN_REFUSAL_INVALID_SOURCE_KIND, SONARE_SIDECHAIN_REFUSAL_SELF_KEY,
        SONARE_SIDECHAIN_REFUSAL_CYCLE}) {
    INFO("refusal " << value);
    CHECK(seen.count(value) == 1);
  }
  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine tail and latency match the core for a configured engine",
          "[c_api][engine][feedback]") {
  SonareRealtimeEngine* engine = make_keyed_rig();
  engine->engine.set_graph_latency_samples_q8(512);
  int tail = -1;
  int latency = -1;
  REQUIRE(sonare_engine_tail_samples(engine, &tail) == SONARE_OK);
  REQUIRE(sonare_engine_graph_latency_samples_q8(engine, &latency) == SONARE_OK);
  CHECK(tail == engine->engine.tail_samples());
  CHECK(latency == engine->engine.graph_latency_samples_q8());
  CHECK(latency == 512);
  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine lane transient shaper reports negative gain reduction",
          "[c_api][engine][feedback]") {
  SonareRealtimeEngine* engine = make_dc_engine({10});
  SonareEngineTrackLane lane[] = {{10, nullptr, 0, 0, SONARE_CHANNEL_LAYOUT_STEREO}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);
  REQUIRE(
      sonare_engine_set_track_strip_json(
          engine, 10,
          R"({"version":1,"strips":[{"id":"track-10","inserts":[{"slot":"pre","processor":"dynamics.transientShaper","params":{"attackGainDb":-12,"sustainGainDb":0}}]}],"buses":[],"connections":[]})") ==
      SONARE_OK);

  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  std::array<float, kBlock> block{};
  float* io[] = {block.data()};
  float deepest = 0.0f;
  size_t count = 0;
  for (int b = 0; b < 8; ++b) {
    block.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
    std::array<float, SONARE_METER_MAX_INSERTS> entries{};
    REQUIRE(sonare_engine_meter_target_insert_gain_reduction(engine, 1, entries.data(),
                                                             entries.size(), &count) == SONARE_OK);
    for (size_t i = 0; i < std::min(count, entries.size()); ++i) {
      deepest = std::min(deepest, entries[i]);
    }
  }
  CHECK(count == 1);
  CHECK(deepest < -0.5f);
  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine refuses a reserved mixer id that names no strip",
          "[c_api][engine][feedback]") {
  SonareRealtimeEngine* engine = make_prepared();
  SonareEngineTrackLane lanes[] = {{5, nullptr, 0, 0, SONARE_CHANNEL_LAYOUT_STEREO},
                                   {7, nullptr, 0, 0, SONARE_CHANNEL_LAYOUT_STEREO}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes, 2) == SONARE_OK);
  const SonareAutomationPoint point{0.0, -6.0f, 0};

  // The retired positional encoding: lane byte below the master's 0xFF.
  for (uint32_t positional : {0x4D580101u, 0x4D580001u, 0x4D580100u, 0x4D58FE00u}) {
    INFO("id " << positional);
    CHECK(sonare_engine_set_parameter_smoothed(engine, positional, -6.0f, -1) == kInvalid);
    CHECK(sonare_engine_set_parameter(engine, positional, -6.0f, -1) == kInvalid);
    CHECK(sonare_engine_set_automation_lane(engine, positional, &point, 1) == kInvalid);
  }

  // The master ids and the ids issued per strip stay valid.
  CHECK(sonare_engine_set_parameter_smoothed(engine, 0x4D58FF01u, -6.0f, -1) == SONARE_OK);
  CHECK(sonare_engine_set_automation_lane(engine, 0x4D58FF01u, &point, 1) == SONARE_OK);
  uint32_t track_id = 0;
  REQUIRE(sonare_engine_resolve_track_lane_automation_id(engine, 7, "faderDb", &track_id) ==
          SONARE_OK);
  CHECK(sonare_engine_set_parameter_smoothed(engine, track_id, -6.0f, -1) == SONARE_OK);
  CHECK(sonare_engine_set_automation_lane(engine, track_id, &point, 1) == SONARE_OK);
  sonare_engine_destroy(engine);
}

#endif  // defined(SONARE_WITH_MIXING)
