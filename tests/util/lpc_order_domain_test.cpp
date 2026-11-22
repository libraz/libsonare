/// @file lpc_order_domain_test.cpp
/// @brief LPC order-domain checks at the signed integer boundary.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <climits>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "core/audio_ops.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/lpc.h"

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

TEST_CASE("LPC refuses an order the signal cannot support and names the order reached",
          "[audio_ops][lpc]") {
  // A sinusoid is predicted exactly at order two, so the recursion stops long before order 36.
  std::vector<float> tone(600);
  for (size_t i = 0; i < tone.size(); ++i) {
    tone[i] = static_cast<float>(
        0.9 * std::sin(sonare::constants::kTwoPiD * 440.0 * static_cast<double>(i) / 48000.0));
  }
  const int order = 36;
  const sonare::LpcResult model = sonare::lpc_burg(tone.data(), tone.size(), order);
  REQUIRE(model.order_reached < order);

  std::string message;
  try {
    (void)sonare::lpc(tone, order);
  } catch (const sonare::SonareException& error) {
    REQUIRE(error.code() == sonare::ErrorCode::InvalidParameter);
    message = error.what();
  }
  CAPTURE(message);
  REQUIRE(message.find("ill-conditioned for an order-36 model; stable up to order " +
                       std::to_string(model.order_reached)) != std::string::npos);

  const std::vector<float> supported = sonare::lpc(tone, model.order_reached);
  REQUIRE(supported.size() == static_cast<size_t>(model.order_reached) + 1);
}
