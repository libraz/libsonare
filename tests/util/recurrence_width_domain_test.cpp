/// @file recurrence_width_domain_test.cpp
/// @brief Domain tests for recurrence-matrix width handling.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "feature/segment.h"
#include "util/exception.h"

using namespace sonare;

namespace {
void check_close(const std::vector<float>& actual, const std::vector<float>& expected) {
  if (actual.size() != expected.size()) {
    CHECK(actual.size() == expected.size());
    return;
  }
  for (size_t i = 0; i < actual.size(); ++i) {
    const float tolerance = 2.0e-5f * std::max(1.0f, std::abs(expected[i]));
    CAPTURE(i, expected[i], actual[i]);
    CHECK(std::abs(actual[i] - expected[i]) <= tolerance);
  }
}
}  // namespace

TEST_CASE("recurrence affinity handles maximum width without integer overflow",
          "[util][segment][domain]") {
  const std::vector<float> features{0.0f, 1.0f};
  const std::vector<float> empty(4, 0.0f);

  for (const bool sym : {false, true}) {
    for (const int k : {0, 1}) {
      CAPTURE(sym, k);
      CHECK(recurrence_matrix(features.data(), 1, 2, k, std::numeric_limits<int>::max(), sym,
                              "euclidean", "affinity") == empty);
    }
  }
}

TEST_CASE("recurrence width boundaries retain their documented band behavior",
          "[util][segment][domain]") {
  const std::vector<float> features{0.0f, 1.0f, 10.0f, 11.0f};
  const std::vector<float> empty(16, 0.0f);

  for (const std::string mode : {"connectivity", "affinity"}) {
    CAPTURE(mode);
    CHECK(recurrence_matrix(features.data(), 1, 4, 0, 4, false, "euclidean", mode) == empty);
    CHECK(recurrence_matrix(features.data(), 1, 4, 0, 5, true, "euclidean", mode) == empty);
  }
}

TEST_CASE("nonpositive recurrence widths normalize to the default band",
          "[util][segment][domain]") {
  const std::vector<float> features{0.0f, 1.0f, 10.0f, 11.0f};

  for (const std::string mode : {"connectivity", "affinity"}) {
    CAPTURE(mode);
    const auto width_one = recurrence_matrix(features.data(), 1, 4, 2, 1, true, "euclidean", mode);
    check_close(recurrence_matrix(features.data(), 1, 4, 2, 0, true, "euclidean", mode), width_one);
    check_close(recurrence_matrix(features.data(), 1, 4, 2, -1, true, "euclidean", mode),
                width_one);
    check_close(recurrence_matrix(features.data(), 1, 4, 2, std::numeric_limits<int>::min(), true,
                                  "euclidean", mode),
                width_one);
  }
}

TEST_CASE("ordinary recurrence affinity values remain unchanged", "[util][segment][domain]") {
  const std::vector<float> features{0.0f, 1.0f, 10.0f, 11.0f};
  const auto affinity =
      recurrence_matrix(features.data(), 1, 4, 2, 1, false, "euclidean", "affinity");
  REQUIRE(affinity.size() == 16);

  // The median second-neighbour distance is 9.5. This transposed reference
  // topology is pinned independently; derive its weights from the documented
  // Gaussian kernel rather than rounding the expected exponentials by hand.
  // The source rows' top-two indexes are {1, 2}, {0, 2}, {1, 3}, and {1, 2};
  // this lists their transposed distances. -1 marks omitted cells and diagonal.
  const std::vector<float> selected_distances{-1.0f, 1.0f,  -1.0f, -1.0f,   // output row 0
                                              1.0f,  -1.0f, 9.0f,  10.0f,   // output row 1
                                              10.0f, 9.0f,  -1.0f, 1.0f,    // output row 2
                                              -1.0f, -1.0f, 1.0f,  -1.0f};  // output row 3
  std::vector<float> expected(selected_distances.size(), 0.0f);
  for (size_t i = 0; i < selected_distances.size(); ++i) {
    if (selected_distances[i] >= 0.0f) {
      expected[i] = std::exp(-selected_distances[i] / 9.5f);
    }
  }
  check_close(affinity, expected);
}

