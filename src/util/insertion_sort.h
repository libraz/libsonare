#pragma once

/// @file insertion_sort.h
/// @brief Stable, allocation-free small-code sort: insertion runs merged in place.
/// @details Each std::sort instantiation carries an introsort, a heap fallback
///          and sorting networks (several KB of code per element type); this
///          is a few hundred bytes. The sort is stable and allocation-free (safe
///          on audio threads): 32-element insertion runs are merged in place by
///          rotations, O(n log^2 n) for any length. Iterators must be random access.

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <utility>

namespace sonare {

/// @brief Length of the insertion-sorted runs the merge starts from.
constexpr std::ptrdiff_t kInsertionSortLimit = 32;

namespace detail {

template <typename It, typename Less>
void insertion_pass(It first, It last, Less& less) {
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

/// Stable in-place merge of sorted [first, mid) and [mid, last) by rotations (symmerge).
template <typename It, typename Less>
void merge_runs(It first, It mid, It last, Less& less) {
  const std::ptrdiff_t a = 0;
  const std::ptrdiff_t m = mid - first;
  const std::ptrdiff_t b = last - first;
  if (m - a == 0 || b - m == 0) return;
  if (m - a == 1) {
    // Insert the single left element after every right element it does not exceed.
    std::ptrdiff_t lo = m, hi = b;
    while (lo < hi) {
      const std::ptrdiff_t c = lo + (hi - lo) / 2;
      if (less(first[c], first[a])) {
        lo = c + 1;
      } else {
        hi = c;
      }
    }
    std::rotate(first, first + 1, first + lo);
    return;
  }
  if (b - m == 1) {
    // Insert the single right element before the first left element greater than it.
    std::ptrdiff_t lo = a, hi = m;
    while (lo < hi) {
      const std::ptrdiff_t c = lo + (hi - lo) / 2;
      if (!less(first[m], first[c])) {
        lo = c + 1;
      } else {
        hi = c;
      }
    }
    std::rotate(first + lo, mid, mid + 1);
    return;
  }
  const std::ptrdiff_t half = a + (b - a) / 2;
  const std::ptrdiff_t n = half + m;
  std::ptrdiff_t start, r;
  if (m > half) {
    start = n - b;
    r = half;
  } else {
    start = a;
    r = m;
  }
  const std::ptrdiff_t p = n - 1;
  while (start < r) {
    const std::ptrdiff_t c = start + (r - start) / 2;
    if (!less(first[p - c], first[c])) {
      start = c + 1;
    } else {
      r = c;
    }
  }
  const std::ptrdiff_t end = n - start;
  if (start < m && m < end) std::rotate(first + start, mid, first + end);
  if (a < start && start < half) merge_runs(first, first + start, first + half, less);
  if (half < end && end < b) merge_runs(first + half, first + end, last, less);
}

}  // namespace detail

template <typename It, typename Less>
void insertion_sort(It first, It last, Less less) {
  const std::ptrdiff_t n = std::distance(first, last);
  if (n <= kInsertionSortLimit) {
    detail::insertion_pass(first, last, less);
    return;
  }
  for (std::ptrdiff_t start = 0; start < n; start += kInsertionSortLimit) {
    const std::ptrdiff_t stop = start + kInsertionSortLimit < n ? start + kInsertionSortLimit : n;
    detail::insertion_pass(first + start, first + stop, less);
  }
  for (std::ptrdiff_t width = kInsertionSortLimit; width < n; width *= 2) {
    for (std::ptrdiff_t start = 0; start + width < n; start += 2 * width) {
      const std::ptrdiff_t stop = start + 2 * width < n ? start + 2 * width : n;
      detail::merge_runs(first + start, first + start + width, first + stop, less);
    }
  }
}

template <typename It>
void insertion_sort(It first, It last) {
  insertion_sort(first, last, std::less<>());
}

}  // namespace sonare
