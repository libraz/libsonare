#pragma once

/// @file parameter_checks.h
/// @brief Per-field domain checks a config validator refuses through.
/// @details Each check refuses with a SonareParameterRefusal naming @p subject — the config
///          member as the validator spells it — in the predicate vocabulary of
///          refusal_predicates.h. A layer that read the member from a caller's key respells the
///          subject (mastering/api/param_map.h, with_caller_keys).

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "util/exception.h"
#include "util/numeric_validation.h"
#include "util/refusal_predicates.h"

namespace sonare::util::check {

/// @brief Whether a numeric domain holds integers or real numbers.
enum class NumberKind { Integer, Real };

/// @brief A numeric domain: the one statement of what a member accepts, read by the core check
///        that refuses it and by a binding's reader before it narrows a caller's number, so a
///        fraction, a NaN or a value no target type holds is refused in the domain's own text.
/// @details An absent bound leaves that side open. An integer domain is closed at both ends or
///          open above; a real domain may be open or half-open at either end. @ref sentinel is the
///          value that stands for "not given" in the C ABI (a default the entry resolves), stated
///          so a binding can map absence to it and refuse it when typed; the checks ignore it.
struct NumericDomain {
  NumberKind kind = NumberKind::Real;
  std::optional<double> lo;
  std::optional<double> hi;
  bool lo_included = true;
  bool hi_included = true;
  std::optional<double> sentinel;
};

/// @brief @p domain with @p value as its "not given" sentinel.
constexpr NumericDomain with_sentinel(const NumericDomain& domain, double value) {
  return {domain.kind, domain.lo, domain.hi, domain.lo_included, domain.hi_included, value};
}

/// @brief What a caller who gives the member may pass: @p domain without its sentinel.
/// @details A sentinel at an included end is taken off that end (the next integer, or the end
///          excluded for a real domain); one outside the domain leaves it unchanged. A sentinel
///          strictly inside the domain cannot be told apart from a given value, so it fails to
///          compile where the domain is a constant.
constexpr NumericDomain given_domain(const NumericDomain& domain) {
  if (!domain.sentinel) return domain;
  const double s = *domain.sentinel;
  const bool integer = domain.kind == NumberKind::Integer;
  const bool at_lo = domain.lo && domain.lo_included && s == *domain.lo;
  const bool at_hi = domain.hi && domain.hi_included && s == *domain.hi;
  const bool below = domain.lo && (s < *domain.lo || (s == *domain.lo && !domain.lo_included));
  const bool above = domain.hi && (s > *domain.hi || (s == *domain.hi && !domain.hi_included));
  if (!at_lo && !at_hi && !below && !above) {
    throw std::logic_error("a domain's sentinel lies strictly inside it");
  }
  return {domain.kind,
          at_lo && integer ? std::optional<double>(*domain.lo + 1.0) : domain.lo,
          at_hi && integer ? std::optional<double>(*domain.hi - 1.0) : domain.hi,
          at_lo && !integer ? false : domain.lo_included,
          at_hi && !integer ? false : domain.hi_included,
          domain.sentinel};
}

/// @brief The integers in [lo, hi].
constexpr NumericDomain integer_domain(long long lo, long long hi) {
  return {NumberKind::Integer, static_cast<double>(lo), static_cast<double>(hi), true, true,
          std::nullopt};
}

/// @brief The integers of at least @p lo, for a 64-bit position or count.
constexpr NumericDomain integer_domain_at_least(long long lo) {
  return {NumberKind::Integer, static_cast<double>(lo), std::nullopt, true, true, std::nullopt};
}

/// @brief The integers of at least @p lo that @p T holds: closed at T's maximum when T is no
///        wider than int, open above otherwise.
template <typename T>
constexpr NumericDomain integer_domain_of(long long lo) {
  static_assert(std::is_integral_v<T>, "integer_domain_of takes an integer type");
  if constexpr (sizeof(T) <= sizeof(int)) {
    return integer_domain(lo, static_cast<long long>(std::numeric_limits<T>::max()));
  } else {
    return integer_domain_at_least(lo);
  }
}

/// @brief The finite reals between @p lo and @p hi, each end included as given.
constexpr NumericDomain real_domain(double lo, bool lo_included, double hi, bool hi_included) {
  return {NumberKind::Real, lo, hi, lo_included, hi_included, std::nullopt};
}

/// @brief The finite reals in [lo, hi].
constexpr NumericDomain real_domain(double lo, double hi) {
  return real_domain(lo, true, hi, true);
}

/// @brief The finite reals of at least @p bound, or greater than it when @p included is false.
constexpr NumericDomain real_domain_above(double bound, bool included) {
  return {NumberKind::Real, bound, std::nullopt, included, true, std::nullopt};
}

/// @brief The finite reals of at most @p bound, or less than it when @p included is false.
constexpr NumericDomain real_domain_below(double bound, bool included) {
  return {NumberKind::Real, std::nullopt, bound, true, included, std::nullopt};
}

/// @brief Every value a 32-bit float holds, for a float member with no narrower domain.
constexpr NumericDomain float_domain() {
  return real_domain(-static_cast<double>(std::numeric_limits<float>::max()),
                     static_cast<double>(std::numeric_limits<float>::max()));
}

/// @brief The predicate text that states @p domain.
inline std::string domain_predicate(const NumericDomain& domain) {
  if (domain.kind == NumberKind::Integer) {
    if (domain.hi) return refusal::integer_range_predicate(*domain.lo, *domain.hi);
    return refusal::integer_at_least_predicate(static_cast<long long>(*domain.lo));
  }
  if (domain.lo && domain.hi) {
    return refusal::interval_predicate(*domain.lo, domain.lo_included, *domain.hi,
                                       domain.hi_included);
  }
  if (domain.lo) {
    return domain.lo_included ? refusal::at_least_predicate(*domain.lo)
                              : refusal::greater_than_predicate(*domain.lo);
  }
  if (domain.hi) {
    return domain.hi_included ? refusal::at_most_predicate(*domain.hi)
                              : refusal::less_than_predicate(*domain.hi);
  }
  return refusal::kFiniteDoublePredicate;
}

/// @brief Whether @p value lies in @p domain: finite, whole for an integer domain, inside both
///        ends.
inline bool in_domain(double value, const NumericDomain& domain) {
  if (!std::isfinite(value)) return false;
  if (domain.kind == NumberKind::Integer && std::trunc(value) != value) return false;
  if (domain.lo && !(domain.lo_included ? value >= *domain.lo : value > *domain.lo)) return false;
  if (domain.hi && !(domain.hi_included ? value <= *domain.hi : value < *domain.hi)) return false;
  return true;
}

/// @brief Refuses @p subject as outside @p domain, stating the domain and carrying it as data.
[[noreturn]] inline void refuse_outside(const NumericDomain& domain, const char* subject) {
  throw SonareParameterRefusal::predicate(
      ErrorCode::InvalidParameter, subject, domain_predicate(domain),
      RefusalDomain{domain.lo, domain.hi, domain.lo_included, domain.hi_included});
}

/// @brief Refuses @p value unless it lies in @p domain.
inline void within(double value, const NumericDomain& domain, const char* subject) {
  if (!in_domain(value, domain)) refuse_outside(domain, subject);
}

/// @brief Refuses @p value unless it is finite and in [lo, hi].
inline void in_range(double value, double lo, double hi, const char* subject) {
  within(value, real_domain(lo, hi), subject);
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
  within(value, real_domain(lo, lo_included, hi, hi_included), subject);
}

/// @brief Refuses @p value unless it is finite and at least @p bound.
inline void at_least(double value, double bound, const char* subject) {
  within(value, real_domain_above(bound, true), subject);
}

/// @brief Refuses @p value unless it is finite and greater than @p bound.
inline void greater_than(double value, double bound, const char* subject) {
  within(value, real_domain_above(bound, false), subject);
}

/// @brief Refuses @p value unless it is finite and at most @p bound.
inline void at_most(double value, double bound, const char* subject) {
  within(value, real_domain_below(bound, true), subject);
}

/// @brief Refuses @p value unless it is finite and less than @p bound.
inline void less_than(double value, double bound, const char* subject) {
  within(value, real_domain_below(bound, false), subject);
}

/// @brief Refuses deriving a gain from @p level_db unless it is finite: -inf (no measurable level)
///        and NaN (not measured) both leave nothing to scale from.
/// @param predicate @ref refusal::kNoMeasurableLoudnessPredicate or
///        @ref refusal::kNoMeasurableLevelPredicate.
inline void measurable_level(float level_db, const std::string& subject, const char* predicate) {
  if (!std::isfinite(level_db)) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject, predicate);
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

/// @brief Refuses @p value unless it is in [lo, hi]. Compared as an integer, so a 64-bit value
///        past 2^53 is judged exactly.
inline void integer_in_range(long long value, long long lo, long long hi, const char* subject) {
  if (value < lo || value > hi) refuse_outside(integer_domain(lo, hi), subject);
}

/// @brief Refuses @p value unless it is at least @p bound. A type no wider than int states its
///        whole domain ("must be an integer in [N, 2147483647]"), the form a binding's integer
///        reader prints; a 64-bit count states its floor ("must be an integer of at least N").
template <typename T>
inline void integer_at_least(T value, long long bound, const char* subject) {
  static_assert(std::is_integral_v<T>, "integer_at_least takes an integer");
  if (static_cast<long long>(value) < bound) refuse_outside(integer_domain_of<T>(bound), subject);
}

/// @brief Refuses an integer @p value outside an integer @p domain, compared as integers so a
///        64-bit value past 2^53 is judged exactly and an unsigned one past LLONG_MAX is refused.
template <typename T>
inline void integer_within(T value, const NumericDomain& domain, const char* subject) {
  static_assert(std::is_integral_v<T>, "integer_within takes an integer");
  if constexpr (std::is_unsigned_v<T>) {
    if (static_cast<unsigned long long>(value) >
        static_cast<unsigned long long>(std::numeric_limits<long long>::max())) {
      refuse_outside(domain, subject);
    }
  }
  const auto v = static_cast<long long>(value);
  if ((domain.lo && v < static_cast<long long>(*domain.lo)) ||
      (domain.hi && v > static_cast<long long>(*domain.hi))) {
    refuse_outside(domain, subject);
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

/// @brief The element count @p lhs * @p rhs, computed in 64 bits and refused naming @p subject —
///        the argument the caller scales the product by — when it passes @p limit:
///        "<subject> must keep <what> within N elements". @p Size is the target's size type.
template <typename Size = std::size_t>
Size element_count(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t limit, const char* subject,
                   const char* what) {
  const std::uint64_t bound =
      std::min<std::uint64_t>(limit, std::numeric_limits<Size>::max());  // the limit that binds
  Size count = 0;
  if (!numeric::checked_size_product(lhs, rhs, bound, &count)) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                            refusal::element_limit_predicate(what, bound));
  }
  return count;
}

/// @brief power_of_two_in_range over a closed integer @p domain, so the range is a named constant.
inline void power_of_two_within(long long value, const NumericDomain& domain, const char* subject) {
  power_of_two_in_range(value, static_cast<long long>(*domain.lo),
                        static_cast<long long>(*domain.hi), subject);
}

/// @brief even_integer_in_range over a closed integer @p domain.
inline void even_within(long long value, const NumericDomain& domain, const char* subject) {
  even_integer_in_range(value, static_cast<long long>(*domain.lo),
                        static_cast<long long>(*domain.hi), subject);
}

/// @brief odd_integer_in_range over a closed integer @p domain.
inline void odd_within(long long value, const NumericDomain& domain, const char* subject) {
  odd_integer_in_range(value, static_cast<long long>(*domain.lo),
                       static_cast<long long>(*domain.hi), subject);
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
