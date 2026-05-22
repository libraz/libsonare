/// @file nnls_test.cpp
/// @brief Reference compatibility tests for util/nnls.

#include "util/nnls.h"

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

void check_nnls_case(const JsonValue& case_data, float tol) {
  const int A_rows = case_data["A_rows"].as_int();
  const int A_cols = case_data["A_cols"].as_int();
  const int B_cols = case_data["B_cols"].as_int();
  auto A = flatten_matrix(case_data["A"]);
  auto B = flatten_matrix(case_data["B"]);
  auto expected = flatten_matrix(case_data["X"]);

  auto got = nnls(A.data(), A_rows, A_cols, B.data(), B_cols, /*max_iter=*/200, /*tol=*/1e-6f);
  REQUIRE(got.size() == expected.size());
  for (size_t i = 0; i < got.size(); ++i) {
    CAPTURE(i, got[i], expected[i]);
    REQUIRE_THAT(got[i], WithinAbs(expected[i], tol));
  }
}

}  // namespace

TEST_CASE("nnls (overdetermined, non-negative LS) matches scipy", "[librosa][nnls]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/nnls.json");
  check_nnls_case(json["data"]["case_a"], 1e-3f);
}

TEST_CASE("nnls (active-set constrained) matches scipy", "[librosa][nnls]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/nnls.json");
  // Active-set NNLS with a perturbed RHS: looser tolerance because exact
  // active-set selection can differ at numerical boundaries.
  check_nnls_case(json["data"]["case_b"], 5e-3f);
}
