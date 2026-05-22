/// @file decompose_test.cpp
/// @brief Reference compatibility tests for effects/decompose.

#include "effects/decompose.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;

namespace {

std::vector<float> flatten_matrix(const JsonValue& m) {
  std::vector<float> out;
  for (const auto& row : m.as_array()) {
    for (const auto& v : row.as_array()) out.push_back(v.as_float());
  }
  return out;
}

}  // namespace

TEST_CASE("decompose reconstructs S within a reasonable tolerance", "[librosa][decompose]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/decompose.json");
  const auto& d = json["data"];
  const int n_features = d["n_features"].as_int();
  const int n_frames = d["n_frames"].as_int();
  auto S = flatten_matrix(d["S"]);

  auto r = decompose(S.data(), n_features, n_frames, /*n_components=*/2,
                     /*n_iter=*/300, "mu", /*beta=*/2.0f, /*init=*/"nndsvd");
  REQUIRE(r.W.size() == static_cast<size_t>(n_features) * 2);
  REQUIRE(r.H.size() == static_cast<size_t>(2) * n_frames);

  // Reconstruct and compare to S. NMF is identifiable only up to permutation
  // and scaling of components, so we cannot compare W or H directly. The
  // librosa reference does the same and we just check the recovered S^.
  double err = 0.0;
  double norm_S = 0.0;
  for (int f = 0; f < n_features; ++f) {
    for (int t = 0; t < n_frames; ++t) {
      float s = 0.0f;
      for (int c = 0; c < 2; ++c) {
        s += r.W[f * 2 + c] * r.H[c * n_frames + t];
      }
      const float diff = S[f * n_frames + t] - s;
      err += static_cast<double>(diff) * diff;
      norm_S += static_cast<double>(S[f * n_frames + t]) * S[f * n_frames + t];
    }
  }
  // Relative Frobenius error should be modest for a 2-component synthetic input.
  REQUIRE(std::sqrt(err / norm_S) < 0.20);
}

TEST_CASE("nn_filter preserves shape and non-negativity", "[librosa][decompose]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/decompose.json");
  const auto& d = json["data"];
  const int n_features = d["n_features"].as_int();
  const int n_frames = d["n_frames"].as_int();
  auto S = flatten_matrix(d["S"]);

  auto out = nn_filter(S.data(), n_features, n_frames, "mean", /*k=*/3, /*width=*/1);
  REQUIRE(out.size() == static_cast<size_t>(n_features) * n_frames);
  for (float v : out) REQUIRE(v >= 0.0f);
}
