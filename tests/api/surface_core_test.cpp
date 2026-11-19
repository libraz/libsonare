/// @file surface_core_test.cpp
/// @brief Consumer-side tests of the core part of the public C surface.
///
/// Cases reach the library only through sonare_c.h, exactly as an installed
/// consumer would, and carry the tags [surface][core].

#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <utility>
#include <vector>

#include "support/c_strings.h"

TEST_CASE("StaleResult is error code 11 with its own message", "[surface][core]") {
  STATIC_REQUIRE(SONARE_ERROR_STALE_RESULT == 11);
  const std::string message = sonare_error_message(SONARE_ERROR_STALE_RESULT);
  REQUIRE(message == "Stale result");
  REQUIRE(message != sonare_error_message(SONARE_ERROR_UNKNOWN));
}

namespace {

// Reads the flat `"features":{"name":bool,...}` object out of the capabilities
// document without a JSON library, so the case stays a pure C-ABI consumer.
std::vector<std::pair<std::string, bool>> capability_features() {
  const std::string json = sonare_test::c_string(sonare_capabilities_json);
  const std::string marker = "\"features\":{";
  const size_t begin = json.find(marker);
  REQUIRE(begin != std::string::npos);
  const size_t end = json.find('}', begin);
  REQUIRE(end != std::string::npos);
  std::vector<std::pair<std::string, bool>> features;
  size_t pos = begin + marker.size();
  while (pos < end) {
    const size_t key_begin = json.find('"', pos) + 1;
    const size_t key_end = json.find('"', key_begin);
    const size_t value_begin = key_end + 2;
    const bool value = json.compare(value_begin, 4, "true") == 0;
    REQUIRE((value || json.compare(value_begin, 5, "false") == 0));
    features.emplace_back(json.substr(key_begin, key_end - key_begin), value);
    pos = json.find_first_of(",}", value_begin) + 1;
  }
  return features;
}

}  // namespace

TEST_CASE("sonare_has_feature agrees with every capabilities feature", "[surface][core]") {
  const auto features = capability_features();
  REQUIRE(features.size() >= 2);
  for (const auto& [name, enabled] : features) {
    INFO(name);
    CHECK(sonare_has_feature(name.c_str()) == (enabled ? 1 : 0));
  }
}

TEST_CASE("ffmpeg and vocalEdit are capabilities keys in every build", "[surface][core]") {
  const auto features = capability_features();
  for (const char* required : {"ffmpeg", "vocalEdit"}) {
    INFO(required);
    CHECK(std::any_of(features.begin(), features.end(),
                      [&](const auto& feature) { return feature.first == required; }));
  }
}

TEST_CASE("sonare_has_feature refuses unknown and NULL names", "[surface][core]") {
  CHECK(sonare_has_feature(nullptr) == 0);
  CHECK(sonare_has_feature("") == 0);
  CHECK(sonare_has_feature("noSuchFeature") == 0);
  CHECK(sonare_has_feature("FFMPEG") == 0);
}

TEST_CASE("Version macros are reachable from sonare_c.h", "[surface][core]") {
  const std::string expected = std::to_string(SONARE_VERSION_MAJOR) + "." +
                               std::to_string(SONARE_VERSION_MINOR) + "." +
                               std::to_string(SONARE_VERSION_PATCH);
  CHECK(std::string(SONARE_VERSION_STRING) == expected);
  CHECK(std::string(sonare_version()) == SONARE_VERSION_STRING);
}

TEST_CASE("Each ABI counter accessor returns its macro", "[surface][core]") {
  CHECK(sonare_engine_abi_version() == SONARE_ENGINE_ABI_VERSION);
  CHECK(sonare_vocal_edit_abi_version() == SONARE_VOCAL_EDIT_ABI_VERSION);
  CHECK(sonare_abi_version() == SONARE_ABI_VERSION);
}

