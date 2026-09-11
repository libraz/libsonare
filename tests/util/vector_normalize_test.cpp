/// @file vector_normalize_test.cpp
/// @brief Unit + librosa parity tests for util/vector_normalize.

#include "util/vector_normalize.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "util/exception.h"
#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;
using Catch::Matchers::WithinAbs;

namespace {

/// @brief Deterministic row-major [rows x cols] matrix with one dominant term per column.
/// @details The dominant term pushes the remaining terms off the end of a float accumulator
///          while a wider one still carries them, which is what makes the norm's own
///          accumulator width observable. Columns 0 and `cols - 1` are zero and column 1 sits
///          under the epsilon floor, so the branch that leaves a column alone is reached in the
///          first and the last column tile rather than only in the first.
std::vector<float> matrix_fixture(int rows, int cols, std::uint32_t seed) {
  std::vector<float> m(static_cast<std::size_t>(rows) * cols, 0.0f);
  std::uint32_t state = seed;
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      state = state * 1664525u + 1013904223u;
      const float unit = static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
      float v = std::ldexp(unit - 0.5f, static_cast<int>(state % 40u) - 20);
      if (r == 0) v = std::ldexp(1.0f + unit, 20);
      if (c == 0 || c == cols - 1) v = 0.0f;
      if (c == 1) v = 1e-12f * (unit + 0.5f);
      m[static_cast<std::size_t>(r) * cols + c] = v;
    }
  }
  return m;
}

/// @brief Column `c` of a row-major matrix, normalized through the single-vector entry point.
std::vector<float> normalized_column(const std::vector<float>& m, int rows, int cols, int c,
                                     NormType norm) {
  std::vector<float> column(static_cast<std::size_t>(rows), 0.0f);
  for (int r = 0; r < rows; ++r) column[r] = m[static_cast<std::size_t>(r) * cols + c];
  return normalize(column, norm);
}

}  // namespace

TEST_CASE("normalize Inf norm puts peak at 1", "[util][normalize]") {
  std::vector<float> x{1.0f, -2.0f, 3.0f, -4.0f};
  auto r = normalize(x, NormType::Inf);
  REQUIRE_THAT(r[0], WithinAbs(0.25f, 1e-6f));
  REQUIRE_THAT(r[3], WithinAbs(-1.0f, 1e-6f));
}

TEST_CASE("normalize L2 norm produces unit vector", "[util][normalize]") {
  std::vector<float> x{3.0f, 4.0f};
  auto r = normalize(x, NormType::L2);
  REQUIRE_THAT(r[0], WithinAbs(0.6f, 1e-6f));
  REQUIRE_THAT(r[1], WithinAbs(0.8f, 1e-6f));
}

TEST_CASE("normalize leaves zero vectors unchanged", "[util][normalize][edge]") {
  std::vector<float> x(5, 0.0f);
  auto r = normalize(x, NormType::Inf);
  for (float v : r) REQUIRE(v == 0.0f);
}

TEST_CASE("normalize raw empty inputs accept null pointers", "[util][normalize][edge]") {
  REQUIRE(normalize(nullptr, 0).empty());
  REQUIRE(normalize_matrix(nullptr, 0, 4, 1).empty());
  REQUIRE(normalize_matrix(nullptr, 4, 0, 0).empty());
  REQUIRE_THROWS_AS(normalize(nullptr, 1), SonareException);
  REQUIRE_THROWS_AS(normalize_matrix(nullptr, 1, 1, 1), SonareException);
}

TEST_CASE("vector normalize matches librosa (Inf/L1/L2)", "[librosa][util][normalize]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/vector_normalize.json");
  const auto& v = json["data"]["vector"];
  const auto& input_arr = v["input"].as_array();
  std::vector<float> input;
  input.reserve(input_arr.size());
  for (const auto& e : input_arr) input.push_back(e.as_float());

  auto check = [&](const std::string& key, NormType type) {
    const auto& expected_arr = v[key].as_array();
    auto got = normalize(input, type);
    REQUIRE(got.size() == expected_arr.size());
    for (size_t i = 0; i < got.size(); ++i) {
      REQUIRE_THAT(got[i], WithinAbs(expected_arr[i].as_float(), 1e-5f));
    }
  };
  check("inf_norm", NormType::Inf);
  check("l1_norm", NormType::L1);
  check("l2_norm", NormType::L2);
}