TEST_CASE("feature matrix dimensions are validated before finite scans",
          "[util][segment][domain]") {
  float dummy = 0.0f;

  CHECK_THROWS_AS(cross_similarity(&dummy, -1, 2, &dummy, -1, 2, 0, "cosine"), SonareException);
  CHECK_THROWS_AS(recurrence_matrix(&dummy, -1, 2, 0, 1, false, "cosine"), SonareException);
  CHECK_THROWS_AS(agglomerative(&dummy, -1, 2, 1), SonareException);

  // The output is only 2x2. The input feature count itself is too large for
  // row * cols int indexing, so this tiny pointer must never be scanned.
  CHECK_THROWS_AS(cross_similarity(&dummy, std::numeric_limits<int>::max(), 2, &dummy,
                                   std::numeric_limits<int>::max(), 2, 0, "cosine"),
                  SonareException);

  float other = 0.0f;
  const int max_rows = std::numeric_limits<int>::max();
  CHECK_THROWS_AS(recurrence_matrix(&dummy, max_rows, 2), SonareException);
  CHECK_THROWS_AS(agglomerative(&dummy, max_rows, 2, 1), SonareException);
  CHECK_THROWS_AS(subsegment(&dummy, max_rows, 2, std::vector<int>{0, 2}, 1), SonareException);
  CHECK_THROWS_AS(cross_similarity(&dummy, max_rows, 2, &dummy, max_rows, 1, 0, "cosine"),
                  SonareException);
  CHECK_THROWS_AS(cross_similarity(&dummy, max_rows, 2, &other, max_rows, 1, 0, "cosine"),
                  SonareException);
  CHECK_THROWS_AS(cross_similarity(&dummy, max_rows, 1, &dummy, max_rows, 2, 0, "cosine"),
                  SonareException);
  CHECK_THROWS_AS(cross_similarity(&dummy, max_rows, 1, &other, max_rows, 2, 0, "cosine"),
                  SonareException);
}

TEST_CASE("aliased cross-similarity validates both feature counts before scanning",
          "[util][segment][domain]") {
  float dummy = 0.0f;

  // X's feature product is representable; Y's is not. The pointers alias, so
  // validating Y only after scanning X would read past this one-float buffer.
  CHECK_THROWS_AS(cross_similarity(&dummy, 46341, 1, &dummy, 46341, 46341, 0, "cosine"),
                  SonareException);

  float aliased[4]{0.0f, 1.0f, 2.0f, std::numeric_limits<float>::quiet_NaN()};
  CHECK_THROWS_AS(cross_similarity(aliased, 2, 1, aliased, 2, 2, 0, "cosine"), SonareException);
}

TEST_CASE("zero-row feature matrices preserve their existing behavior", "[util][segment][domain]") {
  float dummy = 0.0f;
  const auto cross = cross_similarity(&dummy, 0, 2, &dummy, 0, 3, 0, "cosine");
  REQUIRE(cross.size() == 6);
  CHECK(std::all_of(cross.begin(), cross.end(), [](float value) { return value == 0.0f; }));

  const auto recurrence = recurrence_matrix(&dummy, 0, 3, 0, 1, false, "cosine");
  REQUIRE(recurrence.size() == 9);
  CHECK(
      std::all_of(recurrence.begin(), recurrence.end(), [](float value) { return value == 0.0f; }));

  const std::vector<int> labels{0, 1, 2};
  CHECK(agglomerative(&dummy, 0, 3, 3) == labels);

  const std::vector<int> boundaries{0, 3};
  CHECK(subsegment(&dummy, 0, 3, boundaries, 2) == boundaries);
  CHECK(subsegment(&dummy, -1, 3, boundaries, 2) == boundaries);
}

TEST_CASE("empty columns return before feature-shape validation", "[util][segment][domain]") {
  float dummy = 0.0f;
  const std::vector<int> boundaries{0, 3};

  CHECK(cross_similarity(&dummy, -1, 0, &dummy, -1, 2, 0, "cosine").empty());
  CHECK(recurrence_matrix(&dummy, -1, 0, 0, 1, false, "cosine").empty());
  CHECK(agglomerative(&dummy, -1, 0, 1).empty());
  CHECK(subsegment(&dummy, 1, 0, boundaries, 2) == boundaries);
}

TEST_CASE("zero-row distinct-input affinity retains metric defaults", "[util][segment][domain]") {
  float x = 0.0f;
  float y = 0.0f;
  const auto cosine = cross_similarity(&x, 0, 1, &y, 0, 1, 0, "cosine", "affinity");
  const auto euclidean = cross_similarity(&x, 0, 1, &y, 0, 1, 0, "euclidean", "affinity");

  check_close(cosine, std::vector<float>{std::exp(-1.0f)});
  check_close(euclidean, std::vector<float>{1.0f});
}

TEST_CASE("two-column recurrence affinity keeps zero diagonal and expected neighbours",
          "[util][segment][domain]") {
  const std::vector<float> features{0.0f, 2.0f};
  const auto recurrence =
      recurrence_matrix(features.data(), 1, 2, 0, 1, false, "euclidean", "affinity");
  const float neighbour_affinity = std::exp(-1.0f);
  check_close(recurrence, std::vector<float>{0.0f, neighbour_affinity, neighbour_affinity, 0.0f});

  float dummy = 0.0f;
  check_close(recurrence_matrix(&dummy, 0, 2, 0, 1, false, "cosine", "affinity"),
              std::vector<float>{0.0f, neighbour_affinity, neighbour_affinity, 0.0f});
  check_close(recurrence_matrix(&dummy, 0, 2, 0, 1, false, "euclidean", "affinity"),
              std::vector<float>{0.0f, 1.0f, 1.0f, 0.0f});
}
