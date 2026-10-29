#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>

#include "util/exception.h"
#include "util/frame.h"

TEST_CASE("Frame count rejects results outside its int return range", "[util][frame][edge]") {
  const size_t max_frames = static_cast<size_t>(std::numeric_limits<int>::max());
  REQUIRE(sonare::frame_count(max_frames, 1, 1) == std::numeric_limits<int>::max());
  REQUIRE_THROWS_AS(sonare::frame_count(max_frames + 1, 1, 1), sonare::SonareException);
  REQUIRE(sonare::frame_count(max_frames * 3, 1, 3) == std::numeric_limits<int>::max());
  REQUIRE_THROWS_AS(sonare::frame_count(max_frames * 3 + 1, 1, 3), sonare::SonareException);
}

TEST_CASE("Invalid framing parameters still produce an empty frame count", "[util][frame]") {
  REQUIRE(sonare::frame_count(20, 0, 1) == 0);
  REQUIRE(sonare::frame_count(20, 1, 0) == 0);
  REQUIRE(sonare::frame_count(0, 1, 1) == 0);
}
