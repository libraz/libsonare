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
  const std::string json = sonare_capabilities_json();
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
