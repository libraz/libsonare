/// @file sonare_c_scene_warnings_test.cpp
/// @brief Unknown mixing scene keys through the C ABI: a whole scene or project document
///        reports them, a partial strip fragment applied to the live engine refuses them.

#include <sonare/sonare_c.h>
#include <sonare/sonare_c_engine.h>
#include <sonare/sonare_c_project.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>

namespace {

#if defined(SONARE_WITH_ARRANGEMENT)

constexpr const char* kCleanScene =
    R"({"version":1,"strips":[{"id":"lead","faderDb":-3.0}],"buses":[{"id":"master","role":"master"}]})";

struct OwnedProject {
  SonareProject* handle = nullptr;
  OwnedProject() { REQUIRE(sonare_project_create(&handle) == SONARE_OK); }
  ~OwnedProject() { sonare_project_destroy(handle); }
  OwnedProject(const OwnedProject&) = delete;
  OwnedProject& operator=(const OwnedProject&) = delete;
};

std::string take_diagnostics(char* diagnostics) {
  std::string out = diagnostics != nullptr ? diagnostics : "";
  if (diagnostics != nullptr) sonare_free_string(diagnostics);
  return out;
}

TEST_CASE("sonare_project_set_mixer_scene_json reports unknown scene keys as warnings",
          "[c_api][project][warning]") {
  OwnedProject project;

  const char* typo =
      R"({"version":1,"strips":[{"id":"lead","faderDB":-3.0,"x-note":1}],"$schema":"s",)"
      R"("buses":[{"id":"master","role":"master","roel":"aux"}]})";
  REQUIRE(sonare_project_set_mixer_scene_json(project.handle, typo) == SONARE_OK);
  const std::string warning = sonare_last_warning_message();
  CHECK(warning.find("unknown scene key 'strips[0].faderDB'") != std::string::npos);
  CHECK(warning.find("unknown scene key 'buses[0].roel'") != std::string::npos);
  CHECK(warning.find('\n') != std::string::npos);
  CHECK(warning.find("x-note") == std::string::npos);
  CHECK(warning.find("$schema") == std::string::npos);

  // A clean scene clears the previous call's warning.
  REQUIRE(sonare_project_set_mixer_scene_json(project.handle, kCleanScene) == SONARE_OK);
  CHECK(std::string(sonare_last_warning_message()).empty());
}

TEST_CASE("sonare_project_deserialize reports unknown scene keys with the project path",
          "[c_api][project][warning]") {
  const std::string typo =
      R"({"version":1,"scene":{"version":1,"strips":[{"id":"lead","faderDB":-3.0}],)"
      R"("buses":[{"id":"master","role":"master","x-note":true}]}})";
  SonareProject* loaded = nullptr;
  char* diagnostics = nullptr;
  REQUIRE(sonare_project_deserialize(typo.data(), typo.size(), &loaded, &diagnostics) == SONARE_OK);
  const std::string report = take_diagnostics(diagnostics);
  sonare_project_destroy(loaded);
  CHECK(report.find("unknown_scene_key: unknown scene key 'scene.strips[0].faderDB'") !=
        std::string::npos);
  CHECK(report.find("x-note") == std::string::npos);

  // The writer's own output carries no unknown key.
  OwnedProject project;
  REQUIRE(sonare_project_set_mixer_scene_json(project.handle, kCleanScene) == SONARE_OK);
  char* json = nullptr;
  size_t json_size = 0;
  REQUIRE(sonare_project_serialize(project.handle, &json, &json_size) == SONARE_OK);
  const std::string saved(json, json_size);
  sonare_free_string(json);
  loaded = nullptr;
  diagnostics = nullptr;
  REQUIRE(sonare_project_deserialize(saved.data(), saved.size(), &loaded, &diagnostics) ==
          SONARE_OK);
  CHECK(take_diagnostics(diagnostics).find("unknown_scene_key") == std::string::npos);
  sonare_project_destroy(loaded);
}

#endif  // SONARE_WITH_ARRANGEMENT

#if defined(SONARE_WITH_MIXING)

struct StripEngine {
  SonareRealtimeEngine* engine = nullptr;
  StripEngine() {
    REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
    REQUIRE(sonare_engine_prepare(engine, 48000.0, 256, 64, 64) == SONARE_OK);
    SonareEngineTrackLane lane[] = {{10, nullptr, 0, 0, 1}};
    REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);
    SonareEngineBus buses[] = {{1, 0.0f, 0, 0, nullptr, 0}};
    REQUIRE(sonare_engine_set_track_buses(engine, buses, 1) == SONARE_OK);
  }
  ~StripEngine() { sonare_engine_destroy(engine); }
  StripEngine(const StripEngine&) = delete;
  StripEngine& operator=(const StripEngine&) = delete;
};

std::string last_error() { return sonare_last_error_message(); }

