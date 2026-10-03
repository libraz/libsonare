#pragma once

/// @file insertion_sort.h
/// @brief Stable in-place insertion sort for short or already-ordered ranges.
/// @details Each std::sort instantiation carries an introsort, a heap fallback
///          and sorting networks (several KB of code per element type); this
///          is a few hundred bytes. Use it only where the range is short or
///          arrives nearly sorted, and where equivalent elements are
///          indistinguishable, so the result is the one std::sort produces.

#include <functional>
#include <iterator>
#include <utility>

namespace sonare {

template <typename It, typename Less>
void insertion_sort(It first, It last, Less less) {
  if (first == last) return;
  for (It i = std::next(first); i != last; ++i) {
    auto value = std::move(*i);
    It j = i;
    for (It prev = std::prev(j); less(value, *prev); --prev) {
      *j = std::move(*prev);
      j = prev;
      if (prev == first) break;
    }
    *j = std::move(value);
  }
}

template <typename It>
void insertion_sort(It first, It last) {
  insertion_sort(first, last, std::less<>());
}

}  // namespace sonare
