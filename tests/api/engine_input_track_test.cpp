/// @file engine_input_track_test.cpp
/// @brief The live input track and input monitor C entries: set / read / clear round trips, an
///        unknown track refused with the state kept, a dropped lane reading as absent, and the
///        monitor reader unaffected by configuring capture.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace {

SonareEngine* make_engine() {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 16, 16, 0) == SONARE_OK);
  return engine;
}

std::string last_subject() {
  const char* subject = sonare_last_refusal_subject(0);
  return subject != nullptr ? subject : "";
}

void read_monitor(const SonareEngine* engine, int* enabled, float* gain) {
  REQUIRE(sonare_engine_input_monitor(engine, enabled, gain) == SONARE_OK);
}

}  // namespace

TEST_CASE("the input monitor reader returns what the setter wrote", "[c_api][engine_input]") {
  SonareEngine* engine = make_engine();
  int enabled = -1;
  float gain = -1.0f;
  read_monitor(engine, &enabled, &gain);
  CHECK(enabled == 1);
  CHECK(gain == 1.0f);

  REQUIRE(sonare_engine_set_input_monitor(engine, 0, 0.25f) == SONARE_OK);
  read_monitor(engine, &enabled, &gain);
  CHECK(enabled == 0);
  CHECK(gain == 0.25f);

  REQUIRE(sonare_engine_set_input_monitor(engine, 1, 2.0f) == SONARE_OK);
  read_monitor(engine, &enabled, &gain);
  CHECK(enabled == 1);
  CHECK(gain == 2.0f);
  sonare_engine_destroy(engine);
}

TEST_CASE("configuring capture leaves the input monitor as it was set", "[c_api][engine_input]") {
  SonareEngine* engine = make_engine();
  REQUIRE(sonare_engine_set_input_monitor(engine, 0, 0.5f) == SONARE_OK);

  std::vector<float> plane_l(256);
  std::vector<float> plane_r(256);
  float* planes[2] = {plane_l.data(), plane_r.data()};
  const SonareEngineCaptureBuffer buffer{planes, 2, 256};
  REQUIRE(sonare_engine_set_capture_buffer(engine, &buffer) == SONARE_OK);
  REQUIRE(sonare_engine_set_capture_source(engine, SONARE_ENGINE_CAPTURE_SOURCE_INPUT) ==
          SONARE_OK);
  REQUIRE(sonare_engine_arm_capture(engine, 1) == SONARE_OK);

  int enabled = -1;
  float gain = -1.0f;
  read_monitor(engine, &enabled, &gain);
  CHECK(enabled == 0);
  CHECK(gain == 0.5f);
  REQUIRE(sonare_engine_arm_capture(engine, 0) == SONARE_OK);
  sonare_engine_destroy(engine);
}

