#include "util/json.h"

#include <catch2/catch_test_macros.hpp>
#include <clocale>
#include <limits>
#include <locale>
#include <stdexcept>
#include <string>
#include <utility>

#include "util/base64.h"

TEST_CASE("util json parses and serializes nested values", "[json]") {
  const auto value = sonare::util::json::parse(
      "{\"name\":\"voice\",\"items\":[1,true,null,{\"x\":-2.5e1}],\"text\":\"a\\n"
      "\\u3053\\u3093\\u306b\\u3061\\u306f\"}");

  REQUIRE(value.is_object());
  REQUIRE(value["name"].as_string() == "voice");
  REQUIRE(value["items"].as_array().size() == 4);
  REQUIRE(value["items"][0].as_number() == 1.0);
  REQUIRE(value["items"][1].as_bool());
  REQUIRE(value["items"][2].is_null());
  REQUIRE(value["items"][3]["x"].as_number() == -25.0);
  REQUIRE(value["text"].as_string().find("こんにちは") != std::string::npos);

  const auto reparsed = sonare::util::json::parse(sonare::util::json::dump(value));
  REQUIRE(reparsed["items"][3]["x"].as_number() == -25.0);
}

TEST_CASE("util json decodes surrogate pairs and reports invalid input", "[json]") {
  const auto value = sonare::util::json::parse("{\"emoji\":\"\\ud83d\\ude00\"}");
  REQUIRE(value["emoji"].as_string() == "\xF0\x9F\x98\x80");

  REQUIRE_THROWS_AS(sonare::util::json::parse("{\"x\":"), sonare::util::json::JsonError);
  REQUIRE_THROWS_AS(sonare::util::json::parse("[[[", 2), sonare::util::json::JsonError);
  REQUIRE_THROWS_AS(sonare::util::json::parse("\"\\uD800\""), sonare::util::json::JsonError);
}

TEST_CASE("util json escapes the full control-character range", "[json]") {
  // Build a payload that contains every byte in [0x00, 0x1F] inside a string
  // and verify dump() emits \uXXXX escapes for each, never raw control bytes.
  std::string raw;
  for (unsigned char b = 0; b < 0x20; ++b) raw.push_back(static_cast<char>(b));
  const auto escaped = sonare::util::json::escape_string(raw);
  for (unsigned char b : escaped) {
    // Ensure no control byte leaked through unescaped.
    REQUIRE(b >= 0x20);
  }
  // Common escapes must use the short forms, not \uXXXX.
  REQUIRE(escaped.find("\\b") != std::string::npos);
  REQUIRE(escaped.find("\\f") != std::string::npos);
  REQUIRE(escaped.find("\\n") != std::string::npos);
  REQUIRE(escaped.find("\\r") != std::string::npos);
  REQUIRE(escaped.find("\\t") != std::string::npos);
  // Remaining bytes (e.g., 0x01) must use \u00XX form.
  REQUIRE(escaped.find("\\u0001") != std::string::npos);
  REQUIRE(escaped.find("\\u0000") != std::string::npos);
}

TEST_CASE("util json rejects nesting beyond max_depth", "[json]") {
  // 5-deep nested array; max_depth=3 must reject before reaching the bottom.
  REQUIRE_THROWS_AS(sonare::util::json::parse("[[[[[]]]]]", 3), sonare::util::json::JsonError);
  // A shallow document must still succeed under the same limit.
  REQUIRE_NOTHROW(sonare::util::json::parse("[[1]]", 3));
}

TEST_CASE("util json resource limits reject before unbounded tree and string growth", "[json]") {
  using sonare::util::json::JsonResourceError;
  using sonare::util::json::ParseResourceLimits;

  REQUIRE_NOTHROW(sonare::util::json::parse_with_limits("[1,2]", 128, {3u, 0u}));
  REQUIRE_THROWS_AS(sonare::util::json::parse_with_limits("[1,2,3]", 128, {3u, 0u}),
                    JsonResourceError);

  REQUIRE_NOTHROW(
      sonare::util::json::parse_with_limits(R"({"a":"bc"})", 128, ParseResourceLimits{3u, 3u}));
  REQUIRE_THROWS_AS(
      sonare::util::json::parse_with_limits(R"({"a":"bcd"})", 128, ParseResourceLimits{3u, 3u}),
      JsonResourceError);
}

TEST_CASE("base64 decode enforces its output budget before reserve", "[json][resource_limit]") {
  std::vector<uint8_t> decoded;
  REQUIRE(sonare::base64_decode("AQI=", &decoded, 2u));
  REQUIRE(decoded == std::vector<uint8_t>{1u, 2u});
  REQUIRE_FALSE(sonare::base64_decode("AQI=", &decoded, 1u));
  REQUIRE(decoded.empty());
}

