/// @file decompose_test.cpp
/// @brief Reference compatibility tests for effects/decompose.

#include "effects/decompose.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "support/alloc_guard.h"
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
  auto ref_recon = flatten_matrix(d["reconstruction"]);

  auto r = decompose(S.data(), n_features, n_frames, /*n_components=*/2,
                     /*n_iter=*/500, "mu", /*beta=*/2.0f, /*init=*/"nndsvd");
  REQUIRE(r.W.size() == static_cast<size_t>(n_features) * 2);
  REQUIRE(r.H.size() == static_cast<size_t>(2) * n_frames);

  // Reconstruct W·H and compare to both the input S and librosa's reconstruction.
  // NMF is identifiable only up to permutation/scaling of components, so direct
  // W/H comparison is not meaningful — but the W·H product converges to a
  // similar low-rank approximation regardless of initialisation.
  double err_S = 0.0;
  double norm_S = 0.0;
  double err_ref = 0.0;
  double norm_ref = 0.0;
  for (int f = 0; f < n_features; ++f) {
    for (int t = 0; t < n_frames; ++t) {
      float s = 0.0f;
      for (int c = 0; c < 2; ++c) {
        s += r.W[f * 2 + c] * r.H[c * n_frames + t];
      }
      const double dS = static_cast<double>(S[f * n_frames + t]) - s;
      err_S += dS * dS;
      norm_S += static_cast<double>(S[f * n_frames + t]) * S[f * n_frames + t];
      const double dR = static_cast<double>(ref_recon[f * n_frames + t]) - s;
      err_ref += dR * dR;
      norm_ref += static_cast<double>(ref_recon[f * n_frames + t]) * ref_recon[f * n_frames + t];
    }
  }
  const double rel_err_S = std::sqrt(err_S / norm_S);
  const double rel_err_ref = std::sqrt(err_ref / norm_ref);
  CAPTURE(rel_err_S, rel_err_ref);
  REQUIRE(rel_err_S < 0.20);
  // Our NNDSVD-initialised MU NMF should converge close to librosa/sklearn's
  // solution for this 2-component synthetic input.
  REQUIRE(rel_err_ref < 0.10);
}

TEST_CASE("nn_filter matches librosa mean output", "[librosa][decompose]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/decompose.json");
  const auto& d = json["data"];
  const int n_features = d["n_features"].as_int();
  const int n_frames = d["n_frames"].as_int();
  auto S = flatten_matrix(d["S"]);
  auto ref = flatten_matrix(d["nn_filter_mean"]);

  auto out = nn_filter(S.data(), n_features, n_frames, "mean", /*k=*/3, /*width=*/1);
  REQUIRE(out.size() == ref.size());

  double err = 0.0;
  double norm = 0.0;
  for (size_t i = 0; i < out.size(); ++i) {
    REQUIRE(out[i] >= 0.0f);
    const double dd = static_cast<double>(out[i]) - ref[i];
    err += dd * dd;
    norm += static_cast<double>(ref[i]) * ref[i];
  }
  const double rel_err = std::sqrt(err / std::max(norm, 1e-12));
  CAPTURE(rel_err);
  // nn_filter is deterministic: given the same k-NN selection it should match
  // librosa to float precision.
  REQUIRE(rel_err < 1e-5);
}

TEST_CASE("nn_filter allocates one selector list per frame and nothing else per frame",
          "[librosa][decompose][nn-filter-workspace]") {
  constexpr int kFeatures = 6;
  constexpr int kFrames = 200;
  std::vector<float> S(static_cast<size_t>(kFeatures) * kFrames);
  for (size_t i = 0; i < S.size(); ++i)
    S[i] = 1.0f + 0.5f * std::sin(0.37f * static_cast<float>(i));

  sonare::test::AllocationGuard guard;
  const auto out = nn_filter(S.data(), kFeatures, kFrames, "mean", /*k=*/3, /*width=*/1);
  const size_t allocations = guard.count();

  REQUIRE(out.size() == S.size());
  // One list per frame plus a fixed handful of buffers; a per-frame scratch vector would double it.
  REQUIRE(allocations < static_cast<size_t>(kFrames) + 32);
}
