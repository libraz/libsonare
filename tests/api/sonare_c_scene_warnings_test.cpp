/// @file sonare_c_scene_warnings_test.cpp
/// @brief Unknown mixing scene keys through the C ABI: a whole scene or project document and a
///        partial strip fragment applied to the live engine all refuse them, naming the key path.

#include <sonare/sonare_c.h>
#include <sonare/sonare_c_engine.h>
#include <sonare/sonare_c_project.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

namespace {

#if defined(SONARE_WITH_ARRANGEMENT) || defined(SONARE_WITH_MIXING)

std::string last_error() { return sonare_last_error_message(); }

#endif

#if defined(SONARE_WITH_ARRANGEMENT)

constexpr const char* kCleanScene =
    R"({"version":2,"strips":[{"id":"lead","faderDb":-3.0}],"buses":[{"id":"master","role":"master"}]})";

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

TEST_CASE("sonare_project_set_mixer_scene_json refuses unknown scene keys", "[c_api][project]") {
  OwnedProject project;

  // Extension keys are allowed; the first unknown key is refused by path.
  const char* typo =
      R"({"version":2,"strips":[{"id":"lead","faderDB":-3.0,"x-note":1}],"$schema":"s",)"
      R"("buses":[{"id":"master","role":"master"}]})";
  REQUIRE(sonare_project_set_mixer_scene_json(project.handle, typo) == SONARE_ERROR_INVALID_FORMAT);
  CHECK(last_error().find("unknown key strips[0].faderDB") != std::string::npos);
  CHECK(sonare_last_diagnostic_count() == 0);

  const char* bus_typo = R"({"version":2,"buses":[{"id":"master","role":"master","roel":"aux"}]})";
  REQUIRE(sonare_project_set_mixer_scene_json(project.handle, bus_typo) ==
          SONARE_ERROR_INVALID_FORMAT);
  CHECK(last_error().find("unknown key buses[0].roel") != std::string::npos);

  const char* annotated =
      R"({"$schema":"s","x-note":1,"version":2,"strips":[{"id":"lead","x-tag":1}]})";
  REQUIRE(sonare_project_set_mixer_scene_json(project.handle, annotated) == SONARE_OK);
  REQUIRE(sonare_project_set_mixer_scene_json(project.handle, kCleanScene) == SONARE_OK);
  CHECK(sonare_last_diagnostic_count() == 0);
}

TEST_CASE("sonare_project_deserialize refuses unknown scene keys", "[c_api][project]") {
  const std::string typo =
      R"({"version":7,"scene":{"version":2,"strips":[{"id":"lead","faderDB":-3.0}],)"
      R"("buses":[{"id":"master","role":"master","x-note":true}]}})";
  SonareProject* loaded = nullptr;
  char* diagnostics = nullptr;
  REQUIRE(sonare_project_deserialize(typo.data(), typo.size(), &loaded, &diagnostics) ==
          SONARE_ERROR_INVALID_FORMAT);
  const std::string report = take_diagnostics(diagnostics);
  CHECK(loaded == nullptr);
  CHECK(report.find("invalid_format: unknown key scene.strips[0].faderDB") != std::string::npos);
  CHECK(report.find("unknown_scene_key") == std::string::npos);
  CHECK(report.find("x-note") == std::string::npos);

  // The writer's own output loads without a diagnostic.
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
  CHECK(take_diagnostics(diagnostics).find("unknown key") == std::string::npos);
  sonare_project_destroy(loaded);
}

#endif  // SONARE_WITH_ARRANGEMENT

#if defined(SONARE_WITH_MIXING)

struct StripEngine {
  SonareEngine* engine = nullptr;
  StripEngine() {
    REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
    REQUIRE(sonare_engine_prepare(engine, 48000, 256, 64, 64, 0) == SONARE_OK);
    SonareEngineTrackLane lane[] = {{10, nullptr, 0, 0, 1}};
    REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);
    SonareEngineBus buses[] = {{1, 0.0f, 0, 0, nullptr, 0}};
    REQUIRE(sonare_engine_set_track_buses(engine, buses, 1) == SONARE_OK);
  }
  ~StripEngine() { sonare_engine_destroy(engine); }
  StripEngine(const StripEngine&) = delete;
  StripEngine& operator=(const StripEngine&) = delete;
};