TEST_CASE("util json round-trips small and large numbers", "[json]") {
  // After the dump_value precision fix (setprecision 15 → max_digits10 = 17),
  // every finite double must roundtrip losslessly via dump/parse. The previous
  // 15-digit precision corrupted filter coefficients that needed full double
  // resolution (e.g. K-weighting denominators, RT60 feedback gains).
  for (double v : {0.0, 1.0e-300, 1.0e+300, 1.23456789012345,
                   // Float values whose 17-digit round-trip differs from their
                   // 15-digit roundtrip — these were silently rounded before.
                   0.1, 0.2, 0.3, 1.0 / 3.0, 1.0 - 1.0 / 7.0, std::numeric_limits<double>::min(),
                   std::numeric_limits<double>::max()}) {
    const auto reparsed =
        sonare::util::json::parse(sonare::util::json::dump(sonare::util::json::Value(v)));
    REQUIRE(reparsed.is_number());
    REQUIRE(reparsed.as_number() == v);
  }
}

namespace {

/// A comma-decimal numeric facet: "," as the decimal point, "." as the
/// thousands separator, three-digit grouping.
///
/// Used instead of requiring a named locale to be generated on the host. The
/// facet is what an iostream consults, so it reproduces the hostile-host
/// behavior exactly -- an unimbued `istringstream("1.5") >> double` yields 15
/// and leaves the stream in a failed state, and an unimbued `ostringstream <<
/// 1.5` writes "1,5" -- on every platform, with nothing to install.
class CommaDecimalPunct : public std::numpunct<char> {
 protected:
  char do_decimal_point() const override { return ','; }
  char do_thousands_sep() const override { return '.'; }
  std::string do_grouping() const override { return "\3"; }
};

/// Installs a comma-decimal locale for the lifetime of the object.
///
/// Both the global C++ locale and LC_NUMERIC are switched, and only the first
/// of them reaches this header: `std::istringstream` / `std::ostringstream`
/// carry the global C++ locale, not the C one, so a test that calls only
/// `std::setlocale` cannot fail no matter how the number is formatted.
/// LC_NUMERIC is switched alongside it so any C-library formatting a caller
/// mixes in is covered too, and it only moves when a named locale is available.
class CommaDecimalLocaleGuard {
 public:
  CommaDecimalLocaleGuard() : saved_global_(std::locale()) {
    const char* previous = std::setlocale(LC_NUMERIC, nullptr);
    saved_numeric_ = previous ? previous : "C";
    // Prefer a real installed locale, which moves LC_NUMERIC as well. Both
    // glibc and macOS ship de_DE.UTF-8, but a slim CI image may not have
    // generated it, so fall back to the synthetic facet rather than skipping.
    for (const char* tag : {"de_DE.UTF-8", "de_DE.utf8", "de_DE"}) {
      try {
        std::locale::global(std::locale(tag));
        std::setlocale(LC_NUMERIC, tag);
        return;
      } catch (const std::runtime_error&) {
        continue;
      }
    }
    std::locale::global(std::locale(std::locale::classic(), new CommaDecimalPunct));
  }

  ~CommaDecimalLocaleGuard() {
    std::locale::global(saved_global_);
    std::setlocale(LC_NUMERIC, saved_numeric_.c_str());
  }

  CommaDecimalLocaleGuard(const CommaDecimalLocaleGuard&) = delete;
  CommaDecimalLocaleGuard& operator=(const CommaDecimalLocaleGuard&) = delete;

 private:
  std::locale saved_global_;
  std::string saved_numeric_;
};

}  // namespace

TEST_CASE("util json parse and dump are locale-independent", "[json][locale]") {
  // DAW plugin hosts sometimes run under a comma-decimal locale. Both the
  // parser and dump() build a stream and imbue the classic locale so "." stays
  // the decimal separator either way; the guard below makes that load-bearing.
  // Without the imbue, parse reads {"a":1.5} as 15 and dump writes 1.5 as
  // "1,5" -- which is not a parse error downstream, just a different document.
  CommaDecimalLocaleGuard hostile_locale;

  const auto value = sonare::util::json::parse("{\"a\":1.5,\"b\":-3.25e-2,\"c\":[0.125,2.5]}");
  REQUIRE(value["a"].as_number() == 1.5);
  REQUIRE(value["b"].as_number() == -0.0325);
  REQUIRE(value["c"][0].as_number() == 0.125);
  REQUIRE(value["c"][1].as_number() == 2.5);

  // Exact text, not just "contains a dot": a comma decimal separator inside an
  // object or array is swallowed by the member separator and would otherwise
  // read as a well-formed document with different values.
  REQUIRE(sonare::util::json::dump(sonare::util::json::Value(1.5)) == "1.5");

  sonare::util::json::Object object;
  object.emplace("a", sonare::util::json::Value(1.5));
  object.emplace("b", sonare::util::json::Value(-0.0325));
  const auto dumped = sonare::util::json::dump(sonare::util::json::Value(std::move(object)));
  const auto reparsed = sonare::util::json::parse(dumped);
  REQUIRE(reparsed.size() == 2);
  REQUIRE(reparsed["a"].as_number() == 1.5);
  REQUIRE(reparsed["b"].as_number() == -0.0325);
}

