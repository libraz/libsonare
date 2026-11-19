#include "util/parameter_checks.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <stdexcept>
#include <string>

#include "util/db.h"
#include "util/exception.h"
#include "util/refusal_predicates.h"

namespace check = sonare::util::check;
namespace refusal = sonare::util::refusal;

namespace {

template <typename Fn>
std::string refusal_text(Fn&& fn) {
  try {
    fn();
  } catch (const sonare::SonareParameterRefusal& e) {
    return e.what();
  }
  return "";
}

}  // namespace

TEST_CASE("a float bound is written as the float's shortest decimal", "[util][refusal]") {
  CHECK(refusal::bound_text(static_cast<double>(0.98f)) == "0.98");
  CHECK(refusal::bound_text(0.98) == "0.98");
  CHECK(refusal::bound_text(1e-10) == "1e-10");
  CHECK(refusal::bound_text(24000.0) == "24000");
  CHECK(refusal::bound_text(static_cast<double>(3.4028235e38f)) == "3.4028235e38");
  CHECK(refusal::bound_text(1e-5) == "1e-5");
}

TEST_CASE("a computed bound is written inward at four digits unless it has a short decimal",
          "[util][refusal]") {
  using refusal::Inward;
  CHECK(refusal::bound_text(0.7341619357060499, Inward::Up) == "0.7342");
  CHECK(refusal::bound_text(2.2024857275554948, Inward::Down) == "2.202");
  // Inward even where nearest would round outward: 0.98107 is printed 0.981, not 0.9811.
  CHECK(refusal::bound_text(0.98107, Inward::Down) == "0.981");
  CHECK(refusal::bound_text(0.73414, Inward::Up) == "0.7342");
  // A value with a short exact decimal keeps it, a float one included.
  CHECK(refusal::bound_text(static_cast<double>(0.55f), Inward::Up) == "0.55");
  CHECK(refusal::bound_text(static_cast<double>(1.65f), Inward::Down) == "1.65");
  CHECK(refusal::computed_range_predicate(0.6929566, 2.0788694) ==
        "must be finite and in [0.693, 2.078]");
}

TEST_CASE("the largest accepted gain is finite as a linear float gain", "[util][refusal]") {
  CHECK(std::isfinite(sonare::db_to_linear(sonare::kMaxFiniteGainDb)));
  CHECK(sonare::db_to_linear(sonare::kMinPositiveGainDb) > 0.0f);
  CHECK(refusal_text([] { check::at_most(771.0, sonare::kMaxFiniteGainDb, "drive_db"); }) ==
        "drive_db must be finite and at most 770");
  CHECK(refusal_text([] { check::at_most(770.0, sonare::kMaxFiniteGainDb, "drive_db"); }).empty());
}

TEST_CASE("set, power-of-two, odd and sum checks state their domain", "[util][refusal]") {
  constexpr std::array<int, 4> kFactors{1, 2, 4, 8};
  CHECK(refusal_text([&] { check::one_of(3, kFactors, "oversample_factor"); }) ==
        "oversample_factor must be one of 1, 2, 4, 8");
  CHECK(refusal_text([&] { check::one_of(8, kFactors, "oversample_factor"); }).empty());

  CHECK(refusal_text([] { check::power_of_two_in_range(48, 16, 65536, "n_fft"); }) ==
        "n_fft must be a power of two in [16, 65536]");
  CHECK(refusal_text([] { check::power_of_two_in_range(8, 16, 65536, "n_fft"); }) ==
        "n_fft must be a power of two in [16, 65536]");
  CHECK(refusal_text([] { check::power_of_two_in_range(64, 16, 65536, "n_fft"); }).empty());

  CHECK(refusal_text([] { check::odd_integer_in_range(4, 3, 255, "kernel_size"); }) ==
        "kernel_size must be an odd integer in [3, 255]");
  CHECK(refusal_text([] { check::odd_integer_in_range(5, 3, 255, "kernel_size"); }).empty());

  CHECK(refusal_text([] {
          check::sum_at_most(500.0, 300.0, sonare::kMaxFiniteGainDb, "max_boost_db",
                             "output_gain_db");
        }) == "max_boost_db + output_gain_db must be at most 770");
  CHECK(refusal_text([] {
          check::sum_at_most(500.0, 270.0, sonare::kMaxFiniteGainDb, "max_boost_db",
                             "output_gain_db");
        }).empty());
}

TEST_CASE("a finite-only float member refuses a non-finite value", "[util][refusal]") {
  CHECK(refusal_text([] { check::finite_float(INFINITY, "bias"); }) ==
        "bias must be finite and in [-3.4028235e38, 3.4028235e38]");
  CHECK(refusal_text([] { check::finite_float(-1.0e30f, "bias"); }).empty());
}

