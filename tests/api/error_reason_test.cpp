#include <sonare/sonare_c.h>
#include <sonare/sonare_c_project.h>

#include <catch2/catch_test_macros.hpp>
#include <string>

#include "c_api/sonare_c_internal.h"

TEST_CASE("C error reasons follow the last error record", "[c_api][error_reason]") {
  sonare_c_detail::clear_last_error();
  CHECK(sonare_last_error_reason() == nullptr);

#if defined(SONARE_WITH_ARRANGEMENT)
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  CHECK(sonare_last_error_reason() == nullptr);

  REQUIRE(sonare_project_set_mixer_scene_json(project, "{") == SONARE_ERROR_INVALID_FORMAT);
  const char* malformed_reason = sonare_last_error_reason();
  REQUIRE(malformed_reason != nullptr);
  CHECK(std::string(malformed_reason) == "malformedJson");
  CHECK(sonare_last_error_reason() == malformed_reason);
  CHECK(sonare_last_error_code() == SONARE_ERROR_INVALID_FORMAT);

  REQUIRE(sonare_project_set_mixer_scene_json(project, R"({"version":3})") ==
          SONARE_ERROR_INVALID_FORMAT);
  const char* version_reason = sonare_last_error_reason();
  REQUIRE(version_reason != nullptr);
  CHECK(std::string(version_reason) == "unsupportedSchemaVersion");

  REQUIRE(sonare_project_set_mixer_scene_json(project, R"({"version":2})") == SONARE_OK);
  CHECK(sonare_last_error_reason() == nullptr);

  REQUIRE(sonare_project_set_mixer_scene_json(project, R"({"version":2,"unknown":true})") ==
          SONARE_ERROR_INVALID_FORMAT);
  CHECK(sonare_last_error_reason() == nullptr);

  REQUIRE(sonare_project_set_mixer_scene_json(project, R"({"version":2})") == SONARE_OK);
  CHECK(sonare_last_error_reason() == nullptr);
  sonare_project_destroy(project);
#endif

  REQUIRE(sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement) ==
          SONARE_ERROR_NOT_SUPPORTED);
  const char* static_reason = sonare_last_error_reason();
  REQUIRE(static_reason != nullptr);
  CHECK(std::string(static_reason) == "notCompiledIn");
  CHECK(sonare_last_error_code() == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(sonare_last_error_reason() == static_reason);
  CHECK(sonare_last_error_code() == SONARE_ERROR_NOT_SUPPORTED);

  CHECK(sonare_c_detail::refuse_not_compiled_in_handle<SonareProject>(
            sonare_c_detail::Feature::kArrangement) == nullptr);
  CHECK(sonare_last_error_reason() == static_reason);

  REQUIRE(sonare_c_detail::refuse_value_not_compiled_in(sonare_c_detail::Feature::kArrangement,
                                                        "codec") == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(sonare_last_error_reason() == static_reason);
}
