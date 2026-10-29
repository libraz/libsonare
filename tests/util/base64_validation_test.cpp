#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "util/base64.h"

TEST_CASE("Base64 rejection clears partial output", "[util][base64]") {
  for (const std::string text : {"!AAA", "A!AA", "AA!A", "AAA!", "AAAA!AAA", "AAAAA!AA", "AAAAAA!A",
                                 "AAAAAAA!", "AAAAAA=A", "AAAAAA==AAAA", "AAA"}) {
    CAPTURE(text);
    std::vector<uint8_t> decoded{42};
    REQUIRE_FALSE(sonare::base64_decode(text, &decoded));
    REQUIRE(decoded.empty());
  }
}

TEST_CASE("Base64 decoding preserves valid padding and output limits", "[util][base64]") {
  std::vector<uint8_t> decoded;
  REQUIRE(sonare::base64_decode("", &decoded, 0));
  REQUIRE(decoded.empty());
  REQUIRE(sonare::base64_decode("AA==", &decoded, 1));
  REQUIRE(decoded == std::vector<uint8_t>{0});
  REQUIRE(sonare::base64_decode("AAE=", &decoded, 2));
  REQUIRE(decoded == std::vector<uint8_t>{0, 1});
  REQUIRE(sonare::base64_decode("AAEC/f7/", &decoded, 6));
  REQUIRE(decoded == std::vector<uint8_t>{0, 1, 2, 253, 254, 255});
  REQUIRE_FALSE(sonare::base64_decode("AAEC/f7/", &decoded, 5));
  REQUIRE(decoded.empty());
  REQUIRE_FALSE(sonare::base64_decode("AAAA", nullptr));
}
