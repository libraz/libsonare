#include "util/parameter_checks.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

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
  // Its requirement is a presence token, so it refuses the request's shape.
  CHECK(counted.shape_error());
}

TEST_CASE("an empty string, an unknown id and a stated predicate are refused by name",
          "[util][refusal]") {
  CHECK_NOTHROW(check::non_empty("warm", "preset_name"));
  CHECK(refusal_text([] { check::non_empty("", "preset_name"); }) ==
        "preset_name must not be empty");
  CHECK(refusal_text([] { check::refuse_unknown_id("strip_id", "a strip", "engine"); }) ==
        "strip_id must be the id of a strip in this engine");
  const auto stated = caught_refusal([] { check::refuse("layout", "must name a declared value"); });
  CHECK(std::string(stated.what()) == "layout must name a declared value");
  CHECK_FALSE(stated.shape_error());
}

TEST_CASE("a finite-only double is refused by name, and an indexed member names its element",
          "[util][refusal]") {
  CHECK_NOTHROW(check::finite(1.0e300, "gain_db"));
  CHECK(refusal_text([] { check::finite(NAN, "gain_db"); }) == "gain_db must be finite");
  CHECK(refusal_text([] { check::finite(-INFINITY, "gain_db"); }) == "gain_db must be finite");
  CHECK(check::indexed_subject("lanes", 2, "send_count") == "lanes[2].send_count");
  const auto indexed = caught_refusal(
      [] { check::finite(INFINITY, check::indexed_subject("bands", 0, "gain_db").c_str()); });
  CHECK(std::string(check::camel_spelled(indexed).what()) == "bands[0].gainDb must be finite");
}

TEST_CASE("an integer bound is written in plain digits, never in exponent form",
          "[util][refusal]") {
  CHECK(refusal_text([] { check::integer_in_range(600000000, 0, 500000000, "count"); }) ==
        "count must be an integer in [0, 500000000]");
  CHECK(refusal::integer_bound_text(500000000.0) == "500000000");
  CHECK(refusal::integer_bound_text(4294967295.0) == "4294967295");
  CHECK(refusal::integer_bound_text(0.5) == "0.5");
}

TEST_CASE("a numeric domain refuses a fraction, a NaN and an out-of-type value in its own text",
          "[util][refusal]") {
  const check::NumericDomain hop = check::integer_domain_of<int>(1);
  const std::string hop_text = "hop_length must be an integer in [1, 2147483647]";
  CHECK(refusal_text([&] { check::within(2.5, hop, "hop_length"); }) == hop_text);
  CHECK(refusal_text([&] { check::within(std::nan(""), hop, "hop_length"); }) == hop_text);
  CHECK(refusal_text([&] { check::within(4294967296.0, hop, "hop_length"); }) == hop_text);
  CHECK(refusal_text([&] { check::within(0.0, hop, "hop_length"); }) == hop_text);
  CHECK(refusal_text([&] { check::within(512.0, hop, "hop_length"); }).empty());
  // The integer helper states the same domain the descriptor does.
  CHECK(refusal_text([] { check::integer_at_least(0, 1, "hop_length"); }) == hop_text);

  const check::NumericDomain position = check::integer_domain_of<int64_t>(0);
  CHECK(refusal_text([&] { check::within(-1.0, position, "start"); }) ==
        "start must be an integer of at least 0");

  const check::NumericDomain ratio = check::real_domain(0.0, false, 1.0, true);
  CHECK(refusal_text([&] { check::within(0.0, ratio, "ratio"); }) ==
        "ratio must be finite and in (0, 1]");
  CHECK(refusal_text([&] { check::within(0.5, ratio, "ratio"); }).empty());
  CHECK(refusal_text([&] { check::within(1e300, check::float_domain(), "gain"); }) ==
        std::string("gain ") + refusal::kFloatRangePredicate);
  CHECK(refusal_text([&] { check::within(INFINITY, check::NumericDomain{}, "t"); }) ==
        "t must be finite");

  try {
    check::within(2.5, check::real_domain_above(3.0, false), "x");
    FAIL("refused nothing");
  } catch (const sonare::SonareParameterRefusal& e) {
    REQUIRE(e.domain());
    CHECK(*e.domain()->lo == 3.0);
    CHECK_FALSE(e.domain()->lo_included);
    CHECK_FALSE(e.domain()->hi);
  }
}

