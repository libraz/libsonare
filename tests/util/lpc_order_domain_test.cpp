/// @file lpc_order_domain_test.cpp
/// @brief LPC order-domain checks at the signed integer boundary.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <climits>
#include <cstddef>
#include <vector>

#include "core/audio_ops.h"
#include "util/exception.h"

namespace {

template <typename Operation>
void require_invalid_parameter(Operation&& operation) {
  bool rejected = false;
  try {
    operation();
  } catch (const sonare::SonareException& error) {
    rejected = error.code() == sonare::ErrorCode::InvalidParameter;
  }
  REQUIRE(rejected);
}

}  // namespace

TEST_CASE("LPC rejects extreme orders before order-plus-one arithmetic", "[audio_ops][lpc]") {
  const float sample = 0.25f;
  require_invalid_parameter([] { (void)sonare::lpc(nullptr, 0, INT_MAX); });
  require_invalid_parameter([&] { (void)sonare::lpc(&sample, 1, INT_MAX); });
}

TEST_CASE("LPC retains ordinary order-one coefficients and rejects equal length",
          "[audio_ops][lpc]") {
  const std::vector<float> samples{1.0f, 2.0f, 3.0f, 4.0f};
  const std::vector<float> coefficients = sonare::lpc(samples, 1);
  REQUIRE(coefficients.size() == 2);
  REQUIRE(coefficients[0] == 1.0f);
  REQUIRE_THAT(coefficients[1], Catch::Matchers::WithinAbs(-40.0f / 43.0f, 1e-6f));

  const std::vector<float> length_equal{0.25f, -0.5f};
  require_invalid_parameter([&] { (void)sonare::lpc(length_equal, 2); });
}
