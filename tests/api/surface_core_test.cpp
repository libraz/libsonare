/// @file surface_core_test.cpp
/// @brief Consumer-side tests of the core part of the public C surface.
///
/// Cases reach the library only through sonare_c.h, exactly as an installed
/// consumer would, and carry the tags [surface][core].

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <string>

TEST_CASE("StaleResult is error code 11 with its own message", "[surface][core]") {
  STATIC_REQUIRE(SONARE_ERROR_STALE_RESULT == 11);
  const std::string message = sonare_error_message(SONARE_ERROR_STALE_RESULT);
  REQUIRE(message == "Stale result");
  REQUIRE(message != sonare_error_message(SONARE_ERROR_UNKNOWN));
}