TEST_CASE("util json rejects malformed numeric tokens", "[json]") {
  // The number grammar requires fraction/exponent digits after their leading
  // chars; bare "1." or "1e" must fail rather than silently parse as 1.0.
  REQUIRE_THROWS_AS(sonare::util::json::parse("{\"x\":1.}"), sonare::util::json::JsonError);
  REQUIRE_THROWS_AS(sonare::util::json::parse("{\"x\":1e}"), sonare::util::json::JsonError);
  REQUIRE_THROWS_AS(sonare::util::json::parse("{\"x\":01}"), sonare::util::json::JsonError);
  REQUIRE_THROWS_AS(sonare::util::json::parse("{\"x\":NaN}"), sonare::util::json::JsonError);
}

TEST_CASE("util json object duplicates take last-write-wins", "[json]") {
  // std::map::operator[] semantics: assigning twice overwrites. Confirm the
  // parser behaves the same way (no surprise "first wins" or throw).
  const auto value = sonare::util::json::parse("{\"a\":1,\"a\":2,\"a\":3}");
  REQUIRE(value["a"].as_number() == 3.0);
}

TEST_CASE("util json parse_strict rejects duplicate object keys", "[json]") {
  // parse_strict opts into duplicate-key detection so schema-driven validators
  // (preset JSON, EQ band JSON) can fail fast on ambiguous input rather than
  // silently accepting the last-write value.
  REQUIRE_THROWS_AS(sonare::util::json::parse_strict("{\"a\":1,\"a\":2}"),
                    sonare::util::json::JsonError);
  REQUIRE_NOTHROW(sonare::util::json::parse_strict("{\"a\":1,\"b\":2}"));
}

TEST_CASE("util json serializes non-finite numbers as null", "[json]") {
  // RFC 8259 has no representation for NaN/Inf; dumping them would emit
  // implementation-defined strings ("nan"/"inf") that no JSON parser accepts,
  // including our own. Instead of throwing (which made it impossible to store a
  // legitimately non-finite value such as a -inf LUFS/true-peak reading for a
  // silent input), the serializer emits the valid JSON token `null`, which
  // round-trips cleanly through parse().
  REQUIRE(sonare::util::json::dump(
              sonare::util::json::Value(std::numeric_limits<double>::quiet_NaN())) == "null");
  REQUIRE(sonare::util::json::dump(
              sonare::util::json::Value(std::numeric_limits<double>::infinity())) == "null");
  REQUIRE(sonare::util::json::dump(
              sonare::util::json::Value(-std::numeric_limits<double>::infinity())) == "null");

  // Embedded non-finite inside a container is also rendered as null (whole
  // document stays valid JSON) and parses back to a null member.
  sonare::util::json::Object obj;
  obj["x"] = sonare::util::json::Value(-std::numeric_limits<double>::infinity());
  const std::string dumped = sonare::util::json::dump(sonare::util::json::Value(std::move(obj)));
  REQUIRE(dumped == "{\"x\":null}");
  const auto reparsed = sonare::util::json::parse(dumped);
  REQUIRE(reparsed["x"].is_null());
}

TEST_CASE("util json tolerates a UTF-8 BOM at the document head", "[json]") {
  // Windows editors and a few CLI tools emit JSON prefixed with a UTF-8 BOM.
  // The legacy code path rejected this with the unhelpful "unexpected JSON
  // character" error; the parser now silently skips the BOM.
  const std::string bom = "\xEF\xBB\xBF";
  const auto value = sonare::util::json::parse(bom + "{\"a\":1}");
  REQUIRE(value["a"].as_number() == 1.0);
  // BOM only valid at the very start — embedded BOM bytes inside a string must
  // still be preserved (they're valid UTF-8 content there).
  const auto value2 = sonare::util::json::parse("{\"a\":\"\\u00e9\"}");
  REQUIRE(value2["a"].as_string() == "\xC3\xA9");  // é (U+00E9) as UTF-8
}

TEST_CASE("util json dump_pretty emits indented, parseable output", "[json]") {
  // dump_pretty is intended for human-readable schema/example files. Its output
  // must still round-trip through parse() unchanged.
  sonare::util::json::Object obj;
  obj["name"] = std::string("voice");
  sonare::util::json::Array items;
  items.push_back(1.0);
  items.push_back(2.5);
  obj["items"] = std::move(items);
  const auto pretty = sonare::util::json::dump_pretty(sonare::util::json::Value(std::move(obj)), 2);
  REQUIRE(pretty.find('\n') != std::string::npos);
  REQUIRE(pretty.find("  ") != std::string::npos);  // at least one indent
  const auto reparsed = sonare::util::json::parse(pretty);
  REQUIRE(reparsed["name"].as_string() == "voice");
  REQUIRE(reparsed["items"][0].as_number() == 1.0);
  REQUIRE(reparsed["items"][1].as_number() == 2.5);
  // Empty objects/arrays stay compact.
  REQUIRE(sonare::util::json::dump_pretty(sonare::util::json::Value(sonare::util::json::Array{}),
                                          2) == "[]");
  REQUIRE(sonare::util::json::dump_pretty(sonare::util::json::Value(sonare::util::json::Object{}),
                                          2) == "{}");
}