TEST_CASE("a domain's sentinel comes off the end it sits on and leaves the checks alone",
          "[util][parameter_checks]") {
  constexpr check::NumericDomain counts = check::with_sentinel(check::integer_domain_of<int>(0), 0);
  constexpr check::NumericDomain given_counts = check::given_domain(counts);
  static_assert(*given_counts.lo == 1.0 && given_counts.lo_included);
  static_assert(*given_counts.hi == *counts.hi);

  constexpr check::NumericDomain top = check::with_sentinel(check::integer_domain(1, 16), 16);
  static_assert(*check::given_domain(top).hi == 15.0);

  constexpr check::NumericDomain ceiling =
      check::with_sentinel(check::real_domain_above(0.0, true), 0.0);
  constexpr check::NumericDomain given_ceiling = check::given_domain(ceiling);
  static_assert(*given_ceiling.lo == 0.0 && !given_ceiling.lo_included);

  constexpr check::NumericDomain unset = check::with_sentinel(check::integer_domain(1, 127), 0);
  constexpr check::NumericDomain given_unset = check::given_domain(unset);
  static_assert(*given_unset.lo == 1.0 && *given_unset.hi == 127.0);

  constexpr check::NumericDomain plain = check::integer_domain(1, 8);
  static_assert(!check::given_domain(plain).sentinel);

  CHECK_THROWS_AS(check::given_domain(check::with_sentinel(check::integer_domain(0, 8), 4)),
                  std::logic_error);
  CHECK_NOTHROW(check::integer_within(0, counts, "k"));
  CHECK(refusal_text([&] { check::integer_within(0, unset, "fixed_velocity"); }) ==
        "fixed_velocity must be an integer in [1, 127]");
}

TEST_CASE("a presence token renders as NULL in C and in each surface's own terms",
          "[util][refusal]") {
  using sonare::PresenceSpelling;
  using sonare::SonareParameterRefusal;
  const auto relation = [](std::vector<std::string> subjects, const std::string& text) {
    try {
      check::refuse_relation(std::move(subjects), text);
    } catch (const SonareParameterRefusal& e) {
      return e;
    }
    FAIL("refuse_relation returned");
    throw std::logic_error("unreachable");
  };
  struct Row {
    std::vector<std::string> subjects;
    std::string pattern;
    const char* c;
    const char* python;
    const char* js;
  };
  const std::string must_be_absent = refusal::kMustBeAbsent;
  const std::string must_be_present = refusal::kMustBePresent;
  const std::string is_absent = refusal::kIsAbsent;
  const std::string is_present = refusal::kIsPresent;
  const std::vector<Row> rows = {
      {{"a", "mode"},
       "{0} " + must_be_absent + " unless {1} is chain",
       "a must be NULL unless mode is chain",
       "a must be None unless mode is chain",
       "a must be omitted unless mode is chain"},
      {{"a", "mode"},
       "{0} " + must_be_present + " when {1} is chain",
       "a must not be NULL when mode is chain",
       "a must not be None when mode is chain",
       "a must be given when mode is chain"},
      {{"sync", "ms"},
       "{1} " + must_be_absent + " when {0} is tempoSync",
       "ms must be NULL when sync is tempoSync",
       "ms must be None when sync is tempoSync",
       "ms must be omitted when sync is tempoSync"},
      {{"a", "b"},
       std::string("{0} and {1} ") + refusal::kMustNotBothBeAbsent,
       "a and b must not both be NULL",
       "a and b must not both be None",
       "a and b must not both be omitted"},
      {{"a", "b"},
       "{0} " + must_be_present + " when {1} " + is_present,
       "a must not be NULL when b is not NULL",
       "a must not be None when b is not None",
       "a must be given when b is given"},
      {{"a", "b", "c"},
       "{0} and {1} must be 0 when {2} " + is_absent,
       "a and b must be 0 when c is NULL",
       "a and b must be 0 when c is None",
       "a and b must be 0 when c is omitted"},
      {{"values", "count"},
       refusal::kNonNullWhenCountRelation,
       "values must not be NULL when count is not 0",
       "values must not be None when count is not 0",
       "values must be given when count is not 0"},
  };
  for (const Row& row : rows) {
    const SonareParameterRefusal e = relation(row.subjects, row.pattern);
    CHECK(std::string(e.what()) == row.c);
    CHECK(e.text(PresenceSpelling::kC) == row.c);
    CHECK(e.text(PresenceSpelling::kPython) == row.python);
    CHECK(e.text(PresenceSpelling::kJs) == row.js);
    // The token stays in the structured refusal, so a respelled one still renders per surface.
    CHECK(e.predicate() == row.pattern);
    const SonareParameterRefusal respelled =
        e.respelled(e.code(), [](const std::string& s) { return s + "X"; }, true);
    CHECK(respelled.predicate() == row.pattern);
    CHECK(respelled.text(PresenceSpelling::kJs).find("NULL") == std::string::npos);
  }

  const std::string non_null = refusal_text([] { check::non_null(nullptr, "out"); });
  CHECK(non_null == "out must not be NULL");
  try {
    check::non_null(nullptr, "out");
  } catch (const SonareParameterRefusal& e) {
    CHECK(e.predicate() == must_be_present);
    CHECK(e.text(PresenceSpelling::kPython) == "out must not be None");
    CHECK(e.text(PresenceSpelling::kJs) == "out must be given");
  }
  // Brace text that names no subject and no token is kept as written.
  CHECK(relation({"a"}, "{0} {unknown} {7}").text(PresenceSpelling::kJs) == "a {unknown} {7}");
}

