/// @file nnls_test.cpp
/// @brief Unit tests for util/nnls.

#include "util/nnls.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;

TEST_CASE("nnls solves a simple non-negative system", "[util][nnls]") {
  // A = [[1, 0], [0, 1], [1, 1]], B = [[1], [2], [3]]
  // Unconstrained solution: x = [1, 2] which is non-negative -> matches LS.
  std::vector<float> A{1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
  std::vector<float> B{1.0f, 2.0f, 3.0f};
  auto X = nnls(A.data(), 3, 2, B.data(), 1);
  REQUIRE(X.size() == 2);
  REQUIRE_THAT(X[0], WithinAbs(1.0f, 1e-4f));
  REQUIRE_THAT(X[1], WithinAbs(2.0f, 1e-4f));
}

TEST_CASE("nnls returns non-negative solution when LS would be negative", "[util][nnls]") {
  // A = [[1, -1]], B = [[0]]. Unconstrained: any x with x0 = x1. NNLS returns 0.
  std::vector<float> A{1.0f, -1.0f};
  std::vector<float> B{-1.0f};
  auto X = nnls(A.data(), 1, 2, B.data(), 1);
  REQUIRE(X.size() == 2);
  REQUIRE(X[0] >= 0.0f);
  REQUIRE(X[1] >= 0.0f);
}

TEST_CASE("nnls handles multiple columns of B", "[util][nnls]") {
  std::vector<float> A{1.0f, 0.0f, 0.0f, 1.0f};
  std::vector<float> B{1.0f, 2.0f, 3.0f, 4.0f};  // 2 columns: [1,3] and [2,4].
  auto X = nnls(A.data(), 2, 2, B.data(), 2);
  REQUIRE(X.size() == 4);
  REQUIRE_THAT(X[0 * 2 + 0], WithinAbs(1.0f, 1e-4f));
  REQUIRE_THAT(X[0 * 2 + 1], WithinAbs(2.0f, 1e-4f));
  REQUIRE_THAT(X[1 * 2 + 0], WithinAbs(3.0f, 1e-4f));
  REQUIRE_THAT(X[1 * 2 + 1], WithinAbs(4.0f, 1e-4f));
}
