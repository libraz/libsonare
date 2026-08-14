/// @file types_test.cpp
/// @brief Tests for shared utility types.

#include "util/types.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <limits>
#include <stdexcept>

using namespace sonare;

TEST_CASE("MatrixView preserves ordinary access and empty views", "[types]") {
  int data[] = {1, 2, 3, 4, 5, 6};
  MatrixView<int> view(data, 2, 3);

  REQUIRE(view.rows() == 2);
  REQUIRE(view.cols() == 3);
  REQUIRE(view.size() == 6);
  REQUIRE_FALSE(view.empty());
  REQUIRE(view.at(1, 2) == 6);
  REQUIRE(view(0, 1) == 2);
  REQUIRE(view.row(1) == data + 3);

  MatrixView<int> null_view(nullptr, 0, std::numeric_limits<size_t>::max());
  REQUIRE(null_view.empty());
  REQUIRE(null_view.size() == 0);

  MatrixView<int> zero_columns(&data[0], std::numeric_limits<size_t>::max(), 0);
  REQUIRE(zero_columns.empty());
  REQUIRE(zero_columns.size() == 0);
}

TEST_CASE("MatrixView rejects overflowing dimensions", "[types][overflow]") {
  const size_t max = std::numeric_limits<size_t>::max();

  REQUIRE_THROWS_AS(MatrixView<int>(nullptr, max, 2), std::overflow_error);
  REQUIRE_THROWS_AS(MatrixView<int>(nullptr, 2, max), std::overflow_error);

  MatrixView<int> max_rows(nullptr, max, 1);
  REQUIRE(max_rows.size() == max);
}

TEST_CASE("MatrixView rejects overflowing or out-of-range indices", "[types][overflow]") {
  const size_t max = std::numeric_limits<size_t>::max();
  MatrixView<int> max_columns(nullptr, 1, max);

  REQUIRE_THROWS_AS(max_columns.row(2), std::overflow_error);
  REQUIRE_THROWS_AS(max_columns.at(2, 0), std::overflow_error);
  REQUIRE_THROWS_AS(max_columns.at(1, max), std::overflow_error);
  REQUIRE_THROWS_AS(max_columns.row(1), std::out_of_range);
  REQUIRE_THROWS_AS(max_columns.at(0, max), std::out_of_range);
}
