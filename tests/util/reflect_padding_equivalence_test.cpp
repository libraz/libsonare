#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <limits>
#include <vector>

#include "util/reflect_padding.h"

TEST_CASE("Reflect padding matches repeated reflection for short and long margins",
          "[util][reflect_padding]") {
  for (size_t size : {size_t{0}, size_t{1}, size_t{2}, size_t{3}, size_t{17}, size_t{4096}}) {
    std::vector<float> input(size);
    for (size_t i = 0; i < size; ++i) input[i] = static_cast<float>(i) - 2.0f;
    if (size > 2) {
      input[0] = -0.0f;
      input[1] = std::numeric_limits<float>::quiet_NaN();
    }
    for (int pad : {0, 1, 2, 17, 1024}) {
      CAPTURE(size, pad);
      const auto output = sonare::reflect_center_pad(input.data(), size, pad);
      REQUIRE(output.size() == size + 2 * static_cast<size_t>(pad));
      for (size_t i = 0; i < output.size(); ++i) {
        const float expected =
            size == 0 ? 0.0f : input[sonare::reflect_index(static_cast<int64_t>(i) - pad, size)];
        REQUIRE(std::memcmp(&output[i], &expected, sizeof(float)) == 0);
      }
      const auto silence = sonare::reflect_center_pad(nullptr, size, pad);
      REQUIRE(silence == std::vector<float>(output.size(), 0.0f));
    }
  }
}

TEST_CASE("Reflect padding refuses an overflowing output length", "[util][reflect_padding]") {
  REQUIRE_THROWS_AS(sonare::reflect_center_pad(nullptr, std::numeric_limits<size_t>::max(), 1),
                    sonare::SonareException);
}