TEST_CASE("engine strip setters refuse an unknown key by name", "[c_api][engine]") {
  StripEngine f;
  const char* strip_typo = R"({"version":2,"strips":[{"id":"s","faderDB":-3}]})";
  const char* bus_typo = R"({"version":2,"buses":[{"id":"1","roel":"aux"}]})";

  REQUIRE(sonare_engine_set_track_strip_json(f.engine, 10, strip_typo) ==
          SONARE_ERROR_INVALID_FORMAT);
  CHECK(last_error().find("unknown key strips[0].faderDB") != std::string::npos);
  REQUIRE(sonare_engine_set_master_strip_json(f.engine, strip_typo) == SONARE_ERROR_INVALID_FORMAT);
  CHECK(last_error().find("unknown key strips[0].faderDB") != std::string::npos);
  REQUIRE(sonare_engine_set_bus_strip_json(f.engine, 1, bus_typo) == SONARE_ERROR_INVALID_FORMAT);
  CHECK(last_error().find("unknown key buses[0].roel") != std::string::npos);

  // A key that would forge a second line is reported escaped, on one line.
  const char* control = "{\"version\":2,\"strips\":[{\"id\":\"s\",\"a\\nb\":1}]}";
  REQUIRE(sonare_engine_set_track_strip_json(f.engine, 10, control) == SONARE_ERROR_INVALID_FORMAT);
  CHECK(last_error().find('\n') == std::string::npos);
  CHECK(last_error().find("\\x0a") != std::string::npos);
}

TEST_CASE("engine strip setters accept annotation keys and refuse earlier spellings",
          "[c_api][engine]") {
  StripEngine f;
  const char* annotated =
      R"({"$schema":"s","x-note":1,"version":2,"strips":[{"id":"s","faderDb":-3,"x-tag":[1]}]})";
  CHECK(sonare_engine_set_track_strip_json(f.engine, 10, annotated) == SONARE_OK);
  CHECK(sonare_engine_set_master_strip_json(f.engine, annotated) == SONARE_OK);

  const char* earlier = R"({"version":2,"strips":[{"id":"s","fader_db":-1}]})";
  CHECK(sonare_engine_set_track_strip_json(f.engine, 10, earlier) == SONARE_ERROR_INVALID_FORMAT);
  CHECK(std::string(sonare_last_error_message()).find("faderDb") != std::string::npos);
  const char* earlier_bus = R"({"version":2,"buses":[{"id":"1","pan_mode":"stereoPan"}]})";
  CHECK(sonare_engine_set_bus_strip_json(f.engine, 1, earlier_bus) == SONARE_ERROR_INVALID_FORMAT);
  CHECK(std::string(sonare_last_error_message()).find("panMode") != std::string::npos);
}

TEST_CASE("engine EQ band setters refuse an unknown key by name", "[c_api][engine]") {
  StripEngine f;
  const char* typo = R"({"type":"peak","frequencyHz":1000,"gainDB":3,"qq":1})";
  REQUIRE(sonare_engine_set_track_strip_eq_band_json(f.engine, 10, 0, typo) ==
          SONARE_ERROR_INVALID_FORMAT);
  CHECK(last_error().find("unknown key gainDB") != std::string::npos);
  REQUIRE(sonare_engine_set_master_strip_eq_band_json(f.engine, 0, typo) ==
          SONARE_ERROR_INVALID_FORMAT);
  CHECK(last_error().find("unknown key gainDB") != std::string::npos);
  REQUIRE(sonare_engine_set_bus_strip_eq_band_json(f.engine, 1, 0, typo) ==
          SONARE_ERROR_INVALID_FORMAT);
  CHECK(last_error().find("unknown key gainDB") != std::string::npos);

  const char* control = "{\"a\\nb\":1}";
  REQUIRE(sonare_engine_set_track_strip_eq_band_json(f.engine, 10, 0, control) ==
          SONARE_ERROR_INVALID_FORMAT);
  CHECK(last_error().find('\n') == std::string::npos);
  CHECK(last_error().find("unknown key a\\x0ab") != std::string::npos);

  const char* annotated =
      R"({"$schema":"s","x-note":1,"type":"peak","frequencyHz":1000,"gainDb":3,"dynamic":false})";
  REQUIRE(sonare_engine_set_track_strip_json(
              f.engine, 10, R"({"version":2,"strips":[{"id":"s","faderDb":0}]})") == SONARE_OK);
  CHECK(sonare_engine_set_track_strip_eq_band_json(f.engine, 10, 0, annotated) == SONARE_OK);
}

#endif  // SONARE_WITH_MIXING

#if defined(SONARE_WITH_MASTERING)

TEST_CASE("sonare_eq_set_band refuses an unknown key by name", "[c_api][warning]") {
  SonareEq* eq = nullptr;
  REQUIRE(sonare_eq_create(48000, 512, &eq) == SONARE_OK);
  REQUIRE(eq != nullptr);
  CHECK(sonare_eq_set_band(eq, 0, R"({"type":"peak","gainDB":3})") == SONARE_ERROR_INVALID_FORMAT);
  CHECK(std::string(sonare_last_error_message()).find("unknown key gainDB") != std::string::npos);
  CHECK(sonare_eq_set_band(eq, 0, R"({"type":"peak","gainDb":3,"x-note":"keep"})") == SONARE_OK);
  sonare_eq_destroy(eq);
}

#endif  // SONARE_WITH_MASTERING

}  // namespace
