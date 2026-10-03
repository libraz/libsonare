/// @file insertion_sort_test.cpp
/// @brief Parity of util/insertion_sort against std::sort and std::stable_sort.

#include "util/insertion_sort.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace sonare;

TEST_CASE("insertion_sort matches std::sort on a total order", "[util][insertion_sort]") {
  std::mt19937 rng(12345);
  for (int trial = 0; trial < 2000; ++trial) {
    const size_t n = static_cast<size_t>(rng() % 70);
    std::vector<std::pair<int, int>> values(n);
    for (auto& value : values) {
      value = {static_cast<int>(rng() % 9) - 4, static_cast<int>(rng() % 5)};
    }
    std::vector<std::pair<int, int>> expected = values;
    std::sort(expected.begin(), expected.end());
    insertion_sort(values.begin(), values.end());
    REQUIRE(values == expected);
  }
}

TEST_CASE("insertion_sort matches std::stable_sort with ties", "[util][insertion_sort]") {
  std::mt19937 rng(678);
  const auto by_key = [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
    return a.first < b.first;
  };
  for (int trial = 0; trial < 2000; ++trial) {
    const size_t n = static_cast<size_t>(rng() % 70);
    std::vector<std::pair<int, int>> values(n);
    for (size_t i = 0; i < n; ++i) {
      values[i] = {static_cast<int>(rng() % 6), static_cast<int>(i)};
    }
    std::vector<std::pair<int, int>> expected = values;
    std::stable_sort(expected.begin(), expected.end(), by_key);
    insertion_sort(values.begin(), values.end(), by_key);
    REQUIRE(values == expected);
  }
}

TEST_CASE("insertion_sort moves non-trivial elements", "[util][insertion_sort]") {
  std::vector<std::string> names{"ratio", "attack", "", "release", "attack", "knee", "a"};
  std::vector<std::string> expected = names;
  std::sort(expected.begin(), expected.end());
  insertion_sort(names.begin(), names.end());
  REQUIRE(names == expected);

  std::vector<std::string> empty;
  insertion_sort(empty.begin(), empty.end());
  REQUIRE(empty.empty());
}
