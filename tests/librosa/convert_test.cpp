/// @file convert_test.cpp
/// @brief librosa compatibility tests for unit conversion functions.
/// @details Reference values from: tests/librosa/reference/convert.json

#include "core/convert.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("convert librosa compatibility", "[convert][librosa]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/convert.json");
  const auto& data = json["data"].as_array();

  for (const auto& item : data) {
    if (item.contains("hz")) {
      float hz = item["hz"].as_float();
      CAPTURE(hz);

      SECTION("hz_to_mel Slaney hz=" + std::to_string(static_cast<int>(hz))) {
        float expected = item["mel_slaney"].as_float();
        REQUIRE_THAT(hz_to_mel(hz), WithinRel(expected, 1e-5f));
      }

      SECTION("hz_to_mel HTK hz=" + std::to_string(static_cast<int>(hz))) {
        float expected = item["mel_htk"].as_float();
        REQUIRE_THAT(hz_to_mel_htk(hz), WithinRel(expected, 1e-5f));
      }

      SECTION("hz_to_midi hz=" + std::to_string(static_cast<int>(hz))) {
        float expected = item["midi"].as_float();
        REQUIRE_THAT(hz_to_midi(hz), WithinRel(expected, 1e-5f));
      }
    }

    if (item.contains("mel")) {
      float mel = item["mel"].as_float();
      CAPTURE(mel);

      SECTION("mel_to_hz Slaney mel=" + std::to_string(static_cast<int>(mel))) {
        float expected = item["hz_slaney"].as_float();
        if (expected == 0.0f) {
          REQUIRE_THAT(mel_to_hz(mel), WithinAbs(expected, 1e-6f));
        } else {
          REQUIRE_THAT(mel_to_hz(mel), WithinRel(expected, 1e-5f));
        }
      }

      SECTION("mel_to_hz HTK mel=" + std::to_string(static_cast<int>(mel))) {
        float expected = item["hz_htk"].as_float();
        if (expected == 0.0f) {
          REQUIRE_THAT(mel_to_hz_htk(mel), WithinAbs(expected, 1e-6f));
        } else {
          REQUIRE_THAT(mel_to_hz_htk(mel), WithinRel(expected, 1e-4f));
        }
      }
    }
  }
}
