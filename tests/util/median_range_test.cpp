#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <vector>

#include "util/math_utils.h"

TEST_CASE("median averages even middle values without float overflow", "[math_utils][median]") {
  const float largest = std::numeric_limits<float>::max();
  std::vector<float> positive{largest, largest};
  std::vector<float> negative{-largest, -largest};
  std::vector<float> opposite{-largest, largest};

  REQUIRE(sonare::median(positive.data(), positive.size()) == largest);
  REQUIRE(sonare::median(negative.data(), negative.size()) == -largest);
  REQUIRE(sonare::median(opposite.data(), opposite.size()) == 0.0f);
  REQUIRE(positive == std::vector<float>{largest, largest});
  REQUIRE(negative == std::vector<float>{-largest, -largest});
  REQUIRE(opposite == std::vector<float>{-largest, largest});
}

TEST_CASE("median uses the sorted percentile interpolation for even ranks",
          "[math_utils][median]") {
  std::vector<float> values{6.0f, 1.0f, 8.0f, 2.0f};
  const std::vector<float> sorted{1.0f, 2.0f, 6.0f, 8.0f};

  REQUIRE(sonare::median(values.data(), values.size()) ==
          static_cast<float>(sonare::percentile_sorted(sorted, 0.5)));
  REQUIRE(values == std::vector<float>{6.0f, 1.0f, 8.0f, 2.0f});
}
