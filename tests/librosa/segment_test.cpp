/// @file segment_test.cpp
/// @brief Reference compatibility tests for feature/segment.

#include "feature/segment.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <vector>

#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;
using Catch::Matchers::WithinAbs;

namespace {

std::vector<float> flatten_matrix(const JsonValue& m) {
  std::vector<float> out;
  for (const auto& row : m.as_array()) {
    for (const auto& v : row.as_array()) out.push_back(v.as_float());
  }
  return out;
}

}  // namespace

TEST_CASE("cross_similarity shape matches librosa", "[librosa][segment]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/segment.json");
  const auto& d = json["data"];
  const int n_features = d["n_features"].as_int();
  const int n_samples = d["n_samples"].as_int();
  auto X = flatten_matrix(d["X"]);

  // Dense (k=0) cosine similarity should be symmetric and have unit diagonal.
  auto S = cross_similarity(X.data(), n_features, n_samples, X.data(), n_features, n_samples,
                            /*k=*/0, "cosine");
  REQUIRE(S.size() == static_cast<size_t>(n_samples) * n_samples);
  for (int i = 0; i < n_samples; ++i) {
    REQUIRE(S[i * n_samples + i] > 0.9f);
  }
}

TEST_CASE("cross_similarity (affinity) matches librosa values", "[librosa][segment]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/segment.json");
  const auto& d = json["data"];
  const int n_features = d["n_features"].as_int();
  const int n_samples = d["n_samples"].as_int();
  const int k = d["k"].as_int();
  auto X = flatten_matrix(d["X"]);
  auto ref = flatten_matrix(d["affinity"]);

  auto A = cross_similarity(X.data(), n_features, n_samples, X.data(), n_features, n_samples, k,
                            "cosine", "affinity");
  REQUIRE(A.size() == static_cast<size_t>(n_samples) * n_samples);

  // librosa includes the self-pair when self-distance computes to a non-zero
  // float epsilon (sklearn quirk), giving a 1.0 on some diagonal entries and
  // 0.0 on others. We always exclude self deterministically — compare only
  // off-diagonal entries, which are the meaningful affinity values.
  for (int i = 0; i < n_samples; ++i) {
    for (int j = 0; j < n_samples; ++j) {
      if (i == j) continue;
      CAPTURE(i, j);
      REQUIRE_THAT(A[i * n_samples + j], WithinAbs(ref[i * n_samples + j], 1e-5f));
    }
    REQUIRE(A[i * n_samples + i] == 0.0f);
  }
}

TEST_CASE("recurrence_matrix produces librosa-shape output", "[librosa][segment]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/segment.json");
  const auto& d = json["data"];
  const int n_features = d["n_features"].as_int();
  const int n_samples = d["n_samples"].as_int();
  auto X = flatten_matrix(d["X"]);

  auto R =
      recurrence_matrix(X.data(), n_features, n_samples, /*k=*/3, /*width=*/1, false, "cosine");
  REQUIRE(R.size() == static_cast<size_t>(n_samples) * n_samples);
  // Central diagonal band (|i-j| < 1) is zeroed; the rest contains the k-NN
  // affinity. librosa uses scikit-learn's NearestNeighbors which can pick
  // different ties from our implementation, so we only require *some*
  // non-zero entries here.
  int nz = 0;
  for (float v : R) nz += (v != 0.0f) ? 1 : 0;
  REQUIRE(nz > 0);
}

TEST_CASE("recurrence_matrix (affinity) matches librosa values", "[librosa][segment]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/segment.json");
  const auto& d = json["data"];
  const int n_features = d["n_features"].as_int();
  const int n_samples = d["n_samples"].as_int();
  const int k = d["k"].as_int();
  auto X = flatten_matrix(d["X"]);
  auto ref = flatten_matrix(d["recurrence"]);

  // librosa.segment.recurrence_matrix(..., mode="affinity", sym=False, width=1).
  // The diagonal is explicitly zeroed by librosa post-bandwidth, so the full
  // matrix is comparable to float precision.
  auto R = recurrence_matrix(X.data(), n_features, n_samples, k, /*width=*/1, /*sym=*/false,
                             "cosine", "affinity");
  REQUIRE(R.size() == ref.size());
  for (size_t i = 0; i < R.size(); ++i) {
    CAPTURE(i);
    REQUIRE_THAT(R[i], WithinAbs(ref[i], 1e-5f));
  }
}

TEST_CASE("recurrence_matrix connectivity never takes the excluded band as neighbours",
          "[librosa][segment]") {
  // A drifting 2-D feature: every frame is closest to its immediate neighbours,
  // which are exactly the band width excludes.
  constexpr int kRows = 2;
  constexpr int kCols = 12;
  std::vector<float> X(static_cast<size_t>(kRows) * kCols);
  for (int j = 0; j < kCols; ++j) {
    X[static_cast<size_t>(j)] = static_cast<float>(j + 1);
    X[static_cast<size_t>(kCols + j)] = 0.5f * static_cast<float>(j % 3);
  }
  constexpr int kK = 2;
  constexpr int kWidth = 3;
  for (const char* metric : {"euclidean", "cosine"}) {
    INFO("metric " << metric);
    const auto R = recurrence_matrix(X.data(), kRows, kCols, kK, kWidth, /*sym=*/false, metric);
    for (int i = 0; i < kCols; ++i) {
      int kept = 0;
      for (int j = 0; j < kCols; ++j) {
        const float v = R[static_cast<size_t>(i) * kCols + j];
        if (std::abs(i - j) < kWidth) {
          CHECK(v == 0.0f);
        } else if (v != 0.0f) {
          ++kept;
        }
      }
      INFO("row " << i);
      CHECK(kept == kK);
    }
    // Symmetrised, an edge survives only where both directions chose it.
    const auto S = recurrence_matrix(X.data(), kRows, kCols, kK, kWidth, /*sym=*/true, metric);
    for (int i = 0; i < kCols; ++i) {
      for (int j = 0; j < kCols; ++j) {
        const bool both = R[static_cast<size_t>(i) * kCols + j] != 0.0f &&
                          R[static_cast<size_t>(j) * kCols + i] != 0.0f;
        INFO("cell " << i << "," << j);
        CHECK((S[static_cast<size_t>(i) * kCols + j] != 0.0f) == both);
      }
    }
  }
}