TEST_CASE("the input monitor reader refuses missing arguments", "[c_api][engine_input]") {
  SonareEngine* engine = make_engine();
  int enabled = -1;
  float gain = -1.0f;
  CHECK(sonare_engine_input_monitor(nullptr, &enabled, &gain) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(enabled == 0);
  CHECK(gain == 0.0f);
  CHECK(sonare_engine_input_monitor(engine, nullptr, &gain) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_engine_input_monitor(engine, &enabled, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  sonare_engine_destroy(engine);
}

#if defined(SONARE_WITH_MIXING)

namespace {

void set_lanes(SonareEngine* engine, const std::vector<uint32_t>& ids) {
  std::vector<SonareEngineTrackLane> lanes(ids.size());
  for (size_t i = 0; i < ids.size(); ++i) {
    lanes[i].track_id = ids[i];
    lanes[i].source_channel_layout = SONARE_CHANNEL_LAYOUT_STEREO;
  }
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes.data(), lanes.size()) == SONARE_OK);
}

/// (has_track, track_id) as the reader reports them.
std::pair<int, uint32_t> read_track(const SonareEngine* engine) {
  uint32_t track_id = 99;
  int has_track = -1;
  REQUIRE(sonare_engine_input_track(engine, &track_id, &has_track) == SONARE_OK);
  return {has_track, track_id};
}

}  // namespace

TEST_CASE("the input track reader returns what was set and nothing after a clear",
          "[c_api][engine_input]") {
  SonareEngine* engine = make_engine();
  set_lanes(engine, {10, 20});
  CHECK(read_track(engine) == std::pair<int, uint32_t>{0, 0});

  REQUIRE(sonare_engine_set_input_track(engine, 10) == SONARE_OK);
  CHECK(read_track(engine) == std::pair<int, uint32_t>{1, 10});
  REQUIRE(sonare_engine_set_input_track(engine, 20) == SONARE_OK);
  CHECK(read_track(engine) == std::pair<int, uint32_t>{1, 20});

  REQUIRE(sonare_engine_clear_input_track(engine) == SONARE_OK);
  CHECK(read_track(engine) == std::pair<int, uint32_t>{0, 0});
  REQUIRE(sonare_engine_clear_input_track(engine) == SONARE_OK);
  CHECK(read_track(engine) == std::pair<int, uint32_t>{0, 0});
  sonare_engine_destroy(engine);
}

TEST_CASE("an input track no lane carries is refused under track_id and changes nothing",
          "[c_api][engine_input]") {
  SonareEngine* engine = make_engine();
  set_lanes(engine, {10});
  REQUIRE(sonare_engine_set_input_track(engine, 10) == SONARE_OK);

  CHECK(sonare_engine_set_input_track(engine, 11) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_subject() == "track_id");
  CHECK(std::string(sonare_last_error_message()).find("track lane") != std::string::npos);
  CHECK(read_track(engine) == std::pair<int, uint32_t>{1, 10});
  sonare_engine_destroy(engine);
}

TEST_CASE("dropping the input track's lane leaves no input track", "[c_api][engine_input]") {
  SonareEngine* engine = make_engine();
  set_lanes(engine, {10, 20});
  REQUIRE(sonare_engine_set_input_track(engine, 10) == SONARE_OK);
  set_lanes(engine, {20});
  CHECK(read_track(engine) == std::pair<int, uint32_t>{0, 0});
  sonare_engine_destroy(engine);
}

TEST_CASE("the input track may be set before prepare and survives it", "[c_api][engine_input]") {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  set_lanes(engine, {10});
  REQUIRE(sonare_engine_set_input_track(engine, 10) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 16, 16, 0) == SONARE_OK);
  CHECK(read_track(engine) == std::pair<int, uint32_t>{1, 10});
  REQUIRE(sonare_engine_prepare(engine, 44100, 256, 16, 16, 0) == SONARE_OK);
  CHECK(read_track(engine) == std::pair<int, uint32_t>{1, 10});
  sonare_engine_destroy(engine);
}

TEST_CASE("the input track entries refuse missing arguments", "[c_api][engine_input]") {
  SonareEngine* engine = make_engine();
  uint32_t track_id = 99;
  int has_track = -1;
  CHECK(sonare_engine_input_track(nullptr, &track_id, &has_track) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(track_id == 0);
  CHECK(has_track == 0);
  CHECK(sonare_engine_input_track(engine, nullptr, &has_track) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_engine_input_track(engine, &track_id, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_engine_set_input_track(nullptr, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_engine_clear_input_track(nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  sonare_engine_destroy(engine);
}

#else

TEST_CASE("the input track entries are not supported without mixing", "[c_api][engine_input]") {
  SonareEngine* engine = make_engine();
  uint32_t track_id = 99;
  int has_track = -1;
  CHECK(sonare_engine_set_input_track(engine, 1) == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(sonare_engine_clear_input_track(engine) == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(sonare_engine_input_track(engine, &track_id, &has_track) == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(track_id == 0);
  CHECK(has_track == 0);
  sonare_engine_destroy(engine);
}

#endif
