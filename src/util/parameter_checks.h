#pragma once

/// @file parameter_checks.h
/// @brief Per-field domain checks a config validator refuses through.
/// @details Each check refuses with a SonareParameterRefusal naming @p subject — the config
///          member as the validator spells it — in the predicate vocabulary of
///          refusal_predicates.h. A layer that read the member from a caller's key respells the
///          subject (mastering/api/param_map.h, with_caller_keys).

#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "util/exception.h"
#include "util/refusal_predicates.h"

namespace sonare::util::check {

/// @brief Refuses @p value unless it is finite and in [lo, hi].
inline void in_range(double value, double lo, double hi, const char* subject) {
  if (!(std::isfinite(value) && value >= lo && value <= hi)) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                            refusal::range_predicate(lo, hi),
                                            RefusalDomain{lo, hi, true, true});
  }
}

/// @brief in_range for bounds computed at run time; the refusal prints them rounded inward.
inline void in_computed_range(double value, double lo, double hi, const char* subject) {
  if (!(std::isfinite(value) && value >= lo && value <= hi)) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                            refusal::computed_range_predicate(lo, hi),
                                            RefusalDomain{lo, hi, true, true});
  }
}

/// @brief Refuses @p value unless it is finite and in the interval with the given ends.
inline void in_interval(double value, double lo, bool lo_included, double hi, bool hi_included,
                        const char* subject) {
  const bool above = lo_included ? value >= lo : value > lo;
  const bool below = hi_included ? value <= hi : value < hi;
  if (!(std::isfinite(value) && above && below)) {
    throw SonareParameterRefusal::predicate(
        ErrorCode::InvalidParameter, subject,
        refusal::interval_predicate(lo, lo_included, hi, hi_included),
        RefusalDomain{lo, hi, lo_included, hi_included});
  }
}

/// @brief Refuses @p value unless it is finite and at least @p bound.
inline void at_least(double value, double bound, const char* subject) {
  if (!(std::isfinite(value) && value >= bound)) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                            refusal::at_least_predicate(bound),
                                            RefusalDomain{bound, std::nullopt, true, true});
  }
}

/// @brief Refuses @p value unless it is finite and greater than @p bound.
inline void greater_than(double value, double bound, const char* subject) {
  if (!(std::isfinite(value) && value > bound)) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                            refusal::greater_than_predicate(bound),
                                            RefusalDomain{bound, std::nullopt, false, true});
  }
}

/// @brief Refuses @p value unless it is finite and at most @p bound.
inline void at_most(double value, double bound, const char* subject) {
  if (!(std::isfinite(value) && value <= bound)) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                            refusal::at_most_predicate(bound),
                                            RefusalDomain{std::nullopt, bound, true, true});
  }
}

/// @brief Refuses @p value unless it is finite and less than @p bound.
inline void less_than(double value, double bound, const char* subject) {
  if (!(std::isfinite(value) && value < bound)) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                            refusal::less_than_predicate(bound),
                                            RefusalDomain{std::nullopt, bound, true, false});
  }
}

/// @brief Refuses @p value unless it is finite, for a float member with no narrower domain.
inline void finite_float(float value, const char* subject) {
  if (!std::isfinite(value)) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                            refusal::kFloatRangePredicate);
  }
}

/// @brief Refuses @p value unless it is finite, for a double member with no narrower domain.
inline void finite(double value, const char* subject) {
  if (!std::isfinite(value)) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                            refusal::kFiniteDoublePredicate);
  }
}

/// @brief The subject "<array>[<index>].<field>" naming one member of an array element.
inline std::string indexed_subject(const char* array, std::size_t index, const char* field) {
  return std::string(array) + "[" + std::to_string(index) + "]." + field;
}

/// @brief Refuses @p value unless it is in [lo, hi].
inline void integer_in_range(long long value, long long lo, long long hi, const char* subject) {
  if (value < lo || value > hi) {
    throw SonareParameterRefusal::predicate(
        ErrorCode::InvalidParameter, subject,
        refusal::integer_range_predicate(static_cast<double>(lo), static_cast<double>(hi)),
        RefusalDomain{static_cast<double>(lo), static_cast<double>(hi), true, true});
  }
}

