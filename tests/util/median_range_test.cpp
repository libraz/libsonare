#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "util/math_utils.h"
#include "util/non_finite_sample.h"

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

TEST_CASE("median selection matches the sorted reference without changing its input",
          "[math_utils][median]") {
  REQUIRE(sonare::median(nullptr, 0) == 0.0f);

  const float infinity = std::numeric_limits<float>::infinity();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const std::vector<std::vector<float>> cases = {
      {1.0f},
      {1.0f, 2.0f, 3.0f},
      {1.0f, 2.0f, 3.0f, 4.0f},
      {9.0f, 4.0f, 7.0f, 1.0f, 5.0f, 8.0f, 2.0f, 6.0f},
      {8.0f, 7.0f, 6.0f, 5.0f, 4.0f, 3.0f, 2.0f, 1.0f},
      {4.0f, 4.0f, 1.0f, 4.0f, 2.0f, 4.0f, 3.0f, 4.0f},
      {nan, 5.0f, 1.0f, infinity, 3.0f, -infinity},
      {nan, infinity, 2.0f, 2.0f, -infinity, 4.0f, 1.0f},
  };

  for (const auto& input : cases) {
    const std::vector<float> original = input;
    std::vector<float> sorted = input;
    (void)sonare::resolve_non_finite_run(sonare::SampleDestination::kOrderedContainer,
                                         sorted.data(), sorted.size());
    std::sort(sorted.begin(), sorted.end());
    const float expected = static_cast<float>(sonare::percentile_sorted(sorted, 0.5));
    const float actual = sonare::median(input.data(), input.size());

    CAPTURE(input.size());
    const float tolerance =
        std::numeric_limits<float>::epsilon() * std::max(1.0f, std::abs(expected));
    REQUIRE_THAT(actual, Catch::Matchers::WithinAbs(expected, tolerance));
    REQUIRE(std::memcmp(input.data(), original.data(), input.size() * sizeof(float)) == 0);
  }
}