TEST_CASE("a double past the float range saturates to the infinity of its sign",
          "[util][refusal]") {
  CHECK(check::saturated_float(1.0e40) == INFINITY);
  CHECK(check::saturated_float(-1.0e40) == -INFINITY);
  CHECK(std::isnan(check::saturated_float(NAN)));
  CHECK(check::saturated_float(0.25) == 0.25f);
  CHECK(check::saturated_float(3.4028234663852886e38) == 3.4028234663852886e38f);
  CHECK(refusal_text([] { check::in_range(check::saturated_float(1.0e40), 0.0, 1.0, "mix"); }) ==
        "mix must be finite and in [0, 1]");
}

TEST_CASE("a refusal is respelled into request keys, camel case unless renamed",
          "[util][refusal]") {
  constexpr std::array<check::MemberKey, 1> kRenamed{{{"max_seconds", "maxSec"}}};
  CHECK(refusal_text([&] {
          check::with_camel_paths([] { check::in_range(700.0f, 0.0, 600.0, "max_seconds"); },
                                  kRenamed);
        }) == "maxSec must be finite and in [0, 600]");
  CHECK(refusal_text([&] {
          check::with_camel_paths(
              [] { check::in_range(200.0f, 0.0, 100.0, "air.humidity_percent"); }, kRenamed);
        }) == "air.humidityPercent must be finite and in [0, 100]");
}

namespace {

sonare::SonareParameterRefusal caught_refusal(void (*call)()) {
  try {
    call();
  } catch (const sonare::SonareParameterRefusal& refusal) {
    return refusal;
  }
  FAIL("the call did not refuse");
  throw std::logic_error("unreachable");
}

}  // namespace

TEST_CASE("the JS refusal renderer renders once and leaves a caller-spelled refusal alone",
          "[util][refusal]") {
  const auto core = caught_refusal([] { check::in_range(2.0f, 0.0, 1.0, "gate_lufs"); });
  REQUIRE_FALSE(core.caller_spelled());
  const auto rendered = check::camel_spelled(core);
  CHECK(std::string(rendered.what()) == "gateLufs must be finite and in [0, 1]");
  CHECK(rendered.caller_spelled());
  CHECK(std::string(check::camel_spelled(rendered).what()) == rendered.what());

  // A subject the caller already spelled keeps its underscores; camel case would rename it.
  const auto keyed = caught_refusal([] {
    throw sonare::SonareParameterRefusal::predicate(sonare::ErrorCode::InvalidParameter,
                                                    "strips[0].inserts[0].params.snake_key",
                                                    "must be finite")
        .as_caller_spelled();
  });
  CHECK(std::string(check::camel_spelled(keyed).what()) ==
        "strips[0].inserts[0].params.snake_key must be finite");

  const auto relation = caught_refusal([] {
    check::refuse_relation({"fundamental_hz", "search_range_hz"}, "{0} + {1} must be at least 1");
  });
  CHECK(std::string(check::camel_spelled(relation).what()) ==
        "fundamentalHz + searchRangeHz must be at least 1");
}

TEST_CASE("an unknown key and a wrong type are shape refusals, a domain refusal is not",
          "[util][refusal]") {
  using sonare::ErrorCode;
  using sonare::SonareParameterRefusal;
  const auto wrong = SonareParameterRefusal::wrong_type(ErrorCode::InvalidParameter, "irBase64",
                                                        "must be a string");
  CHECK(std::string(wrong.what()) == "irBase64 must be a string");
  CHECK(wrong.shape_error());
  CHECK(SonareParameterRefusal::unknown(ErrorCode::InvalidParameter, "gainDbb").shape_error());
  CHECK_FALSE(SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, "gainDb",
                                                "must be finite and at most 770")
                  .shape_error());
  CHECK_FALSE(SonareParameterRefusal::relation(ErrorCode::InvalidParameter, {"fmax", "fmin"},
                                               "{0} must be finite and greater than {1}")
                  .shape_error());
  // A respelling keeps the kind, so the class a translation point picks survives it.
  const auto respelled = sonare::util::check::camel_spelled(SonareParameterRefusal::wrong_type(
      ErrorCode::InvalidParameter, "ir_base64", "must be a string"));
  CHECK(respelled.shape_error());
  CHECK(std::string(respelled.what()) == "irBase64 must be a string");
}

TEST_CASE("a NULL pointer is refused by name, alone or while its count is set", "[util][refusal]") {
  const int value = 0;
  CHECK_NOTHROW(check::non_null(&value, "out"));
  CHECK(refusal_text([] { check::non_null(nullptr, "out"); }) == "out must not be NULL");

  CHECK_NOTHROW(check::non_null_when_count(nullptr, 0, "samples", "length"));
  CHECK_NOTHROW(check::non_null_when_count(&value, 3, "samples", "length"));
  const auto counted =
      caught_refusal([] { check::non_null_when_count(nullptr, 3, "samples", "length"); });
  CHECK(std::string(counted.what()) == "samples must not be NULL when length is not 0");
  CHECK(std::string(check::camel_spelled(counted).what()) ==
        "samples must not be NULL when length is not 0");
  CHECK_FALSE(counted.shape_error());
}