TEST_CASE("a refusal requiring a value left out or supplied is a shape error", "[util][refusal]") {
  using sonare::SonareParameterRefusal;
  const auto caught = [](const std::function<void()>& call) {
    try {
      call();
    } catch (const SonareParameterRefusal& e) {
      return e;
    }
    FAIL("the call returned");
    throw std::logic_error("unreachable");
  };
  const std::string must_be_absent = refusal::kMustBeAbsent;
  const std::string must_be_present = refusal::kMustBePresent;
  const std::string is_absent = refusal::kIsAbsent;
  const std::string is_present = refusal::kIsPresent;
  struct Row {
    std::vector<std::string> subjects;
    std::string pattern;
    bool shape;
  };
  const std::vector<Row> rows = {
      {{"a", "mode"}, "{0} " + must_be_absent + " unless {1} is chain", true},
      {{"a", "mode"}, "{0} " + must_be_present + " when {1} is chain", true},
      {{"sync", "ms"}, "{1} " + must_be_absent + " when {0} is tempoSync", true},
      {{"a", "b"}, std::string("{0} and {1} ") + refusal::kMustNotBothBeAbsent, true},
      {{"a", "b"}, "{0} " + must_be_present + " when {1} " + is_present, true},
      // Presence only in the condition: the requirement is a value, so a domain refusal.
      {{"a", "b", "c"}, "{0} and {1} must be 0 when {2} " + is_absent, false},
  };
  for (const Row& row : rows) {
    CAPTURE(row.pattern);
    const SonareParameterRefusal e =
        caught([&] { check::refuse_relation(row.subjects, row.pattern); });
    CHECK(e.shape_error() == row.shape);
    CHECK(e.form() == SonareParameterRefusal::Form::Relation);
    CHECK(e.code() == sonare::ErrorCode::InvalidParameter);
    // A respelled refusal keeps its text, so it keeps the verdict.
    CHECK(e.respelled(
               e.code(), [](const std::string& s) { return s + "X"; }, true)
              .shape_error() == row.shape);
  }

  const SonareParameterRefusal non_null = caught([] { check::non_null(nullptr, "out"); });
  CHECK(non_null.shape_error());
  CHECK(non_null.form() == SonareParameterRefusal::Form::Predicate);
  CHECK(non_null.code() == sonare::ErrorCode::InvalidParameter);
  CHECK(std::string(non_null.what()) == "out must not be NULL");

  const SonareParameterRefusal when_count =
      caught([] { check::non_null_when_count(nullptr, 3, "values", "count"); });
  CHECK(when_count.shape_error());
  CHECK(when_count.code() == sonare::ErrorCode::InvalidParameter);
  CHECK(std::string(when_count.what()) == "values must not be NULL when count is not 0");

  // A value predicate with no presence token stays a domain refusal.
  CHECK_FALSE(caught([] { check::finite(std::nan(""), "x"); }).shape_error());
  CHECK_FALSE(caught([] { check::refuse("x", "must be odd"); }).shape_error());
}

TEST_CASE("element_count refuses the scaling argument once the product passes its limit",
          "[util][parameter_checks]") {
  CHECK(check::element_count(1000u, 1000u, 1'000'000u, "hop_length", "the framed output") ==
        1'000'000u);
  CHECK(refusal_text([] {
          (void)check::element_count(1000u, 1001u, 1'000'000u, "hop_length", "the framed output");
        }) == "hop_length must keep the framed output within 1000000 elements");
  // At the wasm32 width the size type binds before an unlimited limit does, and the text says so.
  CHECK(refusal_text([] {
          (void)check::element_count<std::uint32_t>(65536u, 65536u, UINT64_MAX, "n_components",
                                                    "the activation matrix");
        }) == "n_components must keep the activation matrix within 4294967295 elements");
  CHECK(check::element_count<std::uint32_t>(65536u, 65535u, UINT64_MAX, "n_components",
                                            "the activation matrix") == 65536u * 65535u);
}
