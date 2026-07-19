// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#include <sonare/sonare_c.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "util/json.h"

namespace {

using MarkerBytes = std::array<unsigned char, sizeof(SonareEngineMarker)>;

sonare::util::json::Value load_public_input_corpus() {
  std::ifstream input("tests/conformance/public_input_corpus.json");
  REQUIRE(input.good());
  const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return sonare::util::json::parse_strict(text);
}

uint32_t corpus_marker_id(const sonare::util::json::Value& value) {
  if (value.is_string()) {
    REQUIRE(value.as_string() == "uint32_max");
    return std::numeric_limits<uint32_t>::max();
  }
  return static_cast<uint32_t>(value.as_number());
}

double corpus_ppq(const sonare::util::json::Value& value) {
  if (!value.is_string()) return value.as_number();
  if (value.as_string() == "nan") return std::numeric_limits<double>::quiet_NaN();
  REQUIRE(value.as_string() == "inf");
  return std::numeric_limits<double>::infinity();
}

float corpus_float(const sonare::util::json::Value& value) {
  if (!value.is_string()) return static_cast<float>(value.as_number());
  if (value.as_string() == "nan") return std::numeric_limits<float>::quiet_NaN();
  if (value.as_string() == "inf") return std::numeric_limits<float>::infinity();
  REQUIRE(value.as_string() == "neg_inf");
  return -std::numeric_limits<float>::infinity();
}

std::vector<SonareEngineMarker> corpus_markers(const sonare::util::json::Value& value) {
  std::vector<SonareEngineMarker> markers;
  markers.reserve(value.size());
  for (const auto& item : value.as_array()) {
    SonareEngineMarker marker{};
    marker.id = corpus_marker_id(item["id"]);
    marker.ppq = corpus_ppq(item["ppq"]);
    const std::string& name = item["name"].as_string();
    std::strncpy(marker.name, name.c_str(), sizeof(marker.name) - 1);
    markers.push_back(marker);
  }
  return markers;
}

std::vector<MarkerBytes> marker_snapshot(SonareRealtimeEngine* engine) {
  size_t count = 0;
  REQUIRE(sonare_engine_marker_count(engine, &count) == SONARE_OK);
  std::vector<MarkerBytes> snapshot(count);
  for (size_t i = 0; i < count; ++i) {
    SonareEngineMarker marker{};
    REQUIRE(sonare_engine_marker_by_index(engine, i, &marker) == SONARE_OK);
    std::memcpy(snapshot[i].data(), &marker, sizeof(marker));
  }
  return snapshot;
}

}  // namespace

TEST_CASE("public input corpus preserves atomic marker state across the C ABI",
          "[conformance][c_api][engine]") {
  const auto corpus = load_public_input_corpus();
  const auto& transaction = corpus["marker_transaction"];
  const auto initial = corpus_markers(transaction["initial"]);

  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

  for (const auto& test_case : transaction["cases"].as_array()) {
    DYNAMIC_SECTION(test_case["id"].as_string()) {
      if (!test_case["c_representable"].as_bool()) continue;
      REQUIRE(sonare_engine_set_markers(engine, initial.data(), initial.size()) == SONARE_OK);
      const auto before = marker_snapshot(engine);
      const auto candidate = corpus_markers(test_case["markers"]);
      const SonareError result =
          sonare_engine_set_markers(engine, candidate.data(), candidate.size());
      if (test_case["accepted"].as_bool()) {
        REQUIRE(result == SONARE_OK);
        REQUIRE(marker_snapshot(engine) == [&candidate] {
          std::vector<MarkerBytes> bytes(candidate.size());
          for (size_t i = 0; i < candidate.size(); ++i) {
            std::memcpy(bytes[i].data(), &candidate[i], sizeof(candidate[i]));
          }
          return bytes;
        }());
      } else {
        REQUIRE(result == SONARE_ERROR_INVALID_PARAMETER);
        REQUIRE(marker_snapshot(engine) == before);
      }
    }
  }

  sonare_engine_destroy(engine);
}

#ifdef SONARE_WITH_MIXING
TEST_CASE("public input corpus keeps mixer option rejection conformant across the C ABI",
          "[conformance][c_api][mixing]") {
  const auto corpus = load_public_input_corpus();
  for (const auto& test_case : corpus["mix_options"]["cases"].as_array()) {
    DYNAMIC_SECTION(test_case["id"].as_string()) {
      if (!test_case["c_representable"].as_bool()) continue;

      SonareMixer* mixer = sonare_mixer_create(48000, 128);
      REQUIRE(mixer != nullptr);
      SonareStrip* strip = sonare_mixer_add_strip(mixer, "corpus");
      REQUIRE(strip != nullptr);

      const std::string& field = test_case["field"].as_string();
      const float value = corpus_float(test_case["value"]);
      SonareError result = SONARE_OK;
      if (field == "input_trim_db") {
        result = sonare_strip_set_input_trim_db(strip, value);
      } else if (field == "fader_db") {
        result = sonare_strip_set_fader_db(strip, value);
      } else if (field == "pan") {
        result = sonare_strip_set_pan(strip, value, SONARE_PAN_MODE_BALANCE);
      } else if (field == "pan_mode") {
        result = sonare_strip_set_pan(strip, 0.0f, static_cast<int>(value));
      } else if (field == "width") {
        result = sonare_strip_set_width(strip, value);
      } else {
        FAIL("unknown mix option corpus field");
      }

      REQUIRE(result == SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(sonare_strip_set_fader_db(strip, 0.0f) == SONARE_OK);
      sonare_mixer_destroy(mixer);
    }
  }
}
#endif
