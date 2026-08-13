/// @file segment_test.cpp
/// @brief Unit tests for feature/segment primitives.

#include "feature/segment.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "util/exception.h"

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

TEST_CASE("segment matrix builders reject overflowing dimensions", "[util][segment]") {
  // cross_similarity / recurrence_matrix index their output with int expressions
  // (`i * cols + j`); a column count whose squared element count exceeds INT_MAX
  // is rejected before allocation or any pointer read, so the dummy pointer is
  // never dereferenced.
  float dummy = 0.0f;
  REQUIRE_THROWS_AS(
      cross_similarity(&dummy, 1, 50000, &dummy, 1, 50000, 0, "cosine", "connectivity"),
      SonareException);
  REQUIRE_THROWS_AS(recurrence_matrix(&dummy, 1, 50000, 0, 1, false, "cosine", "connectivity"),
                    SonareException);
  REQUIRE_THROWS_AS(recurrence_to_lag(&dummy, 50000, true), SonareException);
}

TEST_CASE("subsegment emits at most n_segments boundaries per parent span",
          "[util][segment][subsegment]") {
  // librosa cuts each parent span into exactly min(n_segments, len) runs. Under
  // unconstrained clustering a label can recur after an interruption, so the
  // count of label transitions -- and therefore the emitted boundary count --
  // is unbounded by n_segments. This feature pattern alternates between two
  // values, which is precisely the case that produces a transition at nearly
  // every column when contiguity is not enforced.
  const int cols = 24;
  const int rows = 1;
  std::vector<float> alternating(static_cast<size_t>(cols));
  for (int c = 0; c < cols; ++c) {
    alternating[static_cast<size_t>(c)] = (c % 2 == 0) ? 0.0f : 5.0f;
  }

  const std::vector<int> parents{0, 12, cols};
  const int n_segments = 3;
  const auto out = subsegment(alternating.data(), rows, cols, parents, n_segments);

  // Endpoints plus at most (n_segments - 1) interior cuts per parent span.
  REQUIRE(out.front() == 0);
  REQUIRE(out.back() == cols);
  REQUIRE(std::is_sorted(out.begin(), out.end()));
  REQUIRE(std::adjacent_find(out.begin(), out.end()) == out.end());

  for (size_t p = 0; p + 1 < parents.size(); ++p) {
    const int a = parents[p];
    const int b = parents[p + 1];
    const auto in_span =
        std::count_if(out.begin(), out.end(), [&](int f) { return f >= a && f < b; });
    CAPTURE(a, b, in_span, n_segments, out.size());
    REQUIRE(in_span <= n_segments);
    REQUIRE(in_span >= 1);  // the parent boundary itself is always retained
  }
}

TEST_CASE("subsegment splits at the true plateau edges", "[util][segment][subsegment]") {
  // Three plateaus of unequal width inside one parent span: the contiguous Ward
  // merge must recover exactly their edges rather than equal-width thirds.
  // (This pins split placement. The bound on how MANY boundaries a span may
  // emit is the separate case above -- an unconstrained clustering passes this
  // one, because these plateaus happen to cluster contiguously anyway.)
  const int cols = 12;
  std::vector<float> plateaus{0.0f, 0.0f, 0.0f, 9.0f, 9.0f, 9.0f,
                              9.0f, 9.0f, 0.5f, 0.5f, 0.5f, 0.5f};
  REQUIRE(plateaus.size() == static_cast<size_t>(cols));

  const std::vector<int> parents{0, cols};
  const auto out = subsegment(plateaus.data(), 1, cols, parents, 3);

  // Exactly the two interior plateau edges, plus the two endpoints.
  const std::vector<int> expected{0, 3, 8, cols};
  CAPTURE(out.size());
  REQUIRE(out == expected);
}
