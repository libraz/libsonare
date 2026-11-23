#include "util/error_reason.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>
#include <utility>

#include "mixing/api/scene.h"
#include "util/exception.h"
#include "util/json.h"

#if defined(SONARE_WITH_MASTERING)
#include "mastering/api/chain.h"
#endif

namespace {

struct CapturedError {
  sonare::ErrorCode code;
  std::optional<sonare::ErrorReason> reason;
  std::string message;
  std::optional<std::string> subject;
};

template <typename Operation>
std::optional<CapturedError> capture_error(Operation operation) {
  try {
    operation();
  } catch (const sonare::SonareParameterRefusal& error) {
    return CapturedError{error.code(), error.reason(), error.what(), error.subject()};
  } catch (const sonare::SonareException& error) {
    return CapturedError{error.code(), error.reason(), error.what(), std::nullopt};
  }
  return std::nullopt;
}

}  // namespace

TEST_CASE("error reason tokens are stable", "[error_reason]") {
  constexpr std::array<std::pair<sonare::ErrorReason, const char*>, 10> cases{{
      {sonare::ErrorReason::MalformedJson, "malformedJson"},
      {sonare::ErrorReason::UnsupportedSchemaVersion, "unsupportedSchemaVersion"},
      {sonare::ErrorReason::NotCompiledIn, "notCompiledIn"},
      {sonare::ErrorReason::NotPersistable, "notPersistable"},
      {sonare::ErrorReason::UnresolvedSource, "unresolvedSource"},
      {sonare::ErrorReason::UnresolvedSoundFont, "unresolvedSoundFont"},
      {sonare::ErrorReason::DeviceUnavailable, "deviceUnavailable"},
      {sonare::ErrorReason::DeviceFormatRefused, "deviceFormatRefused"},
      {sonare::ErrorReason::EngineDeviceDriven, "engineDeviceDriven"},
      {sonare::ErrorReason::ReleasedHandle, "releasedHandle"},
  }};

  for (const auto& [reason, token] : cases) {
    CHECK(std::string(sonare::error_reason_token(reason)) == token);
  }

  const sonare::SonareException without_reason(sonare::ErrorCode::InvalidFormat,
                                               "invalid document");
  CHECK_FALSE(without_reason.reason().has_value());

  const auto document_refusal = sonare::SonareParameterRefusal::predicate_with_reason(
      sonare::ErrorCode::InvalidFormat, "version", "must be 2",
      sonare::ErrorReason::UnsupportedSchemaVersion);
  const auto argument_refusal = document_refusal.respelled(
      sonare::ErrorCode::InvalidParameter,
      [](const std::string&) { return std::string("overrides"); }, true);
  CHECK(argument_refusal.code() == sonare::ErrorCode::InvalidParameter);
  CHECK_FALSE(argument_refusal.reason().has_value());
  CHECK(argument_refusal.subject() == "overrides");
}

TEST_CASE("scene and chain readers attach JSON reasons", "[error_reason]") {
  const std::string malformed = "{broken";
  std::string parser_message;
  try {
    (void)sonare::util::json::parse(malformed);
  } catch (const sonare::util::json::JsonError& error) {
    parser_message = error.what();
  }
  REQUIRE_FALSE(parser_message.empty());

  const auto scene_error =
      capture_error([&] { (void)sonare::mixing::api::scene_from_json(malformed); });
  REQUIRE(scene_error.has_value());
  CHECK(scene_error->code == sonare::ErrorCode::InvalidFormat);
  CHECK(scene_error->reason == sonare::ErrorReason::MalformedJson);
  CHECK(scene_error->message == parser_message);

#if defined(SONARE_WITH_MASTERING)
  const auto chain_error =
      capture_error([&] { (void)sonare::mastering::api::chain_config_from_json(malformed); });
  REQUIRE(chain_error.has_value());
  CHECK(chain_error->code == sonare::ErrorCode::InvalidFormat);
  CHECK(chain_error->reason == sonare::ErrorReason::MalformedJson);
  CHECK(chain_error->message == parser_message);
#endif
}

TEST_CASE("scene and chain readers identify unsupported schema versions", "[error_reason]") {
  const auto scene_error =
      capture_error([] { (void)sonare::mixing::api::scene_from_json(R"({"version":3})"); });
  REQUIRE(scene_error.has_value());
  CHECK(scene_error->code == sonare::ErrorCode::InvalidFormat);
  CHECK(scene_error->reason == sonare::ErrorReason::UnsupportedSchemaVersion);
  CHECK(scene_error->message == "version must be 2");
  CHECK(scene_error->subject == "version");

#if defined(SONARE_WITH_MASTERING)
  const auto chain_error = capture_error(
      [] { (void)sonare::mastering::api::chain_config_from_json(R"({"version":3,"params":{}})"); });
  REQUIRE(chain_error.has_value());
  CHECK(chain_error->code == sonare::ErrorCode::InvalidFormat);
  CHECK(chain_error->reason == sonare::ErrorReason::UnsupportedSchemaVersion);
  CHECK(chain_error->message == "version must be 2, or left out");
  CHECK_FALSE(chain_error->subject.has_value());

  const auto fractional_chain_version = capture_error([] {
    (void)sonare::mastering::api::chain_config_from_json(R"({"version":2.5,"params":{}})");
  });
  REQUIRE(fractional_chain_version.has_value());
  CHECK(fractional_chain_version->code == sonare::ErrorCode::InvalidFormat);
  CHECK_FALSE(fractional_chain_version->reason.has_value());
  CHECK(fractional_chain_version->message == "version must be 2, or left out");
#endif
}
