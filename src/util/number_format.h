// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <vector>

namespace sonare::util {

/// @file
/// @brief Locale-independent decimal text for numbers, without `<sstream>`.
///
/// These reproduce what `std::ostringstream` / `std::istringstream` produced for
/// the same value byte for byte: libc++'s `num_put::do_put` builds a printf
/// format string and calls `snprintf`, and `num_get::do_get` calls `sscanf`, so
/// matching the format string matches the text. What they avoid is
/// `std::locale`, whose facet set is instantiated whole the moment any stream is
/// constructed -- roughly 76 KB of the WASM module for number formatting a
/// handful of call sites need.
///
/// A stream carries its own locale (the C++ global locale, "C" unless a program
/// calls `std::locale::global`), while `snprintf` and `strtod` read `LC_NUMERIC`
/// from the C locale, which a plugin host is free to set to one where the
/// decimal separator is a comma. The separator is folded back onto `.` here so
/// the two agree.

namespace detail {

/// @brief The decimal separator the C library will emit and expect.
inline char c_decimal_point() {
  const char* point = std::localeconv()->decimal_point;
  return (point != nullptr && point[0] != '\0') ? point[0] : '.';
}

/// @brief Format into a `std::string`, growing the buffer if it does not fit.
template <typename... Args>
inline std::string formatted(const char* format, Args... args) {
  char stack_buffer[64];
  const int needed = std::snprintf(stack_buffer, sizeof(stack_buffer), format, args...);
  if (needed < 0) return std::string();
  if (static_cast<std::size_t>(needed) < sizeof(stack_buffer)) {
    std::string text(stack_buffer, static_cast<std::size_t>(needed));
    const char point = c_decimal_point();
    if (point != '.') {
      for (char& c : text) {
        if (c == point) c = '.';
      }
    }
    return text;
  }
  // A fixed-notation DBL_MAX runs to well over three hundred characters.
  std::vector<char> heap_buffer(static_cast<std::size_t>(needed) + 1);
  std::snprintf(heap_buffer.data(), heap_buffer.size(), format, args...);
  std::string text(heap_buffer.data(), static_cast<std::size_t>(needed));
  const char point = c_decimal_point();
  if (point != '.') {
    for (char& c : text) {
      if (c == point) c = '.';
    }
  }
  return text;
}

}  // namespace detail

/// @brief Format @p value with @p significant_digits, matching `std::defaultfloat`.
/// @details Equivalent to `out << std::setprecision(significant_digits) << value`
///          on a stream imbued with the classic locale.
inline std::string format_general(double value, int significant_digits) {
  return detail::formatted("%.*g", significant_digits, value);
}

/// @brief Format @p value with @p decimals after the point, matching `std::fixed`.
/// @param force_sign Emit a leading `+` for non-negative values, as `std::showpos`.
inline std::string format_fixed(double value, int decimals, bool force_sign = false) {
  return force_sign ? detail::formatted("%+.*f", decimals, value)
                    : detail::formatted("%.*f", decimals, value);
}

/// @brief Render @p value the way an unformatted `stream << value` rendered it.
/// @details For a diagnostic message assembled from values whose type the caller
///          does not know (a range-check macro, say). A stream's default is six
///          significant digits in general notation for a floating-point value,
///          which is neither what `std::to_string` produces nor what a fixed
///          format produces, so the two are not interchangeable.
/// @{
inline std::string to_text(const char* value) {
  return value != nullptr ? std::string(value) : std::string();
}
inline std::string to_text(const std::string& value) { return value; }
template <typename T, typename = std::enable_if_t<std::is_integral<T>::value>>
inline std::string to_text(T value) {
  return std::to_string(value);
}
template <typename T, typename = std::enable_if_t<std::is_floating_point<T>::value>,
          typename = void>
inline std::string to_text(T value) {
  return format_general(static_cast<double>(value), 6);
}
/// @}

/// @brief Parse the decimal number in [@p first, @p last), independent of `LC_NUMERIC`.
/// @details Requires the whole range to be consumed, and rejects a value that
///          overflows to infinity -- a stream extraction set failbit there, and
///          a non-finite number has no JSON representation, so letting one
///          through would make a value serialize back as `null`.
///
///          One intentional divergence from stream extraction: a subnormal
///          result, and a result that underflows to zero, are accepted. libc++
///          sets failbit for both because `strtod` reports `ERANGE`, but
///          C++17 [facet.num.get.virtuals] ties that to the `strtod` contract,
///          and `strtod` returns the correctly rounded value in both cases. The
///          text names a representable `double`, so rejecting it made a document
///          this header had itself produced fail to parse back.
/// @return false if the text is not a number, has trailing characters, or
///         overflows.
inline bool parse_double(const char* first, const char* last, double* out) {
  if (first == nullptr || out == nullptr || last <= first) return false;
  std::string text(first, static_cast<std::size_t>(last - first));
  const char point = detail::c_decimal_point();
  if (point != '.') {
    for (char& c : text) {
      if (c == '.') c = point;
    }
  }
  char* end = nullptr;
  const double parsed = std::strtod(text.c_str(), &end);
  if (end != text.c_str() + text.size()) return false;
  if (parsed > 1.7976931348623157e308 || parsed < -1.7976931348623157e308 || parsed != parsed) {
    return false;
  }
  *out = parsed;
  return true;
}

}  // namespace sonare::util
