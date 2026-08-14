/// @file padding_test.cpp
/// @brief Unit + librosa parity tests for util/padding.

#include "util/padding.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <vector>

#include "util/exception.h"
#include "util/json_reader.h"
#include "util/reflect_padding.h"

using namespace sonare;
using namespace sonare::test;
using Catch::Matchers::WithinAbs;

TEST_CASE("pad_center centers data in target size", "[util][padding]") {
  std::vector<float> x{1.0f, 2.0f, 3.0f};
  auto r = pad_center(x, 7, 0.0f);
  REQUIRE(r.size() == 7);
  // (7 - 3) / 2 = 2 zeros prepended
  REQUIRE(r[0] == 0.0f);
  REQUIRE(r[1] == 0.0f);
  REQUIRE(r[2] == 1.0f);
  REQUIRE(r[3] == 2.0f);
  REQUIRE(r[4] == 3.0f);
  REQUIRE(r[5] == 0.0f);
  REQUIRE(r[6] == 0.0f);
}

TEST_CASE("pad_center rejects shrinking", "[util][padding][edge]") {
  std::vector<float> x{1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
  REQUIRE_THROWS_AS(pad_center(x, 3), SonareException);
}

TEST_CASE("padding raw empty inputs accept null pointers", "[util][padding][edge]") {
  REQUIRE(pad_center(nullptr, 0, 0).empty());
  const auto centered = pad_center(nullptr, 0, 3, -2.0f);
  REQUIRE(centered == std::vector<float>{-2.0f, -2.0f, -2.0f});

  REQUIRE(fix_length(nullptr, 0, 0).empty());
  const auto padded = fix_length(nullptr, 0, 2, 7.0f);
  REQUIRE(padded == std::vector<float>{7.0f, 7.0f});

  REQUIRE_THROWS_AS(pad_center(nullptr, 1, 1), SonareException);
  REQUIRE_THROWS_AS(fix_length(nullptr, 1, 1), SonareException);
}

TEST_CASE("fix_length truncates and pads", "[util][padding]") {
  std::vector<float> x{1, 2, 3, 4, 5};
  auto trunc = fix_length(x, 3);
  REQUIRE(trunc == std::vector<float>{1, 2, 3});
  auto pad = fix_length(x, 7, -1.0f);
  REQUIRE(pad == std::vector<float>{1, 2, 3, 4, 5, -1, -1});
}

TEST_CASE("fix_frames adds bounds and removes duplicates", "[util][padding]") {
  std::vector<int> frames{1, 3, 3, 5};
  auto r = fix_frames(frames, 0, 10, true);
  REQUIRE(r == std::vector<int>{0, 1, 3, 5, 10});
}

TEST_CASE("reflect_center_pad reflects the input at both edges", "[util][reflect_padding]") {
  const std::vector<float> input{1.0f, 2.0f, 3.0f};
  const auto output = reflect_center_pad(input.data(), input.size(), 1);

  REQUIRE(output == std::vector<float>{2.0f, 1.0f, 2.0f, 3.0f, 2.0f});
}

TEST_CASE("reflect_center_pad preserves the input with zero padding", "[util][reflect_padding]") {
  const std::vector<float> input{1.0f, 2.0f, 3.0f};
  const auto output = reflect_center_pad(input.data(), input.size(), 0);

  REQUIRE(output == input);
}

TEST_CASE("reflect_center_pad rejects negative padding", "[util][reflect_padding][edge]") {
  const std::vector<float> input{1.0f, 2.0f, 3.0f};

  try {
    static_cast<void>(reflect_center_pad(input.data(), input.size(), -1));
    FAIL("Expected reflect_center_pad to reject negative padding");
  } catch (const SonareException& error) {
    REQUIRE(error.code() == ErrorCode::InvalidParameter);
  }
}

TEST_CASE("util.pad_center matches librosa", "[librosa][util][padding]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/padding.json");
  const auto& pc = json["data"]["pad_center"];
  const auto& input_arr = pc["input"].as_array();
  int size = pc["size"].as_int();
  const auto& expected_arr = pc["expected"].as_array();

  std::vector<float> input;
  input.reserve(input_arr.size());
  for (const auto& v : input_arr) input.push_back(v.as_float());

  auto out = pad_center(input, static_cast<size_t>(size), 0.0f);
  REQUIRE(out.size() == expected_arr.size());
  for (size_t i = 0; i < out.size(); ++i) {
    REQUIRE_THAT(out[i], WithinAbs(expected_arr[i].as_float(), 1e-6f));
  }
}

TEST_CASE("util.fix_length matches librosa", "[librosa][util][padding]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/padding.json");

  auto run = [&](const std::string& key) {
    const auto& obj = json["data"][key];
    const auto& input_arr = obj["input"].as_array();
    int size = obj["size"].as_int();
    const auto& expected_arr = obj["expected"].as_array();

    std::vector<float> input;
    input.reserve(input_arr.size());
    for (const auto& v : input_arr) input.push_back(v.as_float());

    auto out = fix_length(input, static_cast<size_t>(size), 0.0f);
    REQUIRE(out.size() == expected_arr.size());
    for (size_t i = 0; i < out.size(); ++i) {
      REQUIRE_THAT(out[i], WithinAbs(expected_arr[i].as_float(), 1e-6f));
    }
  };

  SECTION("truncate") { run("fix_length_truncate"); }
  SECTION("pad") { run("fix_length_pad"); }
}

TEST_CASE("util.fix_frames matches librosa", "[librosa][util][padding]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/padding.json");
  const auto& ff = json["data"]["fix_frames"];
  const auto& input_arr = ff["input"].as_array();
  int x_min = ff["x_min"].as_int();
  int x_max = ff["x_max"].as_int();
  const auto& expected_arr = ff["expected"].as_array();

  std::vector<int> frames;
  frames.reserve(input_arr.size());
  for (const auto& v : input_arr) frames.push_back(v.as_int());

  auto out = fix_frames(frames, x_min, x_max, true);
  REQUIRE(out.size() == expected_arr.size());
  for (size_t i = 0; i < out.size(); ++i) {
    REQUIRE(out[i] == expected_arr[i].as_int());
  }
}