/// @brief Refuses @p value unless it is at least @p bound. A type no wider than int states its
///        whole domain ("must be an integer in [N, 2147483647]"), the form a binding's integer
///        reader prints; a 64-bit count states its floor ("must be an integer of at least N").
template <typename T>
inline void integer_at_least(T value, long long bound, const char* subject) {
  static_assert(std::is_integral_v<T>, "integer_at_least takes an integer");
  if (static_cast<long long>(value) >= bound) return;
  if constexpr (sizeof(T) <= sizeof(int)) {
    throw SonareParameterRefusal::predicate(
        ErrorCode::InvalidParameter, subject,
        refusal::integer_range_predicate(static_cast<double>(bound),
                                         static_cast<double>(std::numeric_limits<T>::max())),
        RefusalDomain{static_cast<double>(bound),
                      static_cast<double>(std::numeric_limits<T>::max()), true, true});
  } else {
    throw SonareParameterRefusal::predicate(
        ErrorCode::InvalidParameter, subject, refusal::integer_at_least_predicate(bound),
        RefusalDomain{static_cast<double>(bound), std::nullopt, true, true});
  }
}

/// @brief Refuses @p value unless it is a power of two in [lo, hi].
inline void power_of_two_in_range(long long value, long long lo, long long hi,
                                  const char* subject) {
  if (value < lo || value > hi || value <= 0 || (value & (value - 1)) != 0) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                            refusal::power_of_two_predicate(lo, hi));
  }
}

/// @brief Refuses @p value unless it is an even integer in [lo, hi].
inline void even_integer_in_range(long long value, long long lo, long long hi,
                                  const char* subject) {
  if (value < lo || value > hi || value % 2 != 0) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                            refusal::even_integer_predicate(lo, hi));
  }
}

/// @brief Refuses @p value unless it is an odd integer in [lo, hi].
inline void odd_integer_in_range(long long value, long long lo, long long hi, const char* subject) {
  if (value < lo || value > hi || value % 2 == 0) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                            refusal::odd_integer_predicate(lo, hi));
  }
}

/// @brief Refuses @p value unless it is one of @p values, the set the caller's check tests.
template <typename Values>
void one_of(long long value, const Values& values, const char* subject) {
  for (const auto& allowed : values) {
    if (value == static_cast<long long>(allowed)) return;
  }
  throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                          refusal::one_of_predicate(values));
}

/// @brief Refuses unless @p a + @p b is finite and at most @p bound: "{0} + {1} must be at most X".
inline void sum_at_most(double a, double b, double bound, const char* subject_a,
                        const char* subject_b) {
  const double sum = a + b;
  if (!(std::isfinite(sum) && sum <= bound)) {
    throw SonareParameterRefusal::relation(
        ErrorCode::InvalidParameter, {subject_a, subject_b},
        "{0} + {1} must be at most " + refusal::bound_text(bound));
  }
}

/// @brief Refuses a null @p pointer: "<subject> must not be NULL".
inline void non_null(const void* pointer, const char* subject) {
  if (pointer == nullptr) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                            refusal::kNonNullPredicate);
  }
}

/// @brief Refuses a null @p pointer while @p count is not 0:
///        "<subject> must not be NULL when <count_subject> is not 0".
inline void non_null_when_count(const void* pointer, std::size_t count, const char* subject,
                                const char* count_subject) {
  if (pointer == nullptr && count != 0) {
    throw SonareParameterRefusal::relation(ErrorCode::InvalidParameter, {subject, count_subject},
                                           refusal::kNonNullWhenCountRelation);
  }
}

/// @brief Refuses @p subject with a predicate no domain helper states: "<subject> <predicate>".
[[noreturn]] inline void refuse(const char* subject, const std::string& predicate) {
  throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject, predicate);
}

