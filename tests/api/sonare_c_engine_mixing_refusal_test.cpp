/// @file sonare_c_engine_mixing_refusal_test.cpp
/// @brief Texts the engine mixing and capture C entries leave in the last error when they refuse
///        a caller argument.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "util/constants.h"
#include "util/json.h"

namespace {

std::string last_message() { return std::string(sonare_last_error_message()); }

SonareEngine* make_engine() {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 16, 16, 0) == SONARE_OK);
  return engine;
}

// A whole mixer scene, which a strip setter refuses by the keys a strip does not take.
constexpr const char* kWholeScene = R"({"version":2,"strips":[],"buses":[],"connections":[]})";

}  // namespace

TEST_CASE("capture entries refuse a NULL argument by name", "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();
  SonareEngineCaptureStatus status{};
  float* planes[1] = {nullptr};
  SonareEngineCaptureBuffer buffer{planes, 1, 16};

  CHECK(sonare_engine_set_capture_buffer(nullptr, &buffer) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "engine must not be NULL");
  CHECK(sonare_engine_set_capture_buffer(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "buffer must not be NULL");
  CHECK(sonare_engine_arm_capture(nullptr, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "engine must not be NULL");
  CHECK(sonare_engine_capture_status(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");
  CHECK(sonare_engine_capture_status(nullptr, &status) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "engine must not be NULL");
  size_t count = 5;
  CHECK(sonare_engine_graph_node_count(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_count must not be NULL");
  CHECK(sonare_engine_graph_connection_count(nullptr, &count) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "engine must not be NULL");
  CHECK(count == 0);
  sonare_engine_destroy(engine);
}

TEST_CASE("capture buffer refuses its members by name", "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();
  float plane[16] = {};
  float* planes[2] = {plane, nullptr};
  SonareEngineCaptureBuffer buffer{nullptr, 1, 16};

  CHECK(sonare_engine_set_capture_buffer(engine, &buffer) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "buffer.channels must not be NULL");

  buffer = SonareEngineCaptureBuffer{planes, 0, 16};
  CHECK(sonare_engine_set_capture_buffer(engine, &buffer) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "buffer.num_channels must be an integer in [1, 2147483647]");

  buffer = SonareEngineCaptureBuffer{planes, 1, 0};
  CHECK(sonare_engine_set_capture_buffer(engine, &buffer) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "buffer.capacity_frames must be an integer of at least 1");

  buffer = SonareEngineCaptureBuffer{planes, 2, 16};
  CHECK(sonare_engine_set_capture_buffer(engine, &buffer) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "buffer.channels[1] must not be NULL");
  sonare_engine_destroy(engine);
}

TEST_CASE("capture punch, source and monitor refuse by name", "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();

  CHECK(sonare_engine_set_capture_punch(nullptr, 0, 10, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "engine must not be NULL");
  CHECK(sonare_engine_set_capture_punch(engine, -1, 10, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "start_sample must be an integer of at least 0");
  CHECK(sonare_engine_set_capture_punch(engine, 10, 5, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "end_sample must not be before start_sample");
  CHECK(sonare_engine_set_capture_punch(engine, 10, 10, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "end_sample must not equal start_sample");

  CHECK(sonare_engine_set_capture_source(engine, static_cast<SonareEngineCaptureSource>(7)) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "source must be one of 0, 1");

  CHECK(sonare_engine_set_input_monitor(engine, 1, std::nanf("")) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "gain must be finite and in [-3.4028235e38, 3.4028235e38]");
  sonare_engine_destroy(engine);
}

TEST_CASE("engine graph refuses its spec by name", "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();
  SonareEngineGraphNode node;
  sonare_engine_graph_node_init(&node);
  std::snprintf(node.id, sizeof(node.id), "a");
  SonareEngineGraphSpec spec{};
  spec.nodes = &node;
  spec.node_count = 1;
  spec.num_channels = 2;

  CHECK(sonare_engine_set_graph(nullptr, &spec) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "engine must not be NULL");
  CHECK(sonare_engine_set_graph(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "spec must not be NULL");

  SonareEngineGraphSpec bad = spec;
  bad.nodes = nullptr;
  CHECK(sonare_engine_set_graph(engine, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "spec.nodes must not be NULL");

  bad = spec;
  bad.node_count = 0;
  CHECK(sonare_engine_set_graph(engine, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "spec.node_count must be an integer of at least 1");

  bad = spec;
  bad.num_channels = 0;
  CHECK(sonare_engine_set_graph(engine, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "spec.num_channels must be an integer in [1, 2147483647]");

  bad = spec;
  bad.connection_count = 1;
  CHECK(sonare_engine_set_graph(engine, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "spec.connections must not be NULL when spec.connection_count is not 0");

  bad = spec;
  bad.parameter_binding_count = 1;
  CHECK(sonare_engine_set_graph(engine, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "spec.parameter_bindings must not be NULL when spec.parameter_binding_count is not 0");

#if defined(SONARE_WITH_GRAPH)
  SonareEngineGraphNode odd = node;
  bad = spec;
  bad.nodes = &odd;

  odd.struct_size = 0;
  CHECK(sonare_engine_set_graph(engine, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "spec.nodes[0].struct_size must be set by sonare_engine_graph_node_init");

  for (const int bad_ports : {0, -2}) {
    odd = node;
    odd.num_ports = bad_ports;
    CHECK(sonare_engine_set_graph(engine, &bad) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "spec.nodes[0].num_ports must be an integer in [1, 2147483647]");
  }

  odd = node;
  odd.type = 5;
  CHECK(sonare_engine_set_graph(engine, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "spec.nodes[0].type must be one of 0, 1");

  odd = node;
  odd.id[0] = '\0';
  CHECK(sonare_engine_set_graph(engine, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "spec.nodes[0] must have a unique non-empty id and a port count in range");

  SonareEngineGraphConnection connection{};
  std::snprintf(connection.source_node, sizeof(connection.source_node), "a");
  std::snprintf(connection.dest_node, sizeof(connection.dest_node), "missing");
  bad = spec;
  bad.connections = &connection;
  bad.connection_count = 1;
  CHECK(sonare_engine_set_graph(engine, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "spec.connections[0] must be a valid connection that no earlier entry repeats");

  connection.mix = 2;
  CHECK(sonare_engine_set_graph(engine, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "spec.connections[0].mix must be one of 0, 1");

  SonareEngineGraphSpec wired = spec;
  std::snprintf(wired.input_node, sizeof(wired.input_node), "a");
  std::snprintf(wired.output_node, sizeof(wired.output_node), "a");

  bad = wired;
  std::snprintf(bad.input_node, sizeof(bad.input_node), "zz");
  CHECK(sonare_engine_set_graph(engine, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "spec.input_node must be the id of a node in spec.nodes");

  bad = wired;
  std::snprintf(bad.output_node, sizeof(bad.output_node), "zz");
  CHECK(sonare_engine_set_graph(engine, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "spec.output_node must be the id of a node in spec.nodes");

  SonareEngineGraphNode narrow = node;
  narrow.num_ports = 2;
  bad = wired;
  bad.nodes = &narrow;
  bad.num_channels = 3;
  CHECK(sonare_engine_set_graph(engine, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "spec.num_channels must not exceed the port count of spec.input_node");

  SonareEngineGraphParameterBinding bindings[2] = {};
  std::snprintf(bindings[0].node_id, sizeof(bindings[0].node_id), "a");
  std::snprintf(bindings[1].node_id, sizeof(bindings[1].node_id), "a");
  bad = wired;
  bad.parameter_bindings = bindings;
  bad.parameter_binding_count = 1;

  bindings[0].param_id = 0x4D580001u;
  CHECK(sonare_engine_set_graph(engine, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "spec.parameter_bindings[0].param_id must not be a reserved engine parameter id");

  bindings[0].param_id = 0;
  CHECK(sonare_engine_set_graph(engine, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "spec.parameter_bindings[0].param_id must be an integer in [1, 4294967295]");

  bindings[0].param_id = 5;
  bindings[1].param_id = 5;
  bad.parameter_binding_count = 2;
  CHECK(sonare_engine_set_graph(engine, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "spec.parameter_bindings[1].param_id must be unique among the parameter bindings");

  bindings[1].param_id = 6;
  std::snprintf(bindings[1].node_id, sizeof(bindings[1].node_id), "zz");
  CHECK(sonare_engine_set_graph(engine, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "spec.parameter_bindings[1].node_id must be the id of a node in "
        "spec.nodes");
#endif
  sonare_engine_destroy(engine);
}

#if defined(SONARE_WITH_MIXING)

TEST_CASE("mixing strip entries refuse NULL, id and domain by name", "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();

  CHECK(sonare_engine_set_track_strip_pan(nullptr, 1, 0.0f) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "engine must not be NULL");
  CHECK(sonare_engine_set_track_strip_pan(engine, 0, 0.0f) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "track_id must be the id of a track in this engine");
  CHECK(sonare_engine_set_bus_strip_pan(engine, 0, 0.0f) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "bus_id must be the id of a bus in this engine");

  CHECK(sonare_engine_set_track_strip_json(engine, 1, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "settings_json must not be NULL");
  CHECK(sonare_engine_set_track_strip_json(engine, 1, kWholeScene) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message().rfind("unknown key settings.buses (expected ", 0) == 0);
  CHECK(sonare_engine_set_master_strip_json(engine, kWholeScene) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message().rfind("unknown key settings.buses (expected ", 0) == 0);
  CHECK(sonare_engine_set_master_strip_json(engine, "{bad") == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message().rfind("settings must be valid JSON: ", 0) == 0);
  CHECK(sonare_engine_set_master_strip_json(engine, "[]") == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "settings must be an object");
  CHECK(sonare_engine_set_master_strip_json(engine, R"({"faderDB":-1})") ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message().rfind("unknown key settings.faderDB (expected ", 0) == 0);

  CHECK(sonare_engine_set_track_strip_eq_band_json(engine, 1, -1, "{}") ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "band_index must be an integer in [0, 23]");
  CHECK(sonare_engine_set_master_strip_eq_band_json(engine, 0, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "band_json must not be NULL");

  SonareSurroundPan pan{};
  CHECK(sonare_engine_set_track_strip_surround_pan(engine, 1, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "pan must not be NULL");
  pan.azimuth = 181.0f;
  CHECK(sonare_engine_set_track_strip_surround_pan(engine, 1, &pan) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "pan.azimuth must be finite and in [-180, 180]");
  pan.azimuth = 0.0f;
  pan.divergence = 2.0f;
  CHECK(sonare_engine_set_track_strip_surround_pan(engine, 1, &pan) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "pan.divergence must be finite and in [0, 1]");
  pan.divergence = 0.0f;
  pan.lfe = -0.5f;
  CHECK(sonare_engine_set_track_strip_surround_pan(engine, 1, &pan) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "pan.lfe must be finite and in [0, 1]");

  CHECK(sonare_engine_set_track_strip_channel_delay_samples(engine, 1, -1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "delay_samples must be an integer in [0, 192000]");
  CHECK(sonare_engine_set_track_strip_channel_delay_samples(engine, 1, 192001) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "delay_samples must be an integer in [0, 192000]");
  sonare_engine_destroy(engine);
}

TEST_CASE("mixing insert-parameter entries refuse name, value and out by name",
          "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();
  int applied = 7;
  uint32_t id = 7;
  float constructed = 0.0f;

  CHECK(sonare_engine_set_track_strip_insert_param_by_name(engine, 1, 0, nullptr, 0.0f) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "param_name must not be NULL");
  CHECK(sonare_engine_set_track_strip_insert_param_by_name(engine, 1, 0, "", 0.0f) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "param_name must not be empty");
  CHECK(sonare_engine_set_master_strip_insert_param_by_name(engine, 0, "gain", std::nanf("")) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "value must be finite and in [-3.4028235e38, 3.4028235e38]");
  CHECK(sonare_engine_set_bus_strip_insert_param_by_name(engine, 0, 0, "gain", 0.0f) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "bus_id must be the id of a bus in this engine");

  CHECK(sonare_engine_apply_track_strip_insert_param_by_name_now(
            engine, 1, 0, "gain", 0.0f, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_applied must not be NULL");
  CHECK(sonare_engine_apply_track_strip_insert_param_by_name_now(
            engine, 0, 0, "gain", 0.0f, &applied) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "track_id must be the id of a track in this engine");
  CHECK(applied == 0);

  CHECK(sonare_engine_restore_master_strip_insert_param_by_name(engine, 0, "", 0.0f) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "param_name must not be empty");
  CHECK(sonare_engine_clear_track_insert_parameter_bases(engine, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "track_id must be the id of a track in this engine");
  CHECK(sonare_engine_insert_parameter_constructed_value(engine, 1, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_value must not be NULL");
  CHECK(sonare_engine_insert_parameter_constructed_value(nullptr, 1, &constructed) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "engine must not be NULL");

  CHECK(sonare_engine_resolve_track_insert_automation_id(engine, 1, 0, "gain", nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_id must not be NULL");
  CHECK(sonare_engine_resolve_bus_automation_id(engine, 0, "faderDb", &id) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "bus_id must be the id of a bus in this engine");
  CHECK(sonare_engine_resolve_master_automation_id(engine, "", &id) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "param_name must not be empty");
  sonare_engine_destroy(engine);
}

TEST_CASE("mixing routing entries refuse a count, a layout and an id by name",
          "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();

  CHECK(sonare_engine_set_track_lanes(engine, nullptr, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "lanes must not be NULL when lane_count is not 0");
  CHECK(sonare_engine_set_track_sends(engine, 1, nullptr, 2) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "sends must not be NULL when send_count is not 0");
  CHECK(sonare_engine_set_track_buses(engine, nullptr, 3) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "buses must not be NULL when bus_count is not 0");

  SonareEngineTrackLane lane{};
  lane.track_id = 1;
  lane.source_channel_layout = SONARE_CHANNEL_LAYOUT_STEREO;
  CHECK(sonare_engine_set_track_lanes(engine, &lane, 33) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "lane_count must be an integer in [0, 32]");

  lane.send_count = 1;
  CHECK(sonare_engine_set_track_lanes(engine, &lane, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "lanes[0].sends must not be NULL when lanes[0].send_count is not 0");
  lane.send_count = 0;
  lane.source_channel_layout = SONARE_CHANNEL_LAYOUT_MONO;
  CHECK(sonare_engine_set_track_lanes(engine, &lane, 1) == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(last_message() == "lanes[0].source_channel_layout other than stereo is not supported");
  lane.source_channel_layout = 200;
  CHECK(sonare_engine_set_track_lanes(engine, &lane, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "lanes[0].source_channel_layout must name a declared value");

  SonareEngineBus bus{};
  bus.bus_id = 1;
  bus.send_count = 1;
  CHECK(sonare_engine_set_track_buses(engine, &bus, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "buses[0].sends must not be NULL when buses[0].send_count is not 0");
  bus.send_count = 0;
  bus.channel_layout = 200;
  CHECK(sonare_engine_set_track_buses(engine, &bus, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "buses[0].channel_layout must name a declared value");

  // No lane carries track 99, so the lane edit names an unknown track.
  CHECK(sonare_engine_set_track_sends(engine, 99, nullptr, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "track_id must be the id of a track in this engine");
  CHECK(sonare_engine_set_track_output_bus(engine, 99, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "track_id must be the id of a track in this engine");

  CHECK(sonare_engine_set_lane_sidechain(engine, 0, 0, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "track_id must be the id of a track in this engine");
  CHECK(sonare_engine_set_bus_sidechain(engine, 1, 0, 7, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "source_kind must be one of 0, 1");
  CHECK(sonare_engine_set_master_sidechain(engine, 0, 7, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "source_kind must be one of 0, 1");

  int refusal = 0;
  CHECK(sonare_engine_can_set_lane_sidechain(engine, 1, 0, 2, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_refusal must not be NULL");
  CHECK(sonare_engine_can_set_bus_sidechain(nullptr, 1, 0, 0, 1, &refusal) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "engine must not be NULL");
  sonare_engine_destroy(engine);
}

TEST_CASE("mixing setters name the cause of a refused engine edit", "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();
  SonareEngineTrackLane lane{};
  lane.track_id = 10;
  lane.source_channel_layout = SONARE_CHANNEL_LAYOUT_STEREO;
  REQUIRE(sonare_engine_set_track_lanes(engine, &lane, 1) == SONARE_OK);
  SonareEngineBus bus{};
  bus.bus_id = 1;
  bus.channel_layout = SONARE_CHANNEL_LAYOUT_STEREO;
  REQUIRE(sonare_engine_set_track_buses(engine, &bus, 1) == SONARE_OK);

  constexpr const char* kNoTrack = "track_id must be the id of a track in this engine";
  constexpr const char* kNoStrip = "track_id must be the id of a track that has a channel strip";
  constexpr const char* kNoBus = "bus_id must be the id of a bus in this engine";

  CHECK(sonare_engine_set_track_strip_pan(engine, 99, 0.0f) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == kNoTrack);
  CHECK(sonare_engine_set_track_strip_pan(engine, 10, 0.0f) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == kNoStrip);
  CHECK(sonare_engine_set_track_strip_pan(engine, 10, 2.0f) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "pan must be finite and in [-1, 1]");
  CHECK(sonare_engine_set_track_strip_pan_law(engine, 10, 9) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "pan_law must be one of 0, 1, 2, 3");
  CHECK(sonare_engine_set_track_strip_pan_mode(engine, 10, 7) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "pan_mode must be one of 0, 1, 2");
  CHECK(sonare_engine_set_track_strip_dual_pan(engine, 10, 0.0f, 2.0f) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "right_pan must be finite and in [-1, 1]");
  CHECK(sonare_engine_set_track_strip_channel_delay_samples(engine, 10, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == kNoStrip);

  SonareSurroundPan surround{};
  surround.divergence = std::nanf("");
  CHECK(sonare_engine_set_track_strip_surround_pan(engine, 10, &surround) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "pan.divergence must be finite and in [0, 1]");

  CHECK(sonare_engine_set_bus_strip_pan(engine, 7, 0.0f) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == kNoBus);
  CHECK(sonare_engine_set_bus_strip_pan_law(engine, 1, 9) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "pan_law must be one of 0, 1, 2, 3");

  CHECK(sonare_engine_set_track_strip_eq_band_json(engine, 10, 24, "{}") ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "band_index must be an integer in [0, 23]");

  CHECK(sonare_engine_set_track_strip_insert_bypassed(engine, 99, 0, 1, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == kNoTrack);
  CHECK(sonare_engine_set_track_strip_insert_bypassed(engine, 10, 0, 1, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "insert_index must be less than the number of inserts on the track");
  CHECK(sonare_engine_set_bus_strip_insert_bypassed(engine, 7, 0, 1, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == kNoBus);
  CHECK(sonare_engine_set_bus_strip_insert_bypassed(engine, 1, 0, 1, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "insert_index must be less than the number of inserts on the bus");
  CHECK(sonare_engine_set_track_strip_insert_param_by_name(engine, 10, 0, "gain", 0.0f) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "insert_index must be less than the number of inserts on the track");
  CHECK(sonare_engine_restore_bus_strip_insert_param_by_name(engine, 1, 0, "gain", 0.0f) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "insert_index must be less than the number of inserts on the bus");

  CHECK(sonare_engine_clear_track_insert_parameter_bases(engine, 99) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == kNoTrack);
  CHECK(sonare_engine_clear_bus_insert_parameter_bases(engine, 7) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == kNoBus);

  float constructed = 0.0f;
  CHECK(sonare_engine_insert_parameter_constructed_value(engine, 1, &constructed) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "param_id must be an insert parameter automation id");
  CHECK(sonare_engine_insert_parameter_constructed_value(engine, 0xE0000000u, &constructed) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "param_id must be the id of a live insert parameter with a construction value in this "
        "engine");

  uint32_t id = 7;
  CHECK(sonare_engine_resolve_track_insert_automation_id(engine, 10, 0, "gain", &id) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "insert_index must be less than the number of inserts on the track");
  CHECK(sonare_engine_resolve_bus_insert_automation_id(engine, 1, 0, "gain", &id) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "insert_index must be less than the number of inserts on the bus");
  CHECK(sonare_engine_resolve_track_lane_automation_id(engine, 10, "gain", &id) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "param_name must be the name of a track strip parameter: faderDb or pan");
  CHECK(sonare_engine_resolve_track_lane_automation_id(engine, 99, "pan", &id) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == kNoTrack);
  CHECK(sonare_engine_resolve_bus_automation_id(engine, 1, "pan", &id) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "param_name must be the name of a bus strip parameter: faderDb");
  CHECK(sonare_engine_resolve_bus_automation_id(engine, 7, "faderDb", &id) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == kNoBus);
  CHECK(sonare_engine_resolve_master_automation_id(engine, "gain", &id) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "param_name must be the name of a master strip parameter: faderDb, pan "
        "or width");

  SonareEngineTrackSend send{99, 0.0f, 1, 0};
  CHECK(sonare_engine_set_track_sends(engine, 10, &send, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "sends[0].bus_id must be the id of a bus in this engine");
  CHECK(sonare_engine_set_track_output_bus(engine, 10, 99) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "output_bus_id must be the id of a bus in this engine");

  CHECK(sonare_engine_set_lane_sidechain(engine, 99, 0, 10) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == kNoTrack);
  CHECK(sonare_engine_set_lane_sidechain(engine, 10, 0, 10) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "insert_index must be less than the number of inserts on the track");
  CHECK(sonare_engine_set_bus_sidechain(engine, 7, 0, SONARE_SIDECHAIN_SOURCE_TRACK, 10) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == kNoBus);
  sonare_engine_destroy(engine);
}

TEST_CASE("track sends name the send the mixer refused", "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();
  SonareEngineBus bus{1, 0.0f, SONARE_CHANNEL_LAYOUT_STEREO, 0, nullptr, 0};
  REQUIRE(sonare_engine_set_track_buses(engine, &bus, 1) == SONARE_OK);
  SonareEngineTrackLane lane{};
  lane.track_id = 10;
  lane.source_channel_layout = SONARE_CHANNEL_LAYOUT_STEREO;
  REQUIRE(sonare_engine_set_track_lanes(engine, &lane, 1) == SONARE_OK);

  SonareEngineTrackSend zero_id{0, 0.0f, 1, 0};
  CHECK(sonare_engine_set_track_sends(engine, 10, &zero_id, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "sends[0].bus_id must be an integer in [1, 4294967295]");

  SonareEngineTrackSend loud{1, 100.0f, 1, 0};
  CHECK(sonare_engine_set_track_sends(engine, 10, &loud, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "sends[0].level_db must be finite and in [-120, 24]");

  SonareEngineTrackSend repeated[2] = {{1, 0.0f, 1, 0}, {1, -6.0f, 1, 0}};
  CHECK(sonare_engine_set_track_sends(engine, 10, repeated, 2) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "sends[1].bus_id must not repeat an earlier send");

  SonareEngineTrackSend many[9] = {};
  for (SonareEngineTrackSend& send : many) send = SonareEngineTrackSend{1, 0.0f, 1, 0};
  CHECK(sonare_engine_set_track_sends(engine, 10, many, 9) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "send_count must be an integer in [0, 8]");

  CHECK(sonare_engine_set_track_output_bus(engine, 10, 99) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "output_bus_id must be the id of a bus in this engine");
  sonare_engine_destroy(engine);
}

TEST_CASE("track lanes name the lane and send the mixer refused", "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();
  SonareEngineBus bus{1, 0.0f, SONARE_CHANNEL_LAYOUT_STEREO, 0, nullptr, 0};
  REQUIRE(sonare_engine_set_track_buses(engine, &bus, 1) == SONARE_OK);

  SonareEngineTrackLane lanes[2] = {};
  for (SonareEngineTrackLane& lane : lanes) {
    lane.track_id = 10;
    lane.source_channel_layout = SONARE_CHANNEL_LAYOUT_STEREO;
  }
  lanes[0].track_id = 0;
  CHECK(sonare_engine_set_track_lanes(engine, lanes, 2) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "lanes[0].track_id must be an integer in [1, 4294967295]");

  lanes[0].track_id = 10;
  CHECK(sonare_engine_set_track_lanes(engine, lanes, 2) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "lanes[1].track_id must be unique among the lanes");

  lanes[1].track_id = 11;
  lanes[1].output_bus_id = 99;
  CHECK(sonare_engine_set_track_lanes(engine, lanes, 2) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "lanes[1].output_bus_id must be the id of a bus in this engine");

  lanes[1].output_bus_id = 0;
  SonareEngineTrackSend undeclared{99, 0.0f, 1, 0};
  lanes[1].sends = &undeclared;
  lanes[1].send_count = 1;
  CHECK(sonare_engine_set_track_lanes(engine, lanes, 2) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "lanes[1].sends[0].bus_id must be the id of a bus in this engine");
  sonare_engine_destroy(engine);
}

TEST_CASE("track buses name the bus and route the mixer refused", "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();
  SonareEngineBus buses[2] = {{1, 0.0f, SONARE_CHANNEL_LAYOUT_STEREO, 0, nullptr, 0},
                              {2, 0.0f, SONARE_CHANNEL_LAYOUT_STEREO, 0, nullptr, 0}};

  buses[0].bus_id = 0;
  CHECK(sonare_engine_set_track_buses(engine, buses, 2) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "buses[0].bus_id must be an integer in [1, 4294967295]");

  buses[0].bus_id = 1;
  buses[1].gain_db = 100.0f;
  CHECK(sonare_engine_set_track_buses(engine, buses, 2) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "buses[1].gain_db must be finite and in [-120, 24]");

  buses[1].gain_db = 0.0f;
  buses[1].bus_id = 1;
  CHECK(sonare_engine_set_track_buses(engine, buses, 2) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "buses[1].bus_id must be unique among the buses");

  SonareEngineBus nine[9] = {};
  for (uint32_t i = 0; i < 9; ++i) {
    nine[i] = SonareEngineBus{i + 1, 0.0f, SONARE_CHANNEL_LAYOUT_STEREO, 0, nullptr, 0};
  }
  CHECK(sonare_engine_set_track_buses(engine, nine, 9) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "bus_count must be an integer in [0, 8]");

  buses[1].bus_id = 2;
  buses[0].output_bus_id = 99;
  CHECK(sonare_engine_set_track_buses(engine, buses, 2) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "buses[0].output_bus_id must be the id of a bus in buses");

  buses[0].output_bus_id = 1;
  CHECK(sonare_engine_set_track_buses(engine, buses, 2) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "buses[0].output_bus_id must not equal buses[0].bus_id");

  buses[0].output_bus_id = 2;
  buses[1].output_bus_id = 1;
  CHECK(sonare_engine_set_track_buses(engine, buses, 2) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "buses must not route a bus back into itself through outputs, sends or sidechain keys");

  SonareEngineTrackSend to_self{1, 0.0f, 1, 0};
  buses[0].output_bus_id = 0;
  buses[1].output_bus_id = 0;
  buses[0].sends = &to_self;
  buses[0].send_count = 1;
  CHECK(sonare_engine_set_track_buses(engine, buses, 2) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "buses[0].sends[0].bus_id must not equal buses[0].bus_id");
  sonare_engine_destroy(engine);
}

TEST_CASE("track buses name the state that forbids dropping a routed bus",
          "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();
  SonareEngineBus bus{1, 0.0f, SONARE_CHANNEL_LAYOUT_STEREO, 0, nullptr, 0};
  REQUIRE(sonare_engine_set_track_buses(engine, &bus, 1) == SONARE_OK);
  SonareEngineTrackLane lane{};
  lane.track_id = 10;
  lane.output_bus_id = 1;
  lane.source_channel_layout = SONARE_CHANNEL_LAYOUT_STEREO;
  REQUIRE(sonare_engine_set_track_lanes(engine, &lane, 1) == SONARE_OK);

  CHECK(sonare_engine_set_track_buses(engine, nullptr, 0) == SONARE_ERROR_INVALID_STATE);
  CHECK(last_message() == "a lane output or send still routes to a bus the new bus list drops");
  sonare_engine_destroy(engine);
}

TEST_CASE("master strip inserts name the index or parameter the mixer refused",
          "[c_api][engine_refusal]") {
  constexpr const char* kMasterWithInsert =
      R"({"id":"master","inserts":[{"slot":"pre","processor":"dynamics.duckingProcessor","params":{"thresholdDb":-20,"ratio":20,"attackMs":0.05,"releaseMs":80,"rangeDb":30}}]})";
  constexpr const char* kPastInserts =
      "insert_index must be less than the number of inserts on the master strip";
  SonareEngine* engine = make_engine();
  REQUIRE(sonare_engine_set_master_strip_json(engine, kMasterWithInsert) == SONARE_OK);

  CHECK(sonare_engine_set_master_strip_insert_bypassed(engine, 5, 1, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == kPastInserts);
  CHECK(sonare_engine_set_master_strip_insert_param_by_name(engine, 5, "ratio", 4.0f) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == kPastInserts);
  CHECK(sonare_engine_set_master_strip_insert_param_by_name(engine, 0, "nope", 4.0f) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "param_name must be the name of a parameter of the insert that can be set while running");
  CHECK(sonare_engine_restore_master_strip_insert_param_by_name(engine, 5, "ratio", 4.0f) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == kPastInserts);
  uint32_t id = 7;
  CHECK(sonare_engine_resolve_master_insert_automation_id(engine, 5, "ratio", &id) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == kPastInserts);
  id = 7;
  CHECK(sonare_engine_resolve_master_automation_id(engine, nullptr, &id) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "param_name must not be NULL");
  CHECK(id == 0);
  sonare_engine_destroy(engine);
}

namespace {

constexpr const char* kGainInsert =
    R"({"slot":"pre","processor":"utility.gain","params":{"levelDb":0}})";
// A band above the 24 kHz Nyquist of the 48 kHz engine; only the engine knows its rate.
constexpr const char* kPastNyquistEq =
    R"("eq":{"bands":[{"type":"peak","frequencyHz":30000,"gainDb":3,"q":1,"enabled":true}]})";

std::string inserts_of(const std::string& insert, int count) {
  std::string out = "[";
  for (int i = 0; i < count; ++i) out += (i == 0 ? "" : ",") + insert;
  return out + "]";
}

// One strip's settings: its id followed by @p members.
std::string strip_scene(const std::string& id, const std::string& members) {
  return R"({"id":")" + id + R"(")" + (members.empty() ? "" : "," + members) + "}";
}

// One bus's settings: its id followed by @p members.
std::string bus_scene(const std::string& id, const std::string& members) {
  return strip_scene(id, members);
}

std::string inserts_member(const std::string& insert, int count) {
  return R"("inserts":)" + inserts_of(insert, count);
}

SonareEngineTrackLane stereo_lane(uint32_t track_id) {
  SonareEngineTrackLane lane{};
  lane.track_id = track_id;
  lane.source_channel_layout = SONARE_CHANNEL_LAYOUT_STEREO;
  return lane;
}

}  // namespace

TEST_CASE("strip settings name the EQ, insert and bus-image cause the mixer refused",
          "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();
  SonareEngineTrackLane lane = stereo_lane(10);
  REQUIRE(sonare_engine_set_track_lanes(engine, &lane, 1) == SONARE_OK);
  SonareEngineBus buses[2] = {{1, 0.0f, SONARE_CHANNEL_LAYOUT_STEREO, 0, nullptr, 0},
                              {2, 0.0f, SONARE_CHANNEL_LAYOUT_5_1, 0, nullptr, 0}};
  REQUIRE(sonare_engine_set_track_buses(engine, buses, 2) == SONARE_OK);

  constexpr const char* kEqText =
      "settings must describe a strip EQ whose bands are valid at the engine's sample rate";
  const std::string nyquist_strip = strip_scene("s", kPastNyquistEq);
  CHECK(sonare_engine_set_track_strip_json(engine, 10, nyquist_strip.c_str()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == kEqText);
  CHECK(sonare_engine_set_master_strip_json(engine, nyquist_strip.c_str()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == kEqText);
  CHECK(sonare_engine_set_bus_strip_json(engine, 1, bus_scene("1", kPastNyquistEq).c_str()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == kEqText);

  constexpr const char* kUnknownInsert =
      R"({"slot":"pre","processor":"missing.processor","params":{}})";
  constexpr const char* kInsertsText = "settings must describe inserts the mixer can create";
  const std::string unknown_strip = strip_scene("s", inserts_member(kUnknownInsert, 1));
  CHECK(sonare_engine_set_track_strip_json(engine, 10, unknown_strip.c_str()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == kInsertsText);
  CHECK(sonare_engine_set_master_strip_json(engine, unknown_strip.c_str()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == kInsertsText);
  CHECK(sonare_engine_set_bus_strip_json(
            engine, 1, bus_scene("1", inserts_member(kUnknownInsert, 1)).c_str()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == kInsertsText);

  // The scene's own layout is stereo, so only the engine's 5.1 bus refuses the pan.
  CHECK(sonare_engine_set_bus_strip_json(engine, 2, bus_scene("2", R"("pan":0.5)").c_str()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "settings must leave the pan and width of a bus wider than stereo at their defaults");
  CHECK(sonare_engine_set_bus_strip_json(engine, 2, bus_scene("2", R"("width":1.5)").c_str()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "settings must leave the pan and width of a bus wider than stereo at their defaults");
  sonare_engine_destroy(engine);
}

TEST_CASE("track buses refuse a wider layout while a bus keeps a pan", "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();
  SonareEngineBus bus{1, 0.0f, SONARE_CHANNEL_LAYOUT_STEREO, 0, nullptr, 0};
  REQUIRE(sonare_engine_set_track_buses(engine, &bus, 1) == SONARE_OK);
  REQUIRE(sonare_engine_set_bus_strip_json(engine, 1, bus_scene("1", R"("pan":0.5)").c_str()) ==
          SONARE_OK);

  bus.channel_layout = SONARE_CHANNEL_LAYOUT_5_1;
  CHECK(sonare_engine_set_track_buses(engine, &bus, 1) == SONARE_ERROR_INVALID_STATE);
  CHECK(last_message() ==
        "buses[0].channel_layout cannot be wider than stereo while the bus keeps a pan or width");
  sonare_engine_destroy(engine);
}

TEST_CASE("track strips name the table that earlier strips filled", "[c_api][engine_refusal]") {
  const std::string scene = strip_scene("s", R"("faderDb":-1)");

  SECTION("owned strip table") {
    SonareEngine* engine = make_engine();
    for (uint32_t track = 1; track <= 32; ++track) {
      REQUIRE(sonare_engine_set_track_strip_json(engine, track, scene.c_str()) == SONARE_OK);
    }
    CHECK(sonare_engine_set_track_strip_json(engine, 33, scene.c_str()) ==
          SONARE_ERROR_INVALID_STATE);
    CHECK(last_message() == "the owned strip table is full (32 strips)");
    sonare_engine_destroy(engine);
  }

  SECTION("lane table") {
    SonareEngine* engine = make_engine();
    SonareEngineTrackLane lanes[32];
    for (uint32_t i = 0; i < 32; ++i) lanes[i] = stereo_lane(i + 1);
    REQUIRE(sonare_engine_set_track_lanes(engine, lanes, 32) == SONARE_OK);
    CHECK(sonare_engine_set_track_strip_json(engine, 99, scene.c_str()) ==
          SONARE_ERROR_INVALID_STATE);
    CHECK(last_message() == "the lane table is full (32 lanes)");
    sonare_engine_destroy(engine);
  }
}

TEST_CASE("master strip names the full insert automation table", "[c_api][engine_refusal]") {
  // Retired entries are never reused, so swapping a 64-slot chain between two processors mints 64.
  const std::string gain = strip_scene("master", inserts_member(kGainInsert, 64));
  const std::string mono = strip_scene(
      "master", inserts_member(R"({"slot":"pre","processor":"stereo.monoMaker","params":{}})", 64));
  SonareEngine* engine = make_engine();

  SonareError result = SONARE_OK;
  int swaps = 0;
  for (; swaps < 200 && result == SONARE_OK; ++swaps) {
    result = sonare_engine_set_master_strip_json(engine, (swaps % 2 == 0 ? gain : mono).c_str());
  }
  REQUIRE(result != SONARE_OK);
  CHECK(result == SONARE_ERROR_INVALID_STATE);
  CHECK(last_message() == "the insert automation table is full (8192 entries)");
  sonare_engine_destroy(engine);
}

TEST_CASE("track strips name a latency past the alignment ceiling", "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();
  SonareEngineTrackLane lane = stereo_lane(10);
  REQUIRE(sonare_engine_set_track_lanes(engine, &lane, 1) == SONARE_OK);

  // 5000 ms at 48 kHz is 240000 samples of lookahead, past the 192000-sample ceiling.
  const std::string slow = strip_scene(
      "s",
      inserts_member(
          R"({"slot":"pre","processor":"dynamics.limiter","params":{"lookaheadMs":5000}})", 1));
  CHECK(sonare_engine_set_track_strip_json(engine, 10, slow.c_str()) == SONARE_ERROR_INVALID_STATE);
  CHECK(last_message() ==
        "the latency the mixer has to align is past the alignment ceiling of 192000 samples");
  sonare_engine_destroy(engine);
}

TEST_CASE("sidechain keys name the self key, the cycle and the full table",
          "[c_api][engine_refusal]") {
  const std::string two_inserts = strip_scene("s", inserts_member(kGainInsert, 1));
  SonareEngine* engine = make_engine();
  SonareEngineTrackLane lanes[2] = {stereo_lane(10), stereo_lane(11)};
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes, 2) == SONARE_OK);
  REQUIRE(sonare_engine_set_track_strip_json(engine, 10, two_inserts.c_str()) == SONARE_OK);
  REQUIRE(sonare_engine_set_track_strip_json(engine, 11, two_inserts.c_str()) == SONARE_OK);
  SonareEngineBus buses[2] = {{1, 0.0f, SONARE_CHANNEL_LAYOUT_STEREO, 0, nullptr, 0},
                              {2, 0.0f, SONARE_CHANNEL_LAYOUT_STEREO, 0, nullptr, 0}};
  REQUIRE(sonare_engine_set_track_buses(engine, buses, 2) == SONARE_OK);
  const std::string bus_chain = bus_scene("b", inserts_member(kGainInsert, 1));
  REQUIRE(sonare_engine_set_bus_strip_json(engine, 1, bus_chain.c_str()) == SONARE_OK);
  REQUIRE(sonare_engine_set_bus_strip_json(engine, 2, bus_chain.c_str()) == SONARE_OK);

  CHECK(sonare_engine_set_lane_sidechain(engine, 10, 0, 10) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "source_track_id must not equal track_id");
  CHECK(sonare_engine_set_bus_sidechain(engine, 1, 0, SONARE_SIDECHAIN_SOURCE_BUS, 1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "source_id must not equal bus_id");

  REQUIRE(sonare_engine_set_lane_sidechain(engine, 10, 0, 11) == SONARE_OK);
  CHECK(sonare_engine_set_lane_sidechain(engine, 11, 0, 10) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "source_track_id must not make the sidechain keys form a cycle");
  REQUIRE(sonare_engine_set_bus_sidechain(engine, 1, 0, SONARE_SIDECHAIN_SOURCE_BUS, 2) ==
          SONARE_OK);
  CHECK(sonare_engine_set_bus_sidechain(engine, 2, 0, SONARE_SIDECHAIN_SOURCE_BUS, 1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "source_id must not make the sidechain keys form a cycle");
  sonare_engine_destroy(engine);
}

TEST_CASE("sidechain keys name the full binding table", "[c_api][engine_refusal]") {
  const std::string many = strip_scene("s", inserts_member(kGainInsert, 33));
  SonareEngine* engine = make_engine();
  SonareEngineTrackLane lanes[2] = {stereo_lane(10), stereo_lane(11)};
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes, 2) == SONARE_OK);
  REQUIRE(sonare_engine_set_track_strip_json(engine, 10, many.c_str()) == SONARE_OK);
  for (unsigned int insert = 0; insert < 32; ++insert) {
    REQUIRE(sonare_engine_set_lane_sidechain(engine, 10, insert, 11) == SONARE_OK);
  }
  CHECK(sonare_engine_set_lane_sidechain(engine, 10, 32, 11) == SONARE_ERROR_INVALID_STATE);
  CHECK(last_message() == "the sidechain binding table is full (32 entries)");
  sonare_engine_destroy(engine);
}

TEST_CASE("a master sidechain key names a latency past the alignment ceiling",
          "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();
  SonareEngineTrackLane lanes[2] = {stereo_lane(10), stereo_lane(11)};
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes, 2) == SONARE_OK);
  // 4000 ms at 48 kHz is exactly the 192000-sample ceiling, so the lane alone still fits.
  const std::string slow = strip_scene(
      "s",
      inserts_member(
          R"({"slot":"pre","processor":"dynamics.limiter","params":{"lookaheadMs":4000}})", 1));
  REQUIRE(sonare_engine_set_track_strip_json(engine, 10, slow.c_str()) == SONARE_OK);
  const std::string master = strip_scene(
      "master",
      R"("inserts":[{"slot":"pre","processor":"dynamics.limiter","params":{"lookaheadMs":1}},)" +
          std::string(kGainInsert) + "]");
  REQUIRE(sonare_engine_set_master_strip_json(engine, master.c_str()) == SONARE_OK);

  // The master input arrives at the ceiling and the key leaves a lane with no latency.
  CHECK(sonare_engine_set_master_sidechain(engine, 1, SONARE_SIDECHAIN_SOURCE_TRACK, 11) ==
        SONARE_ERROR_INVALID_STATE);
  CHECK(last_message() ==
        "the latency the sidechain key would add to the mix is past the alignment ceiling of "
        "192000 samples");
  sonare_engine_destroy(engine);
}

namespace {

constexpr int kRoundTripFrames = 128 * 16;

// Which strip setter a round trip drives.
enum class StripSetter { Track, Bus, Master };

// The clip a round trip plays; the engine reads it in place, so it outlives the engine.
struct Tone {
  std::vector<float> samples;
  const float* channels[2] = {nullptr, nullptr};
};

// An engine playing a 440 Hz tone on track 10, routed through bus 1 for StripSetter::Bus.
SonareEngine* tone_engine(StripSetter setter, Tone& tone) {
  SonareEngine* engine = make_engine();
  tone.samples.assign(kRoundTripFrames, 0.0f);
  for (int i = 0; i < kRoundTripFrames; ++i) {
    tone.samples[static_cast<size_t>(i)] =
        0.5f * std::sin(sonare::constants::kTwoPi * 440.0f * i / 48000.0f);
  }
  tone.channels[0] = tone.samples.data();
  tone.channels[1] = tone.samples.data();
  const float* const* channels = tone.channels;
  SonareEngineClip clip;
  sonare_engine_clip_init(&clip);
  clip.id = 1;
  clip.track_id = 10;
  clip.channels = channels;
  clip.num_channels = 2;
  clip.num_samples = kRoundTripFrames;
  clip.length_samples = kRoundTripFrames;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);
  SonareEngineBus bus{1, 0.0f, SONARE_CHANNEL_LAYOUT_STEREO, 0, nullptr, 0};
  REQUIRE(sonare_engine_set_track_buses(engine, &bus, 1) == SONARE_OK);
  SonareEngineTrackLane lane = stereo_lane(10);
  lane.output_bus_id = setter == StripSetter::Bus ? 1 : 0;
  REQUIRE(sonare_engine_set_track_lanes(engine, &lane, 1) == SONARE_OK);
  return engine;
}

SonareError set_strip(SonareEngine* engine, StripSetter setter, const std::string& settings) {
  const char* text = settings.c_str();
  switch (setter) {
    case StripSetter::Track:
      return sonare_engine_set_track_strip_json(engine, 10, text);
    case StripSetter::Bus:
      return sonare_engine_set_bus_strip_json(engine, 1, text);
    case StripSetter::Master:
      return sonare_engine_set_master_strip_json(engine, text);
  }
  return SONARE_ERROR_INVALID_PARAMETER;
}

// Bounces @p engine and destroys it.
std::vector<float> bounce_and_destroy(SonareEngine* engine) {
  SonareEngineBounceOptions options;
  sonare_engine_bounce_options_init(&options);
  options.total_frames = kRoundTripFrames;
  options.block_size = 128;
  options.num_channels = 2;
  options.source_sample_rate = 48000;
  options.target_sample_rate = 48000;
  options.normalize_lufs = 0;
  options.dither = 0;
  SonareEngineBounceResult result{};
  REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) == SONARE_OK);
  std::vector<float> out(result.interleaved,
                         result.interleaved + static_cast<size_t>(result.frames) * 2);
  sonare_free_engine_bounce_result(&result);
  sonare_engine_destroy(engine);
  return out;
}

// The bounce after handing @p settings to @p setter; empty @p settings keeps the defaults.
std::vector<float> bounce_after(StripSetter setter, const std::string& settings) {
  Tone tone;
  SonareEngine* engine = tone_engine(setter, tone);
  if (!settings.empty()) REQUIRE(set_strip(engine, setter, settings) == SONARE_OK);
  return bounce_and_destroy(engine);
}

constexpr const char* kSavedStrip =
    R"({"id":"s","faderDb":-6,"pan":0.3,"eq":{"bands":[{"type":"peak","frequencyHz":1000,"gainDb":6,"q":1,"enabled":true}]}})";
constexpr const char* kSavedBus = R"({"id":"b","inputTrimDb":-4,"pan":-0.2})";

// The scene a mixer holding kSavedStrip and kSavedBus writes.
std::string saved_scene() {
  const std::string scene = std::string(R"({"version":2,"strips":[)") + kSavedStrip +
                            R"(],"buses":[)" + kSavedBus + R"(],"connections":[]})";
  SonareMixer* mixer = sonare_mixer_from_scene_json(scene.c_str(), 48000, 128);
  REQUIRE(mixer != nullptr);
  char* written = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(mixer, &written) == SONARE_OK);
  std::string out(written);
  sonare_free_string(written);
  sonare_mixer_destroy(mixer);
  return out;
}

}  // namespace

TEST_CASE("strip setters take back the element a saved scene holds for that strip",
          "[c_api][engine_refusal]") {
  namespace json = sonare::util::json;
  const json::Value scene = json::parse(saved_scene());
  const std::string strip = json::dump(scene["strips"][0]);
  const std::string bus = json::dump(scene["buses"][0]);
  for (const StripSetter setter : {StripSetter::Track, StripSetter::Master, StripSetter::Bus}) {
    const bool is_bus = setter == StripSetter::Bus;
    const std::vector<float> handed = bounce_after(setter, is_bus ? kSavedBus : kSavedStrip);
    CHECK(bounce_after(setter, is_bus ? bus : strip) == handed);
    CHECK(bounce_after(setter, std::string()) != handed);
  }
}

TEST_CASE("the master strip setter refuses a saved scene by name and keeps its strip",
          "[c_api][engine_refusal]") {
  const std::string scene = saved_scene();
  Tone tone;
  SonareEngine* engine = tone_engine(StripSetter::Master, tone);
  REQUIRE(set_strip(engine, StripSetter::Master, kSavedStrip) == SONARE_OK);
  CHECK(set_strip(engine, StripSetter::Master, scene) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message().rfind("unknown key settings.", 0) == 0);
  CHECK(bounce_and_destroy(engine) == bounce_after(StripSetter::Master, kSavedStrip));
}

#endif  // defined(SONARE_WITH_MIXING)
