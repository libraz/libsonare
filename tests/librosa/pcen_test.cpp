/// @file pcen_test.cpp
/// @brief librosa parity test for PCEN.
/// @details Reference: tests/librosa/reference/pcen.json

#include "core/pcen.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "util/exception.h"
#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;

TEST_CASE("pcen matches librosa reference", "[librosa][pcen]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/pcen.json");
  const auto& d = json["data"];

  int sr = d["sr"].as_int();
  int hop_length = d["hop_length"].as_int();
  int n_bins = d["n_bins"].as_int();
  int n_frames = d["n_frames"].as_int();
  float time_constant = d["time_constant"].as_float();
  float gain = d["gain"].as_float();
  float bias = d["bias"].as_float();
  float power = d["power"].as_float();
  float eps = d["eps"].as_float();

  const auto& s_arr = d["S_flat"].as_array();
  const auto& exp_arr = d["expected_flat"].as_array();
  REQUIRE(s_arr.size() == static_cast<size_t>(n_bins * n_frames));
  REQUIRE(exp_arr.size() == s_arr.size());

  std::vector<float> S;
  S.reserve(s_arr.size());
  for (const auto& v : s_arr) S.push_back(v.as_float());

  PcenConfig cfg;
  cfg.sr = sr;
  cfg.hop_length = hop_length;
  cfg.time_constant = time_constant;
  cfg.gain = gain;
  cfg.bias = bias;
  cfg.power = power;
  cfg.eps = eps;

  auto got = pcen(S.data(), n_bins, n_frames, cfg);
  REQUIRE(got.size() == exp_arr.size());

  double max_abs_diff = 0.0;
  for (size_t i = 0; i < got.size(); ++i) {
    float e = exp_arr[i].as_float();
    double diff = std::abs(static_cast<double>(got[i]) - static_cast<double>(e));
    max_abs_diff = std::max(max_abs_diff, diff);
  }
  CAPTURE(max_abs_diff);
  REQUIRE(max_abs_diff < 1e-3);
}

TEST_CASE("pcen rejects malformed and non-finite configuration", "[librosa][pcen][edge]") {
  const std::vector<float> input{1.0f};
  PcenConfig cfg;

  SECTION("b must contain at most one coefficient") {
    cfg.b = {0.5f, 0.5f};
    REQUIRE_THROWS_AS(pcen(input.data(), 1, 1, cfg), SonareException);
  }

  SECTION("b and zi values must be finite") {
    cfg.b = {std::numeric_limits<float>::quiet_NaN()};
    REQUIRE_THROWS_AS(pcen(input.data(), 1, 1, cfg), SonareException);

    cfg.b.clear();
    cfg.zi = {std::numeric_limits<float>::infinity()};
    REQUIRE_THROWS_AS(pcen(input.data(), 1, 1, cfg), SonareException);
  }

  SECTION("scalar parameters must be finite") {
    cfg.gain = std::numeric_limits<float>::infinity();
    REQUIRE_THROWS_AS(pcen(input.data(), 1, 1, cfg), SonareException);
    cfg.gain = 0.98f;
    cfg.time_constant = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_THROWS_AS(pcen(input.data(), 1, 1, cfg), SonareException);
  }

  SECTION("derived smoothing coefficient must be finite") {
    cfg.time_constant = std::numeric_limits<float>::denorm_min();
    REQUIRE_THROWS_AS(pcen(input.data(), 1, 1, cfg), SonareException);
  }
}

