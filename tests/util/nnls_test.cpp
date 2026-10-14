/// @file nnls_test.cpp
/// @brief Unit tests for util/nnls.

#include "util/nnls.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <limits>
#include <string>
#include <vector>

#include "util/exception.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;

namespace {
template <typename Fn>
bool throws_invalid_parameter_with_message(Fn&& fn, const char* expected_message) {
  try {
    fn();
  } catch (const SonareException& error) {
    return error.code() == ErrorCode::InvalidParameter &&
           std::string(error.what()).find(expected_message) != std::string::npos;
  } catch (...) {
  }
  return false;
}
}  // namespace

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

TEST_CASE("nnls handles a dominant eigenspace orthogonal to the all-ones vector", "[util][nnls]") {
  // A^T A for [1, -1] annihilates the all-ones vector. The NNLS
  // solution for B=[1] still has to reach the non-negative residual-zero line.
  const std::vector<float> A{1.0f, -1.0f};
  const std::vector<float> B{1.0f};
  const auto X = nnls(A, 1, 2, B, 1, /*max_iter=*/200, /*tol=*/1e-6f);
  REQUIRE(X.size() == 2);
  REQUIRE(X[0] >= 0.0f);
  REQUIRE(X[1] >= 0.0f);
  REQUIRE_THAT(A[0] * X[0] + A[1] * X[1] - B[0], WithinAbs(0.0f, 1e-3f));
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

TEST_CASE("nnls vector overload requires exact matrix sizes", "[util][nnls][edge]") {
  REQUIRE_THROWS_AS(nnls(std::vector<float>{1.0f}, 2, 2, std::vector<float>{1.0f}, 1),
                    SonareException);
  REQUIRE_THROWS_AS(nnls(std::vector<float>{1.0f, 0.0f, 0.0f, 1.0f, 2.0f}, 2, 2,
                         std::vector<float>{1.0f, 2.0f}, 1),
                    SonareException);
  REQUIRE_THROWS_AS(nnls(std::vector<float>{1.0f, 0.0f, 0.0f, 1.0f}, 2, 2,
                         std::vector<float>{1.0f, 2.0f, 3.0f}, 1),
                    SonareException);
  REQUIRE_THROWS_AS(
      nnls(std::vector<float>{1.0f, 0.0f, 0.0f, 1.0f}, 2, 2, std::vector<float>{1.0f}, 1),
      SonareException);
  REQUIRE(throws_invalid_parameter_with_message(
      [&] {
        nnls(std::vector<float>{}, std::numeric_limits<int>::max(), 2, std::vector<float>{}, 1);
      },
      "nnls: A and B sizes must match"));
}
