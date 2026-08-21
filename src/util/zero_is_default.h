#pragma once

/// @file zero_is_default.h
/// @brief The public "0 selects the library default" sentinel convention.

#include <string>

#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare {

/// @brief A caller-supplied scalar in which the documented sentinel 0 selects
///        the library default.
///
/// Every public surface documents this convention for the optional scalars of
/// the acoustic and metering entry points (see sonare_c_acoustic.h and
/// sonare_c_metering.h), and each entry point used to spell the rule out by
/// hand. Two mistakes recurred across those copies, and holding the caller's
/// value in this type removes both:
///
///  - Testing the sentinel with `> 0` instead of `== 0`, which quietly promotes
///    a negative or NaN request to the library default. The comparison lives
///    here now, so a value that is not the sentinel is either applied or
///    rejected -- never replaced.
///  - Validating the value that survived the substitution rather than the value
///    the caller passed, which checks the default's bounds instead of the
///    request's. The value is private and `checked()` inspects it before
///    substituting anything, so that order is not expressible.
class ZeroIsDefault {
 public:
  explicit constexpr ZeroIsDefault(float caller_value) noexcept : caller_value_(caller_value) {}

  /// @brief The caller's value, or @p library_default for the sentinel.
  ///
  /// For entry points that report an out-of-range value further down (the RIR
  /// synthesizer's diagnostic channel, the room-morph core's own validation)
  /// rather than rejecting it here.
  constexpr float or_default(float library_default) const noexcept {
    return caller_value_ == 0.0f ? library_default : caller_value_;
  }

  /// @brief `or_default()` after checking what the caller passed: the sentinel,
  ///        or a finite value within [@p minimum, @p maximum].
  /// @throws SonareException(InvalidParameter) for anything else, so a value
  ///         outside the accepted range can never resolve to the default.
  float checked(float library_default, float minimum, float maximum, const char* field) const {
    SONARE_CHECK_MSG(
        caller_value_ == 0.0f || numeric::finite_in_closed_range(caller_value_, minimum, maximum),
        ErrorCode::InvalidParameter,
        std::string(field) + " must be 0 (the library default) or a finite value in [" +
            std::to_string(minimum) + ", " + std::to_string(maximum) + "]");
    return or_default(library_default);
  }

 private:
  float caller_value_;
};

}  // namespace sonare