/// @brief Refuses an empty @p value: "<subject> must not be empty".
inline void non_empty(std::string_view value, const char* subject) {
  if (value.empty()) refuse(subject, refusal::kNonEmptyPredicate);
}

/// @brief Refuses an id nothing in @p owner carries:
///        "<subject> must be the id of <thing> in this <owner>"; @p thing carries its article.
[[noreturn]] inline void refuse_unknown_id(const char* subject, const char* thing,
                                           const char* owner) {
  refuse(subject, refusal::known_id_predicate(thing, owner));
}

/// @brief Refuses a relation between members; @p text writes subject @c i as "{i}".
[[noreturn]] inline void refuse_relation(std::vector<std::string> subjects,
                                         const std::string& text) {
  throw SonareParameterRefusal::relation(ErrorCode::InvalidParameter, std::move(subjects), text);
}

/// @brief Rethrows @p e with each subject placed under the member path @p parent; a subject the
///        caller spelled stays the caller's spelling.
[[noreturn]] inline void rethrow_under_member(const SonareParameterRefusal& e,
                                              const std::string& parent) {
  throw e.respelled(
      e.code(), [&parent](const std::string& subject) { return parent + "." + subject; },
      e.caller_spelled());
}

/// @brief Runs @p call, placing the subjects of a parameter refusal under @p parent.
template <typename Call>
auto under_member(const std::string& parent, Call&& call) -> decltype(call()) {
  try {
    return call();
  } catch (const SonareParameterRefusal& e) {
    rethrow_under_member(e, parent);
  }
}

/// @brief @p number as the float a domain check reads: a finite value past the float range
///        becomes the infinity of its sign, so the check refuses it by its domain.
inline float saturated_float(double number) {
  constexpr double kMax = static_cast<double>(std::numeric_limits<float>::max());
  if (number > kMax) return std::numeric_limits<float>::infinity();
  if (number < -kMax) return -std::numeric_limits<float>::infinity();
  return static_cast<float>(number);
}

/// @brief @p path with each dot-separated segment in camel case: "receiver_spacing_m" ->
///        "receiverSpacingM", "air.temperature_c" -> "air.temperatureC".
inline std::string camel_path(const std::string& path) {
  std::string out;
  bool upper = false;
  for (const char c : path) {
    if (c == '_') {
      upper = !out.empty() && out.back() != '.';
      continue;
    }
    out += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
    upper = false;
  }
  return out;
}

/// @brief A core member path and the request key that spells it where @ref camel_path does not.
using MemberKey = std::pair<const char*, const char*>;

/// @brief @p refusal in a JS request object's spelling: each subject through @p renamed when it
///        lists the subject, else through @ref camel_path. A refusal already in the caller's
///        spelling is returned as it is, so rendering twice changes nothing.
/// @details The one renderer behind both JS translation points (WASM's exception info and the
///          Node addon's error builder) and @ref with_camel_paths.
template <std::size_t N = 0>
SonareParameterRefusal camel_spelled(const SonareParameterRefusal& refusal,
                                     const std::array<MemberKey, N>& renamed = {}) {
  if (refusal.caller_spelled()) return refusal;
  return refusal.respelled(
      refusal.code(),
      [&renamed](const std::string& subject) {
        for (const MemberKey& row : renamed) {
          if (subject == row.first) return std::string(row.second);
        }
        return camel_path(subject);
      },
      true);
}

/// @brief Runs @p call, rethrowing a refusal it raises through @ref camel_spelled with the
///        subjects @p renamed spells; an entry needs it only for those rows.
template <typename Call, std::size_t N>
auto with_camel_paths(Call&& call, const std::array<MemberKey, N>& renamed) -> decltype(call()) {
  try {
    return call();
  } catch (const SonareParameterRefusal& e) {
    throw camel_spelled(e, renamed);
  }
}

}  // namespace sonare::util::check
