#pragma once

/// @file refusal_predicates.h
/// @brief The predicate texts a value refusal states its domain in.
/// @details A refusal reads "<subject> <predicate>". These are the only spellings of a numeric
///          domain: an interval names both bounds, with "(" or ")" for an exclusive end, and a
///          one-sided domain names its bound. A bound is written in its shortest exact decimal.

#include <cmath>
#include <cstdlib>
#include <string>

#include "util/number_format.h"

namespace sonare::util::refusal {

/// @brief @p text with its exponent written bare: "3.4028235e+38" -> "3.4028235e38", "1e-05" ->
/// "1e-5".
inline std::string bare_exponent(std::string text) {
  const std::string::size_type e = text.find('e');
  if (e == std::string::npos) return text;
  std::string::size_type digits = e + 1;
  if (digits < text.size() && text[digits] == '+') text.erase(digits, 1);
  if (digits < text.size() && text[digits] == '-') ++digits;
  while (digits + 1 < text.size() && text[digits] == '0') text.erase(digits, 1);
  return text;
}

/// @brief The shortest decimal spelling of @p value that reads back as the same number.
/// @details A bound a 32-bit float holds exactly is written as that float's shortest decimal, so
///          a float limit such as 0.98f reads "0.98" whichever width the check compared in.
inline std::string bound_text(double value) {
  constexpr int kMinDigits = 6;
  constexpr int kMaxFloatDigits = 9;
  constexpr int kMaxDigits = 17;
  const auto narrowed = static_cast<float>(value);
  if (std::isfinite(value) && static_cast<double>(narrowed) == value) {
    for (int digits = kMinDigits; digits <= kMaxFloatDigits; ++digits) {
      std::string text = format_general(value, digits);
      if (std::strtof(text.c_str(), nullptr) == narrowed) return bare_exponent(text);
    }
  }
  for (int digits = kMinDigits; digits < kMaxDigits; ++digits) {
    std::string text = format_general(value, digits);
    if (std::strtod(text.c_str(), nullptr) == value) return bare_exponent(text);
  }
  return bare_exponent(format_general(value, kMaxDigits));
}

/// @brief Which way a computed bound with no short decimal is rounded: inward, so the printed
///        bound is itself accepted.
enum class Inward { Up, Down };

/// Significant digits a computed bound is printed at when it has no short exact decimal.
inline constexpr int kComputedBoundDigits = 4;

/// @brief A bound derived at run time (a ratio scaled by a pitch, say), written as a value the
///        caller can type: its exact decimal when that fits kComputedBoundDigits, otherwise
///        rounded inward at that precision.
inline std::string bound_text(double value, Inward toward) {
  if (!std::isfinite(value) || value == 0.0) return bound_text(value);
  const std::string short_text = format_general(value, kComputedBoundDigits);
  const auto narrowed = static_cast<float>(value);
  const bool float_exact = static_cast<double>(narrowed) == value;
  if (float_exact ? std::strtof(short_text.c_str(), nullptr) == narrowed
                  : std::strtod(short_text.c_str(), nullptr) == value) {
    return bare_exponent(short_text);
  }
  const double scale =
      std::pow(10.0, kComputedBoundDigits - 1 - std::floor(std::log10(std::fabs(value))));
  const double scaled = value * scale;
  const double kept = toward == Inward::Up ? std::ceil(scaled) : std::floor(scaled);
  return bound_text(kept / scale);
}

/// @brief "must be finite and in [lo, hi]" for bounds computed at run time, each rounded inward.
inline std::string computed_range_predicate(double lo, double hi) {
  return "must be finite and in [" + bound_text(lo, Inward::Up) + ", " +
         bound_text(hi, Inward::Down) + "]";
}

/// @brief "must be finite and in [lo, hi]".
inline std::string range_predicate(double lo, double hi) {
  return "must be finite and in [" + bound_text(lo) + ", " + bound_text(hi) + "]";
}

/// @brief "must be finite and in (lo, hi)", with "[" / "]" for an end the domain includes.
inline std::string interval_predicate(double lo, bool lo_included, double hi, bool hi_included) {
  return std::string("must be finite and in ") + (lo_included ? "[" : "(") + bound_text(lo) + ", " +
         bound_text(hi) + (hi_included ? "]" : ")");
}

/// @brief "must be finite and at least X".
inline std::string at_least_predicate(double bound) {
  return "must be finite and at least " + bound_text(bound);
}

/// @brief "must be finite and greater than X".
inline std::string greater_than_predicate(double bound) {
  return "must be finite and greater than " + bound_text(bound);
}

/// @brief "must be finite and at most X".
inline std::string at_most_predicate(double bound) {
  return "must be finite and at most " + bound_text(bound);
}

/// @brief "must be finite and less than X".
inline std::string less_than_predicate(double bound) {
  return "must be finite and less than " + bound_text(bound);
}

/// @brief The domain of a value held as a 32-bit float, for one with no narrower declared range.
inline constexpr const char* kFloatRangePredicate =
    "must be finite and in [-3.4028235e38, 3.4028235e38]";

/// @brief The domain of a value held as a double, for one with no narrower declared range.
inline constexpr const char* kFiniteDoublePredicate = "must be finite";

/// @brief An integer bound in plain digits; a bound past the 64-bit range keeps bound_text.
inline std::string integer_bound_text(double value) {
  constexpr double kInt64Limit = 9223372036854775808.0;
  if (std::isfinite(value) && std::nearbyint(value) == value && value > -kInt64Limit &&
      value < kInt64Limit) {
    return std::to_string(static_cast<long long>(value));
  }
  return bound_text(value);
}

/// @brief "must be an integer in [lo, hi]".
inline std::string integer_range_predicate(double lo, double hi) {
  return "must be an integer in [" + integer_bound_text(lo) + ", " + integer_bound_text(hi) + "]";
}

/// @brief "must be a power of two in [lo, hi]".
inline std::string power_of_two_predicate(long long lo, long long hi) {
  return "must be a power of two in [" + std::to_string(lo) + ", " + std::to_string(hi) + "]";
}

/// @brief "must be an even integer in [lo, hi]".
inline std::string even_integer_predicate(long long lo, long long hi) {
  return "must be an even integer in [" + std::to_string(lo) + ", " + std::to_string(hi) + "]";
}

/// @brief "must be an odd integer in [lo, hi]".
inline std::string odd_integer_predicate(long long lo, long long hi) {
  return "must be an odd integer in [" + std::to_string(lo) + ", " + std::to_string(hi) + "]";
}

/// @brief "must be one of a, b, c", listing @p values in the order given (ascending).
template <typename Values>
std::string one_of_predicate(const Values& values) {
  std::string text = "must be one of ";
  bool first = true;
  for (const auto& value : values) {
    if (!first) text += ", ";
    text += bound_text(static_cast<double>(value));
    first = false;
  }
  return text;
}

/// @brief "must be an integer of at least N", for a 64-bit position or count.
inline std::string integer_at_least_predicate(long long bound) {
  return "must be an integer of at least " + std::to_string(bound);
}

/// @brief "must name a declared value".
inline constexpr const char* kDeclaredValuePredicate = "must name a declared value";

/// @brief "must not be NULL", for a pointer the call reads or writes.
inline constexpr const char* kNonNullPredicate = "must not be NULL";

/// @brief "must not be empty", for a string or list the call needs at least one element of.
inline constexpr const char* kNonEmptyPredicate = "must not be empty";

/// @brief "has no measurable loudness" / "has no measurable level", for audio whose measured level
/// is -inf (silent, or below the absolute gate) or not measured, when a gain is derived from it.
inline constexpr const char* kNoMeasurableLoudnessPredicate = "has no measurable loudness";
inline constexpr const char* kNoMeasurableLevelPredicate = "has no measurable level";

/// @brief "must be the id of <thing> in this <owner>"; @p thing carries its article.
inline std::string known_id_predicate(const std::string& thing, const std::string& owner) {
  return "must be the id of " + thing + " in this " + owner;
}

/// @brief "{0} must not be NULL when {1} is not 0", for a pointer read only while a count is set.
inline constexpr const char* kNonNullWhenCountRelation = "{0} must not be NULL when {1} is not 0";

}  // namespace sonare::util::refusal