namespace {

std::vector<std::pair<std::string, std::string>> diagnostic_codes_and_messages() {
  std::vector<std::pair<std::string, std::string>> out;
  for (size_t i = 0; i < sonare_last_diagnostic_count(); ++i) {
    SonareDiagnostic diagnostic;
    sonare_diagnostic_init(&diagnostic);
    REQUIRE(sonare_last_diagnostic(i, &diagnostic) == SONARE_OK);
    REQUIRE(diagnostic.severity == SONARE_DIAGNOSTIC_WARNING);
    out.emplace_back(diagnostic.code, diagnostic.message);
  }
  return out;
}

}  // namespace

TEST_CASE("sonare_diagnostic_init sets the struct size and empty strings", "[surface][core]") {
  SonareDiagnostic diagnostic;
  sonare_diagnostic_init(&diagnostic);
  CHECK(diagnostic.struct_size == sizeof(SonareDiagnostic));
  CHECK(std::string(diagnostic.code).empty());
  CHECK(std::string(diagnostic.message).empty());
}

#if defined(SONARE_WITH_MIXING)

namespace {

// One load that trips each mixing scene warning once.
constexpr const char* kSceneWithEveryMixingWarning = R"({
  "version": 1,
  "buses": [{"id": "master", "role": "master"}],
  "strips": [{"id": "a", "faderDb": -3,
    "inserts": [{"slot": "post", "processor": "eq.parametric",
                 "params": {"highPassHz":80}}],
    "sends": [{"id": "s", "destinationBusId": "aux1typo", "sendDb": 0}]}],
  "connections": [{"source": "a", "destination": "master"}]
})";

}  // namespace

TEST_CASE("Mixer scene warnings carry their public codes in a fixed order", "[surface][core]") {
  SonareMixer* mixer = sonare_mixer_from_scene_json(kSceneWithEveryMixingWarning, 48000, 512);
  REQUIRE(mixer != nullptr);
  const auto diagnostics = diagnostic_codes_and_messages();
  sonare_mixer_destroy(mixer);
  REQUIRE(diagnostics.size() == 2);
  CHECK(diagnostics[0].first == "mixing.ignored_insert_param");
  CHECK(diagnostics[1].first == "mixing.undeclared_send");
  CHECK(diagnostics[1].second ==
        "send 'a' targets undeclared bus 'aux1typo'; created as implicit aux");
}

TEST_CASE("sonare_last_diagnostic refuses a bad request and leaves the list intact",
          "[surface][core]") {
  SonareMixer* mixer = sonare_mixer_from_scene_json(kSceneWithEveryMixingWarning, 48000, 512);
  REQUIRE(mixer != nullptr);
  sonare_mixer_destroy(mixer);
  REQUIRE(sonare_last_diagnostic_count() == 2);

  SonareDiagnostic diagnostic;
  sonare_diagnostic_init(&diagnostic);
  const char* const untouched_code = diagnostic.code;

  SECTION("NULL out") {
    CHECK(sonare_last_diagnostic(0, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }
  SECTION("struct_size below the struct") {
    diagnostic.struct_size = sizeof(SonareDiagnostic) - 1;
    CHECK(sonare_last_diagnostic(0, &diagnostic) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(diagnostic.code == untouched_code);
  }
  SECTION("index past the count") {
    CHECK(sonare_last_diagnostic(2, &diagnostic) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(diagnostic.code == untouched_code);
  }
  // A refused read neither clears the list nor records an error message.
  CHECK(sonare_last_diagnostic_count() == 2);
  CHECK(std::string(sonare_last_error_message()).empty());
}

#endif

#if defined(SONARE_WITH_ARRANGEMENT)

TEST_CASE("Project scene refuses an unknown key instead of warning", "[surface][core]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  const char* typo = R"({"version":1,"strips":[{"id":"lead","faderDB":-3.0}],)"
                     R"("buses":[{"id":"master","role":"master"}]})";
  CHECK(sonare_project_set_mixer_scene_json(project, typo) == SONARE_ERROR_INVALID_FORMAT);
  CHECK(std::string(sonare_last_error_message()).find("unknown key strips[0].faderDB") !=
        std::string::npos);
  CHECK(diagnostic_codes_and_messages().empty());
  sonare_project_destroy(project);
}

#endif