TEST_CASE("engine strip setters refuse an unknown key by name", "[c_api][engine][warning]") {
  StripEngine f;
  const char* strip_typo = R"({"version":1,"strips":[{"id":"s","faderDB":-3}]})";
  const char* bus_typo = R"({"version":1,"buses":[{"id":"1","roel":"aux"}]})";

  REQUIRE(sonare_engine_set_track_strip_json(f.engine, 10, strip_typo) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error().find("unknown strip key 'strips[0].faderDB'") != std::string::npos);
  REQUIRE(sonare_engine_set_master_strip_json(f.engine, strip_typo) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error().find("unknown strip key 'strips[0].faderDB'") != std::string::npos);
  REQUIRE(sonare_engine_set_bus_strip_json(f.engine, 1, bus_typo) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error().find("unknown strip key 'buses[0].roel'") != std::string::npos);

  // A key that would forge a second line is reported escaped, on one line.
  const char* control = "{\"version\":1,\"strips\":[{\"id\":\"s\",\"a\\nb\":1}]}";
  REQUIRE(sonare_engine_set_track_strip_json(f.engine, 10, control) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error().find('\n') == std::string::npos);
}

TEST_CASE("engine strip setters accept annotation keys and every legacy alias",
          "[c_api][engine][warning]") {
  StripEngine f;
  const char* annotated =
      R"({"$schema":"s","x-note":1,"version":1,"strips":[{"id":"s","faderDb":-3,"x-tag":[1]}]})";
  CHECK(sonare_engine_set_track_strip_json(f.engine, 10, annotated) == SONARE_OK);
  CHECK(sonare_engine_set_master_strip_json(f.engine, annotated) == SONARE_OK);

  const char* legacy =
      R"({"version":1,"strips":[{"id":"s","input_trim_db":1,"fader_db":-1,"vca_offset_db":0,)"
      R"("solo_safe":true,"pan_mode":1,"dual_pan_left":-0.5,"dual_pan_right":0.5,)"
      R"("polarity_invert_left":true,"polarity_invert_right":false,"pan_law":1,)"
      R"("channel_delay_samples":4}],"vca_groups":[]})";
  CHECK(sonare_engine_set_track_strip_json(f.engine, 10, legacy) == SONARE_OK);
  const char* legacy_bus =
      R"({"version":1,"buses":[{"id":"1","input_trim_db":1,"polarity_invert_left":true,)"
      R"("polarity_invert_right":true,"pan_mode":1,"dual_pan_left":-0.5,"dual_pan_right":0.5,)"
      R"("pan_law":1}]})";
  CHECK(sonare_engine_set_bus_strip_json(f.engine, 1, legacy_bus) == SONARE_OK);
}

TEST_CASE("engine EQ band setters refuse an unknown key by name", "[c_api][engine][warning]") {
  StripEngine f;
  const char* typo = R"({"type":"Peak","frequencyHz":1000,"gainDB":3,"qq":1})";
  REQUIRE(sonare_engine_set_track_strip_eq_band_json(f.engine, 10, 0, typo) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error().find("unknown band key 'gainDB' (and 1 more)") != std::string::npos);
  REQUIRE(sonare_engine_set_master_strip_eq_band_json(f.engine, 0, typo) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error().find("unknown band key 'gainDB'") != std::string::npos);
  REQUIRE(sonare_engine_set_bus_strip_eq_band_json(f.engine, 1, 0, typo) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error().find("unknown band key 'gainDB'") != std::string::npos);

  const char* control = "{\"a\\nb\":1}";
  REQUIRE(sonare_engine_set_track_strip_eq_band_json(f.engine, 10, 0, control) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error().find('\n') == std::string::npos);
  CHECK(last_error().find("unknown band key 'a\\x0ab'") != std::string::npos);

  const char* annotated =
      R"({"$schema":"s","x-note":1,"type":"Peak","frequencyHz":1000,"gain_db":3,"dyn_enabled":false})";
  REQUIRE(sonare_engine_set_track_strip_json(
              f.engine, 10, R"({"version":1,"strips":[{"id":"s","faderDb":0}]})") == SONARE_OK);
  CHECK(sonare_engine_set_track_strip_eq_band_json(f.engine, 10, 0, annotated) == SONARE_OK);
}

#endif  // SONARE_WITH_MIXING

#if defined(SONARE_WITH_MASTERING)

TEST_CASE("sonare_eq_set_band refuses an unknown key by name", "[c_api][warning]") {
  SonareEq* eq = sonare_eq_create(48000.0, 512);
  REQUIRE(eq != nullptr);
  CHECK(sonare_eq_set_band(eq, 0, R"({"type":"Peak","gainDB":3})") ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("unknown band key 'gainDB'") !=
        std::string::npos);
  CHECK(sonare_eq_set_band(eq, 0, R"({"type":"Peak","gainDb":3,"x-note":"keep"})") == SONARE_OK);
  sonare_eq_destroy(eq);
}

#endif  // SONARE_WITH_MASTERING

}  // namespace
