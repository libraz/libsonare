#include <catch2/catch_test_macros.hpp>
#include <climits>
#include <cstddef>
#include <vector>

#include "util/exception.h"
#include "util/peak.h"

TEST_CASE("peak_pick handles window and wait values at INT_MAX", "[util][peak][edge]") {
  const std::vector<float> signal{0.0f, 1.0f, 0.0f};
  REQUIRE(sonare::peak_pick(signal, 0, INT_MAX, 0, INT_MAX, 0.0f, INT_MAX) == std::vector<int>{1});
}

TEST_CASE("peak_pick clips oversized windows to the input range", "[util][peak][edge]") {
  const std::vector<float> signal{0.0f, 1.0f, 0.0f};
  const std::vector<int> expected{1};
  REQUIRE(sonare::peak_pick(signal, 2, 3, 2, 3, 0.0f, 0) == expected);
  REQUIRE(sonare::peak_pick(signal, INT_MAX, INT_MAX, INT_MAX, INT_MAX, 0.0f, 0) == expected);
}

TEST_CASE("peak_pick refuses lengths that cannot be represented in its output indices",
          "[util][peak][edge]") {
  const float sample = 0.0f;
  const auto too_many_samples = static_cast<std::size_t>(INT_MAX) + 1;
  REQUIRE_THROWS_AS(sonare::peak_pick(&sample, too_many_samples, 0, 1, 0, 1, 0.0f, 0),
                    sonare::SonareException);
}
