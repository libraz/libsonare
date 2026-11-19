#pragma once

/// @file parameter_checks.h
/// @brief Per-field domain checks a config validator refuses through.
/// @details Each check refuses with a SonareParameterRefusal naming @p subject — the config
///          member as the validator spells it — in the predicate vocabulary of
///          refusal_predicates.h. A layer that read the member from a caller's key respells the
///          subject (mastering/api/param_map.h, with_caller_keys).

#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "util/exception.h"
#include "util/refusal_predicates.h"

namespace sonare::util::check {

/// @brief Refuses @p value unless it is finite and in [lo, hi].
inline void in_range(double value, double lo, double hi, const char* subject) {
  if (!(std::isfinite(value) && value >= lo && value <= hi)) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                            refusal::range_predicate(lo, hi));
  }
}

/// @brief in_range for bounds computed at run time; the refusal prints them rounded inward.
inline void in_computed_range(double value, double lo, double hi, const char* subject) {
  if (!(std::isfinite(value) && value >= lo && value <= hi)) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                            refusal::computed_range_predicate(lo, hi));
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
        refusal::interval_predicate(lo, lo_included, hi, hi_included));
  }
}

/// @brief Refuses @p value unless it is finite and at least @p bound.
inline void at_least(double value, double bound, const char* subject) {
  if (!(std::isfinite(value) && value >= bound)) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                            refusal::at_least_predicate(bound));
  }
}

/// @brief Refuses @p value unless it is finite and greater than @p bound.
inline void greater_than(double value, double bound, const char* subject) {
  if (!(std::isfinite(value) && value > bound)) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                            refusal::greater_than_predicate(bound));
  }
}

/// @brief Refuses @p value unless it is finite and at most @p bound.
inline void at_most(double value, double bound, const char* subject) {
  if (!(std::isfinite(value) && value <= bound)) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                            refusal::at_most_predicate(bound));
  }
}

/// @brief Refuses @p value unless it is finite and less than @p bound.
inline void less_than(double value, double bound, const char* subject) {
  if (!(std::isfinite(value) && value < bound)) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                            refusal::less_than_predicate(bound));
  }
}

/// @brief Refuses @p value unless it is finite, for a float member with no narrower domain.
inline void finite_float(float value, const char* subject) {
  if (!std::isfinite(value)) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                            refusal::kFloatRangePredicate);
  }
}

/// @brief Refuses @p value unless it is in [lo, hi].
inline void integer_in_range(long long value, long long lo, long long hi, const char* subject) {
  if (value < lo || value > hi) {
    throw SonareParameterRefusal::predicate(
        ErrorCode::InvalidParameter, subject,
        refusal::integer_range_predicate(static_cast<double>(lo), static_cast<double>(hi)));
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

/// @brief Refuses a relation between members; @p text writes subject @c i as "{i}".
[[noreturn]] inline void refuse_relation(std::vector<std::string> subjects,
                                         const std::string& text) {
  throw SonareParameterRefusal::relation(ErrorCode::InvalidParameter, std::move(subjects), text);
}

/// @brief Rethrows @p e with each subject placed under the member path @p parent.
[[noreturn]] inline void rethrow_under_member(const SonareParameterRefusal& e,
                                              const std::string& parent) {
  throw e.respelled(
      e.code(), [&parent](const std::string& subject) { return parent + "." + subject; }, false);
}

}  // namespace sonare::util::check
