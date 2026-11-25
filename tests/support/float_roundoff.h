#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <iterator>
#include <limits>
#include <type_traits>

namespace sonare::test {

/// Allow four machine epsilons at the larger magnitude of the two results.
template <typename T>
auto roundoff_margin(T a, T b) {
  using Magnitude = decltype(std::abs(a));
  static_assert(std::is_floating_point_v<Magnitude>);
  const auto scale = std::max(std::abs(a), std::abs(b));
  return 4 * std::numeric_limits<Magnitude>::epsilon() * scale;
}

template <typename T>
bool within_roundoff(T a, T b) {
  if (a == b) return true;
  const auto difference = std::abs(a - b);
  return std::isfinite(difference) && difference <= roundoff_margin(a, b);
}

template <typename Iterator, typename ReferenceIterator>
bool ranges_within_roundoff(Iterator first, Iterator last, ReferenceIterator reference) {
  return std::equal(first, last, reference, [](const auto& actual, const auto& expected) {
    return within_roundoff(actual, expected);
  });
}

template <typename Range, typename Reference>
bool ranges_within_roundoff(const Range& actual, const Reference& expected) {
  return std::equal(std::begin(actual), std::end(actual), std::begin(expected), std::end(expected),
                    [](const auto& a, const auto& b) { return within_roundoff(a, b); });
}

}  // namespace sonare::test