TEST_CASE("matrix normalize matches librosa along both axes", "[librosa][util][normalize]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/vector_normalize.json");
  const auto& m = json["data"]["matrix"];
  int rows = m["rows"].as_int();
  int cols = m["cols"].as_int();
  const auto& input_arr = m["input_flat"].as_array();
  std::vector<float> input;
  input.reserve(input_arr.size());
  for (const auto& e : input_arr) input.push_back(e.as_float());

  auto check_axis = [&](int axis, const std::string& key) {
    const auto& expected_arr = m[key].as_array();
    auto got = normalize_matrix(input.data(), rows, cols, axis, NormType::Inf);
    REQUIRE(got.size() == expected_arr.size());
    for (size_t i = 0; i < got.size(); ++i) {
      REQUIRE_THAT(got[i], WithinAbs(expected_arr[i].as_float(), 1e-5f));
    }
  };
  check_axis(1, "axis1_inf_norm_flat");
  check_axis(0, "axis0_inf_norm_flat");
}

TEST_CASE("matrix normalize along axis 0 matches the single-vector norm per column",
          "[util][normalize]") {
  // Exact equality rather than a tolerance: the axis-0 traversal is the thing under test, and
  // a tolerance would re-admit exactly the drift a reordered or wider accumulator introduces.
  // Both narrow orientations run because an index or a stride taken from the wrong extent stays
  // in range in only one of them, and out of range is not a readable failure. The wide shape
  // spans three column tiles with the last one partial, which is the only way an offset that is
  // correct for the first tile and wrong for the rest becomes visible at all.
  for (const std::pair<int, int>& shape :
       {std::make_pair(7, 23), std::make_pair(23, 7), std::make_pair(5, 600)}) {
    const int rows = shape.first;
    const int cols = shape.second;
    const std::vector<float> m = matrix_fixture(rows, cols, 0x51ce5u);
    for (NormType norm : {NormType::Inf, NormType::L1, NormType::L2, NormType::Power}) {
      CAPTURE(rows, cols, static_cast<int>(norm));
      const std::vector<float> got = normalize_matrix(m.data(), rows, cols, /*axis=*/0, norm);
      REQUIRE(got.size() == m.size());
      for (int c = 0; c < cols; ++c) {
        const std::vector<float> want = normalized_column(m, rows, cols, c, norm);
        for (int r = 0; r < rows; ++r) {
          CAPTURE(r, c);
          REQUIRE(got[static_cast<std::size_t>(r) * cols + c] == want[r]);
        }
      }
    }
  }
}

TEST_CASE("matrix normalize leaves a sub-threshold column untouched", "[util][normalize][edge]") {
  // Asserted against the input rather than against normalize(), which the case above uses as
  // its oracle: a floor dropped from both sides at once would keep that comparison green. An
  // all-zero column is an ordinary silent frame for the chroma callers of the axis-0 path, and
  // without the floor it is scaled by the reciprocal of zero and comes back NaN.
  SECTION("a zero column and a column under the epsilon floor") {
    // Run wide as well as narrow so the guarded columns land in the last column tile and not
    // only in the first, where a tile offset is zero and every offset bug looks correct.
    for (const std::pair<int, int>& shape : {std::make_pair(7, 23), std::make_pair(5, 600)}) {
      const int rows = shape.first;
      const int cols = shape.second;
      const std::vector<float> m = matrix_fixture(rows, cols, 0x51ce5u);
      for (NormType norm : {NormType::Inf, NormType::L1, NormType::L2, NormType::Power}) {
        CAPTURE(rows, cols, static_cast<int>(norm));
        const std::vector<float> got = normalize_matrix(m.data(), rows, cols, /*axis=*/0, norm);
        for (int r = 0; r < rows; ++r) {
          for (int c : {0, 1, cols - 1}) {
            CAPTURE(r, c);
            const std::size_t i = static_cast<std::size_t>(r) * cols + c;
            REQUIRE(std::isfinite(got[i]));
            REQUIRE(got[i] == m[i]);
          }
        }
      }
    }
  }

  SECTION("a threshold above every column norm") {
    // The floor is max(threshold, epsilon), so a threshold this large takes every column down
    // the untouched branch, including the ones the section above normalizes.
    const int rows = 7;
    const int cols = 23;
    const std::vector<float> m = matrix_fixture(rows, cols, 0x51ce5u);
    for (NormType norm : {NormType::Inf, NormType::L1, NormType::L2, NormType::Power}) {
      CAPTURE(static_cast<int>(norm));
      const std::vector<float> got =
          normalize_matrix(m.data(), rows, cols, /*axis=*/0, norm, /*threshold=*/1e30f);
      REQUIRE(got.size() == m.size());
      for (std::size_t i = 0; i < m.size(); ++i) {
        CAPTURE(i);
        REQUIRE(got[i] == m[i]);
      }
    }
  }
}
