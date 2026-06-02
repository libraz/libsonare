/// @file segment_test.cpp
/// @brief Unit tests for feature/segment primitives.

#include "feature/segment.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace sonare;

namespace {
std::vector<float> identity_features(int rows, int cols) {
  // rows features, cols samples. Set bit r in sample (r % cols) to 1.
  std::vector<float> X(static_cast<size_t>(rows) * cols, 0.0f);
  for (int r = 0; r < rows && r < cols; ++r) X[r * cols + r] = 1.0f;
  return X;
}
}  // namespace

TEST_CASE("cross_similarity self-cosine has diagonal of 1", "[util][segment]") {
  auto X = identity_features(3, 3);
  auto S = cross_similarity(X.data(), 3, 3, X.data(), 3, 3, 0, "cosine");
  REQUIRE(S.size() == 9);
  for (int i = 0; i < 3; ++i) {
    REQUIRE(S[i * 3 + i] > 0.9f);
  }
}

TEST_CASE("recurrence_matrix excludes central diagonal band", "[util][segment]") {
  auto X = identity_features(3, 3);
  auto R = recurrence_matrix(X.data(), 3, 3, 0, /*width=*/1, false, "cosine");
  REQUIRE(R.size() == 9);
  for (int i = 0; i < 3; ++i) {
    REQUIRE(R[i * 3 + i] == 0.0f);
  }
}

TEST_CASE("recurrence_to_lag / lag_to_recurrence shapes", "[util][segment]") {
  std::vector<float> R{
      1.0f, 0.5f, 0.0f, 0.0f, 1.0f, 0.5f, 0.5f, 0.0f, 1.0f,
  };
  auto lag = recurrence_to_lag(R.data(), 3, false);
  REQUIRE(lag.size() == 9);
  auto roundtrip = lag_to_recurrence(lag.data(), 3, 3);
  REQUIRE(roundtrip.size() == 9);
}

TEST_CASE("subsegment refines boundaries", "[util][segment]") {
  std::vector<float> X(10, 0.0f);
  std::vector<int> bounds{0, 10};
  auto out = subsegment(X.data(), 1, 10, bounds, 4);
  REQUIRE(out.size() >= bounds.size());
}

TEST_CASE("subsegment splits on feature content not fixed width", "[util][segment]") {
  // Two clips with identical parent boundaries but different content must yield
  // different interior boundaries placed at the content transition — clustering
  // splits where the feature vectors change. (Previously subsegment ignored the
  // data and always emitted fixed equal-width chunks, identical for both.)
  const std::vector<int> bounds{0, 8};

  std::vector<float> mid_change{0.0f, 0.0f, 0.0f, 0.0f, 5.0f, 5.0f, 5.0f, 5.0f};
  auto a = subsegment(mid_change.data(), 1, 8, bounds, 2);
  REQUIRE(std::find(a.begin(), a.end(), 4) != a.end());

  std::vector<float> early_change{0.0f, 0.0f, 5.0f, 5.0f, 5.0f, 5.0f, 5.0f, 5.0f};
  auto b = subsegment(early_change.data(), 1, 8, bounds, 2);
  REQUIRE(std::find(b.begin(), b.end(), 2) != b.end());

  // Content-driven: the split positions differ for the two clips.
  REQUIRE(a != b);
}

TEST_CASE("agglomerative returns valid label range", "[util][segment]") {
  auto X = identity_features(4, 6);
  auto labels = agglomerative(X.data(), 4, 6, 2);
  REQUIRE(labels.size() == 6);
  int max_label = 0;
  for (int l : labels) max_label = std::max(max_label, l);
  REQUIRE(max_label < 2);
}

TEST_CASE("path_enhance preserves shape", "[util][segment]") {
  std::vector<float> R(9, 1.0f);
  auto enhanced = path_enhance(R.data(), 3, 3);
  REQUIRE(enhanced.size() == 9);
}
