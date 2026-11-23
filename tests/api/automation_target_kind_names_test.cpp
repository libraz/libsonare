#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <string>

TEST_CASE("automation target kind names follow C ordinal order", "[c_api][project][automation]") {
#if defined(SONARE_WITH_ARRANGEMENT)
  const char* names = sonare_automation_target_kind_names();
  REQUIRE(names != nullptr);
  CHECK(std::string(names) ==
        "opaque\ntrackFaderDb\ntrackPan\ntrackSendDb\ntrackWidth\ntrackInsertParam");
#else
  CHECK(sonare_automation_target_kind_names() == nullptr);
  CHECK(sonare_last_error_code() == SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_last_error_reason() != nullptr);
  CHECK(std::string(sonare_last_error_reason()) == "notCompiledIn");
#endif
}
