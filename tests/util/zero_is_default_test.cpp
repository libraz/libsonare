#include "util/zero_is_default.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <limits>

#include "util/exception.h"

using Catch::Matchers::WithinAbs;
using sonare::ErrorCode;
using sonare::SonareException;
using sonare::ZeroIsDefault;

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

ErrorCode checked_error(float caller_value, float minimum, float maximum) {
  try {
    const float resolved = ZeroIsDefault(caller_value).checked(7.0f, minimum, maximum, "field");
    CAPTURE(resolved);
    return ErrorCode::Ok;
  } catch (const SonareException& error) {
    return error.code();
  }
}

}  // namespace

TEST_CASE("ZeroIsDefault substitutes the library default only for the sentinel",
          "[util][numeric]") {
  REQUIRE_THAT(ZeroIsDefault(0.0f).or_default(5.0f), WithinAbs(5.0f, 0.0f));
  REQUIRE_THAT(ZeroIsDefault(-0.0f).or_default(5.0f), WithinAbs(5.0f, 0.0f));
  REQUIRE_THAT(ZeroIsDefault(2.5f).or_default(5.0f), WithinAbs(2.5f, 0.0f));

  // The point of the type: a value outside the accepted range is NOT the
  // sentinel, so it survives to whichever validation the entry point runs
  // instead of being silently replaced by the default.
  REQUIRE_THAT(ZeroIsDefault(-5.0f).or_default(5.0f), WithinAbs(-5.0f, 0.0f));
  REQUIRE(std::isnan(ZeroIsDefault(kNaN).or_default(5.0f)));
}

TEST_CASE("ZeroIsDefault::checked inspects the caller's value, not the resolved one",
          "[util][numeric]") {
  // The sentinel resolves to the default and is always accepted.
  REQUIRE(checked_error(0.0f, 1.0f, 10.0f) == ErrorCode::Ok);
  REQUIRE_THAT(ZeroIsDefault(0.0f).checked(7.0f, 1.0f, 10.0f, "field"), WithinAbs(7.0f, 0.0f));

  // An in-range request is applied verbatim.
  REQUIRE_THAT(ZeroIsDefault(3.0f).checked(7.0f, 1.0f, 10.0f, "field"), WithinAbs(3.0f, 0.0f));

  // Anything else is rejected rather than resolving to the default, which is
  // what "validate the value the caller passed" buys: had the substitution run
  // first, each of these would have been checked as the (valid) default.
  for (float invalid : {-5.0f, 11.0f, kNaN, kInf, -kInf}) {
    CAPTURE(invalid);
    REQUIRE(checked_error(invalid, 1.0f, 10.0f) == ErrorCode::InvalidParameter);
  }
}
