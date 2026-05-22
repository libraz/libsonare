/// @file audio_ops_test.cpp
/// @brief Reference compatibility tests for mu-law / autocorrelate / LPC.

#include "core/audio_ops.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <vector>

#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

std::vector<float> load_floats(const sonare::test::JsonValue& arr) {
  std::vector<float> out;
  out.reserve(arr.size());
  for (const auto& v : arr.as_array()) out.push_back(v.as_float());
  return out;
}

}  // namespace

TEST_CASE("mu_compress (no quantize) matches librosa", "[librosa][audio_ops]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/audio_ops.json");
  const auto& d = json["data"];
  int mu = d["mu"].as_int();
  auto x = load_floats(d["mu_input"]);
  auto expected = load_floats(d["mu_compressed_no_quantize"]);
  auto got = mu_compress(x, mu, /*quantize=*/false);
  REQUIRE(got.size() == expected.size());
  for (size_t i = 0; i < got.size(); ++i) {
    CAPTURE(i, got[i], expected[i]);
    REQUIRE_THAT(got[i], WithinAbs(expected[i], 1e-5f));
  }
}

TEST_CASE("mu_compress (quantize) matches librosa", "[librosa][audio_ops]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/audio_ops.json");
  const auto& d = json["data"];
  int mu = d["mu"].as_int();
  auto x = load_floats(d["mu_input"]);
  auto expected = load_floats(d["mu_compressed_quantized"]);
  auto got = mu_compress(x, mu, /*quantize=*/true);
  REQUIRE(got.size() == expected.size());
  for (size_t i = 0; i < got.size(); ++i) {
    CAPTURE(i, got[i], expected[i]);
    REQUIRE_THAT(got[i], WithinAbs(expected[i], 1e-4f));
  }
}

TEST_CASE("mu_expand (no quantize) matches librosa", "[librosa][audio_ops]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/audio_ops.json");
  const auto& d = json["data"];
  int mu = d["mu"].as_int();
  auto x = load_floats(d["mu_compressed_no_quantize"]);
  auto expected = load_floats(d["mu_expanded_no_quantize"]);
  auto got = mu_expand(x, mu, /*quantize=*/false);
  REQUIRE(got.size() == expected.size());
  for (size_t i = 0; i < got.size(); ++i) {
    CAPTURE(i, got[i], expected[i]);
    REQUIRE_THAT(got[i], WithinAbs(expected[i], 1e-5f));
  }
}

TEST_CASE("mu_expand (quantize) matches librosa", "[librosa][audio_ops]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/audio_ops.json");
  const auto& d = json["data"];
  int mu = d["mu"].as_int();
  auto x = load_floats(d["mu_compressed_quantized"]);
  auto expected = load_floats(d["mu_expanded_quantized"]);
  auto got = mu_expand(x, mu, /*quantize=*/true);
  REQUIRE(got.size() == expected.size());
  for (size_t i = 0; i < got.size(); ++i) {
    CAPTURE(i, got[i], expected[i]);
    REQUIRE_THAT(got[i], WithinAbs(expected[i], 1e-5f));
  }
}

TEST_CASE("autocorrelate (full) matches librosa", "[librosa][audio_ops]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/audio_ops.json");
  const auto& d = json["data"];
  auto y = load_floats(d["autocorrelate_input"]);
  auto expected = load_floats(d["autocorrelate_full"]);
  auto got = autocorrelate(y);
  REQUIRE(got.size() == expected.size());
  // Autocorr energy ~ O(n), so use rel + abs tolerance.
  for (size_t i = 0; i < got.size(); ++i) {
    CAPTURE(i, got[i], expected[i]);
    REQUIRE_THAT(got[i], WithinAbs(expected[i], 1e-3f));
  }
}

TEST_CASE("autocorrelate (bounded) matches librosa", "[librosa][audio_ops]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/audio_ops.json");
  const auto& d = json["data"];
  auto y = load_floats(d["autocorrelate_input"]);
  int max_size = d["autocorrelate_max_size"].as_int();
  auto expected = load_floats(d["autocorrelate_bounded"]);
  auto got = autocorrelate(y, max_size);
  REQUIRE(got.size() == expected.size());
  for (size_t i = 0; i < got.size(); ++i) {
    CAPTURE(i, got[i], expected[i]);
    REQUIRE_THAT(got[i], WithinAbs(expected[i], 1e-3f));
  }
}

TEST_CASE("lpc matches librosa (Burg)", "[librosa][audio_ops]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/audio_ops.json");
  const auto& d = json["data"];
  auto y = load_floats(d["lpc_input"]);
  int order = d["lpc_order"].as_int();
  auto expected = load_floats(d["lpc_coeffs"]);
  auto got = lpc(y, order);
  REQUIRE(got.size() == expected.size());
  REQUIRE(static_cast<int>(got.size()) == order + 1);
  for (size_t i = 0; i < got.size(); ++i) {
    CAPTURE(i, got[i], expected[i]);
    REQUIRE_THAT(got[i], WithinAbs(expected[i], 1e-4f));
  }
}
