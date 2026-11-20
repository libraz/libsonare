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

namespace {

std::string last_message() { return std::string(sonare_last_error_message()); }

SonareEngine* make_engine() {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);
  return engine;
}

constexpr const char* kEmptyScene = R"({"version":2,"strips":[],"buses":[],"connections":[]})";

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
  SonareEngineGraphNode node{};
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

  odd.num_ports = -1;
  CHECK(sonare_engine_set_graph(engine, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "spec.nodes[0].num_ports must be an integer in [0, 2147483647]");

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
        "spec.parameter_bindings[1].param_id must be unique among "
        "spec.parameter_bindings");

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
  CHECK(last_message() == "scene_json must not be NULL");
  CHECK(sonare_engine_set_track_strip_json(engine, 1, kEmptyScene) == SONARE_ERROR_INVALID_FORMAT);
  CHECK(last_message() == "scene_json must contain at least one strip");
  CHECK(sonare_engine_set_master_strip_json(engine, kEmptyScene) == SONARE_ERROR_INVALID_FORMAT);
  CHECK(last_message() == "scene_json must contain at least one strip");
  CHECK(sonare_engine_set_bus_strip_json(engine, 1, kEmptyScene) == SONARE_ERROR_INVALID_FORMAT);
  CHECK(last_message() == "scene_json must contain at least one bus");

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
  CHECK(last_message() == "sends[1].bus_id must not repeat the bus_id of an earlier send");

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
      R"({"version":2,"strips":[{"id":"master","inserts":[{"slot":"pre","processor":"dynamics.duckingProcessor","params":{"thresholdDb":-20,"ratio":20,"attackMs":0.05,"releaseMs":80,"rangeDb":30}}]}],"buses":[],"connections":[]})";
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

#endif  // defined(SONARE_WITH_MIXING)
