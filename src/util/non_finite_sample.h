#pragma once

/// @file non_finite_sample.h
/// @brief The rule a sample follows when it cannot be represented where it is going.
///
/// A non-finite sample has no single right replacement, because the right answer
/// is a property of the DESTINATION rather than of the sample. Four of the
/// destinations below cannot hold one; the fifth can, and propagating there is
/// what makes the degradation visible. What is shared is the classification and
/// the report, not a value.
///
/// The replacement is silence for every class that substitutes: it is the only
/// value no stage can be said to have computed from its input. Full scale, a
/// ceiling and unity gain are each a value some stage produces on its own.
///
/// A clamp is not a member of this rule. A limiter, a hard clipper and a
/// saturator map an infinity onto their own bound because that is the limit of
/// the transfer function they implement, not because a value was replaced.
///
/// Sits beside non_finite_state.h, which discards a recursive cell a non-finite
/// has ALREADY reached, once per block. This one decides what a sample may
/// enter, at the boundary it crosses.
///
/// The report is the return value and it is `[[nodiscard]]`: a `(void)` at a
/// call site marks a substitution nothing downstream can see.

#include <cmath>
#include <cstddef>

namespace sonare {

/// @brief What a sample is about to enter, which is what decides its fate.
enum class SampleDestination {
  /// A cell the owner carries from one block to the next. One non-finite value
  /// does not degrade a block, it ends the handle (see non_finite_state.h).
  kRecursiveState,
  /// A buffer the stage writes in place and can never reach again -- a file, a
  /// device, a host's callback buffer, a chain's scratch. Something
  /// representable must be written because the sample is out of reach once the
  /// call returns. Whether it later leaves the library is the caller's and
  /// cannot be read at the site, so it does not decide the class: one process()
  /// is reached from both a host callback and a chain.
  kIrreversibleOutput,
  /// A comparison-based algorithm whose precondition a non-finite violates.
  /// `std::lower_bound` and `std::nth_element` over a NaN are undefined
  /// behaviour, which is not the same failure as a wrong answer.
  kOrderedContainer,
  /// A descriptor with a declared closed range. A later clamp cannot restore it,
  /// because every comparison against a NaN is false.
  kBoundedResult,
  /// A pure function's result. The caller is the downstream and a visible
  /// non-finite is itself the report, so nothing is substituted.
  kCallerReturn,
};

/// @brief Whether @p destination replaces a non-finite sample or propagates it.
constexpr bool substitutes_non_finite(SampleDestination destination) noexcept {
  return destination != SampleDestination::kCallerReturn;
}

/// @brief Applies @p destination's rule to @p sample, in place.
/// @param destination What @p sample is about to enter.
/// @param sample Replaced with silence when @p destination substitutes, left
///        exactly as it was when it is finite or when @p destination propagates.
/// @return true when @p sample was non-finite. The caller owns the report: a site
///         holding an rt::OverflowCounter adds it there.
template <typename T>
[[nodiscard]] bool resolve_non_finite(SampleDestination destination, T& sample) noexcept {
  if (std::isfinite(sample)) {
    return false;
  }
  if (substitutes_non_finite(destination)) {
    sample = T{0};
  }
  return true;
}

/// @brief The same rule over a contiguous run of @p n samples.
/// @param first Start of the run; dereferenced without a null check when @p n > 0.
/// @param n Number of samples, not bytes.
/// @return how many of them were non-finite.
template <typename T>
[[nodiscard]] std::size_t resolve_non_finite_run(SampleDestination destination, T* first,
                                                 std::size_t n) noexcept {
  std::size_t count = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (resolve_non_finite(destination, first[i])) ++count;
  }
  return count;
}

}  // namespace sonare
