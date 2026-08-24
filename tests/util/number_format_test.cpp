#include "util/number_format.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <locale>
#include <random>
#include <sstream>
#include <string>

// The formatters replaced `std::ostringstream` / `std::istringstream` so the
// WASM module does not have to carry the std::locale facet set. Their contract
// is that the text is unchanged, so the reference implementation in these tests
// is the stream itself: anything that stops matching it is a regression, not a
// new rounding preference.

namespace {

std::string stream_general(double value, int precision) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(precision) << value;
  return out.str();
}

std::string stream_fixed(double value, int decimals, bool force_sign) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  if (force_sign) out << std::showpos;
  out << std::fixed << std::setprecision(decimals) << value;
  return out.str();
}

bool stream_parse(const std::string& text, double* out) {
  std::string copy = text;
  std::istringstream in(copy);
  in.imbue(std::locale::classic());
  double parsed = 0.0;
  in >> parsed;
  if (!in || in.peek() != std::char_traits<char>::eof()) return false;
  *out = parsed;
  return true;
}

bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

}  // namespace

TEST_CASE("number formatting matches stream output bit for bit", "[number_format]") {
  const double corner[] = {0.0,
                           -0.0,
                           1.0,
                           -1.0,
                           0.5,
                           0.1,
                           1.0 / 3.0,
                           1e-5,
                           1e21,
                           1e-21,
                           1e300,
                           1e-300,
                           123456789.123456789,
                           -0.000001234,
                           2.2250738585072014e-308,
                           1.7976931348623157e308};

  std::mt19937_64 rng(0x5EED);
  std::uniform_real_distribution<double> audio_range(-1e6, 1e6);

  const auto check = [](double value) {
    for (const int precision : {6, 15, 17}) {
      REQUIRE(sonare::util::format_general(value, precision) == stream_general(value, precision));
    }
    for (const int decimals : {0, 1, 2, 3, 6}) {
      REQUIRE(sonare::util::format_fixed(value, decimals) == stream_fixed(value, decimals, false));
      REQUIRE(sonare::util::format_fixed(value, decimals, /*force_sign=*/true) ==
              stream_fixed(value, decimals, true));
    }
  };

  for (const double value : corner) check(value);

  for (int i = 0; i < 20000; ++i) {
    const std::uint64_t bits = rng();
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof value);
    if (value != value || value > 1e308 || value < -1e308) continue;  // NaN / infinity
    check(value);
  }
  for (int i = 0; i < 20000; ++i) check(audio_range(rng));
}

TEST_CASE("number parsing matches stream extraction bit for bit", "[number_format]") {
  std::mt19937_64 rng(0xB0A7);
  std::uniform_real_distribution<double> audio_range(-1e6, 1e6);

  const auto check = [](double value) {
    const std::string text = stream_general(value, 17);
    double from_stream = 0.0;
    double from_helper = 0.0;
    const bool stream_ok = stream_parse(text, &from_stream);
    const bool helper_ok =
        sonare::util::parse_double(text.data(), text.data() + text.size(), &from_helper);
    // A subnormal is the one documented divergence: the stream rejects it
    // because strtod reports ERANGE, while the text names a representable
    // double and the helper accepts it.
    const bool subnormal = from_helper != 0.0 && from_helper < 2.2250738585072014e-308 &&
                           from_helper > -2.2250738585072014e-308;
    if (subnormal) {
      REQUIRE(helper_ok);
      return;
    }
    REQUIRE(helper_ok == stream_ok);
    if (stream_ok) REQUIRE(same_bits(from_stream, from_helper));
  };

  for (const double value : {0.0, -0.0, 1.0, -1.0, 0.1, 1.0 / 3.0, 1e300, 1e-300,
                             1.7976931348623157e308, 123456789.123456789}) {
    check(value);
  }
  for (int i = 0; i < 20000; ++i) {
    const std::uint64_t bits = rng();
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof value);
    if (value != value || value > 1e308 || value < -1e308) continue;
    check(value);
  }
  for (int i = 0; i < 20000; ++i) check(audio_range(rng));
}

TEST_CASE("number parsing rejects what is not a whole number", "[number_format]") {
  double value = 1.0;
  const auto parse = [&value](const std::string& text) {
    return sonare::util::parse_double(text.data(), text.data() + text.size(), &value);
  };

  REQUIRE_FALSE(parse("1.5x"));
  REQUIRE_FALSE(parse(""));
  REQUIRE_FALSE(parse("abc"));

  // Leading whitespace is skipped, which is what stream extraction did. The
  // JSON parser never hands it any, having matched the number's grammar first.
  REQUIRE(parse(" 1.5"));
  REQUIRE(value == 1.5);

  // Overflow is rejected rather than becoming an infinity, which has no JSON
  // representation and would serialize back out as null.
  value = 1.0;
  REQUIRE_FALSE(parse("1e400"));
  REQUIRE_FALSE(parse("-1e400"));
  REQUIRE(value == 1.0);

  // Underflow to zero and a subnormal are accepted: strtod rounds both
  // correctly, and rejecting them made a document this codebase had itself
  // produced fail to parse back.
  REQUIRE(parse("1e-400"));
  REQUIRE(value == 0.0);
  REQUIRE(parse("4.9406564584124654e-324"));
  REQUIRE(value > 0.0);
  REQUIRE(value < 2.2250738585072014e-308);
}

TEST_CASE("to_text renders a value the way an unformatted stream did", "[number_format]") {
  REQUIRE(sonare::util::to_text(42) == "42");
  REQUIRE(sonare::util::to_text(-7) == "-7");
  REQUIRE(sonare::util::to_text(std::size_t{9}) == "9");
  REQUIRE(sonare::util::to_text("name") == "name");
  REQUIRE(sonare::util::to_text(std::string("name")) == "name");
  // Six significant digits in general notation -- what a stream defaults to,
  // and neither what std::to_string nor a fixed format produces.
  REQUIRE(sonare::util::to_text(0.5f) == "0.5");
  REQUIRE(sonare::util::to_text(1.0 / 3.0) == "0.333333");
  REQUIRE(sonare::util::to_text(1234567.0) == "1.23457e+06");
  REQUIRE(sonare::util::to_text(1.0 / 3.0) == stream_general(1.0 / 3.0, 6));
}