namespace {

/// @brief Deterministic strictly-positive [n_bins x n_frames] magnitude, row-major.
/// @details Bins are given distinct levels so a delay state leaking from one bin into the
///          next is visible rather than absorbed into a common scale.
std::vector<float> pcen_fixture(int n_bins, int n_frames, uint32_t seed) {
  std::vector<float> S(static_cast<std::size_t>(n_bins) * n_frames, 0.0f);
  uint32_t state = seed;
  for (int k = 0; k < n_bins; ++k) {
    const float level = 0.1f + 0.9f * static_cast<float>(k % 5);
    for (int t = 0; t < n_frames; ++t) {
      state = state * 1664525u + 1013904223u;
      const float unit = static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
      S[static_cast<std::size_t>(k) * n_frames + t] = 0.01f + unit * level;
    }
  }
  return S;
}

/// @brief pcen() with the AR(1) recursion in its original frame-major traversal.
/// @details The outer loop runs over frames and carries one delay state per bin, so this
///          pins both the summation-free arithmetic and the direction the recursion runs
///          in: reversing t inside a bin would be a different filter, not a last-bit move.
///          `b` is taken from the config rather than derived, so the caller must set it.
std::vector<float> oracle_pcen(const float* S, int n_bins, int n_frames, const PcenConfig& config) {
  const float b = config.b[0];
  std::vector<float> d(static_cast<std::size_t>(n_bins), 1.0f - b);
  for (int k = 0; k < static_cast<int>(config.zi.size()); ++k) {
    d[static_cast<std::size_t>(k)] = config.zi[static_cast<std::size_t>(k)];
  }
  std::vector<float> out(static_cast<std::size_t>(n_bins) * n_frames);
  for (int t = 0; t < n_frames; ++t) {
    for (int k = 0; k < n_bins; ++k) {
      const float s = S[static_cast<std::size_t>(k) * n_frames + t];
      const float y = b * s + d[static_cast<std::size_t>(k)];
      d[static_cast<std::size_t>(k)] = (1.0f - b) * y;
      const float smooth = std::pow(y + config.eps, -config.gain);
      const float compressed = config.power == 0.0f
                                   ? std::log1p(s * smooth)
                                   : std::pow(s * smooth + config.bias, config.power) -
                                         std::pow(config.bias, config.power);
      out[static_cast<std::size_t>(k) * n_frames + t] = compressed;
    }
  }
  return out;
}

void require_bit_equal(const std::vector<float>& got, const std::vector<float>& want) {
  REQUIRE(got.size() == want.size());
  for (std::size_t i = 0; i < got.size(); ++i) {
    CAPTURE(i, got[i], want[i]);
    REQUIRE(std::isfinite(got[i]));
    REQUIRE(got[i] == want[i]);
  }
}

}  // namespace

TEST_CASE("pcen matches a frame-major oracle", "[librosa][pcen]") {
  const int n_bins = 17;
  const int n_frames = 43;
  const std::vector<float> S = pcen_fixture(n_bins, n_frames, 0x5eedu);

  PcenConfig cfg;
  cfg.b = {0.03f};  // explicit, so the oracle needs no copy of the derivation
  // A distinct initial delay state per bin. A scalar state that is not re-seeded at each
  // bin carries bin k-1's tail into bin k, which a uniform zi would hide.
  cfg.zi.resize(static_cast<std::size_t>(n_bins));
  for (int k = 0; k < n_bins; ++k) {
    cfg.zi[static_cast<std::size_t>(k)] = 0.9f + 0.005f * static_cast<float>(k);
  }

  // power 0 takes the log1p branch; a positive power takes the power law and the bias term.
  for (float power : {0.5f, 0.0f, 1.0f}) {
    CAPTURE(power);
    cfg.power = power;
    require_bit_equal(pcen(S.data(), n_bins, n_frames, cfg),
                      oracle_pcen(S.data(), n_bins, n_frames, cfg));
  }
}

TEST_CASE("pcen matches the oracle on single-row and single-column inputs", "[librosa][pcen]") {
  PcenConfig cfg;
  cfg.b = {0.03f};

  SECTION("one frame") {
    // The recursion has no direction to get wrong here, so this isolates the per-bin seed.
    const int n_bins = 17;
    const std::vector<float> S = pcen_fixture(n_bins, 1, 0xc0ffeeu);
    cfg.zi.assign(static_cast<std::size_t>(n_bins), 0.0f);
    for (int k = 0; k < n_bins; ++k) {
      cfg.zi[static_cast<std::size_t>(k)] = 0.9f + 0.005f * static_cast<float>(k);
    }
    require_bit_equal(pcen(S.data(), n_bins, 1, cfg), oracle_pcen(S.data(), n_bins, 1, cfg));
  }

  SECTION("one bin") {
    // One chain over every frame: this is the case the recursion direction alone decides.
    const int n_frames = 43;
    const std::vector<float> S = pcen_fixture(1, n_frames, 0xb0bau);
    cfg.zi = {0.97f};
    require_bit_equal(pcen(S.data(), 1, n_frames, cfg), oracle_pcen(S.data(), 1, n_frames, cfg));
  }
}
