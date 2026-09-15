#pragma once

/// @file non_finite_state.h
/// @brief The rule a recursive state cell follows when a non-finite value reaches it.
///
/// A cell a processor carries from one block to the next — a filter's history
/// taps, an envelope follower, a smoothed gain — is recursive: the next value is
/// computed from the current one. One non-finite input sample therefore does not
/// degrade a block, it ends the handle. Every later block of an otherwise clean
/// stream comes out non-finite, and only an explicit reset restores the
/// processor, which nothing tells the caller to do.
///
/// A cell reinitialised by a sentinel comparison hides the same defect rather
/// than escaping it: a non-finite value answers false to every comparison, so the
/// reinitialisation branch never fires again. A downstream clamp can hide it
/// twice over, turning the non-finite value back into a plausible finite one that
/// is simply wrong forever.
///
/// The owner applies the rule, once per block over its own cells. A per-sample
/// primitive cannot: a check in its inner loop is the O(samples) scan a realtime
/// contract exists to avoid, and inspecting the cells costs O(cells) instead.

#include <algorithm>
#include <cmath>

namespace sonare {

/// @brief Returns @p cell to @p post_reset_value when a non-finite value has
///        reached it.
/// @return true when @p cell was discarded.
template <typename T>
bool discard_if_non_finite(T& cell, T post_reset_value) noexcept {
  if (std::isfinite(cell)) {
    return false;
  }
  cell = post_reset_value;
  return true;
}

/// @brief The same rule over cells that are only meaningful together, such as a
///        biquad section's two history taps or a detector's filter and envelope.
/// @details A section holding one usable tap and one non-finite tap is not
/// half-usable, so any non-finite cell returns the whole group to zero.
/// @return true when the group was discarded.
template <typename... Cells>
bool discard_group_if_non_finite(Cells&... cells) noexcept {
  if ((... && std::isfinite(cells))) {
    return false;
  }
  ((cells = 0), ...);
  return true;
}

/// @brief The same rule over a run of cells a filter holds as one history, such
///        as a noise shaper's error taps.
/// @details Identical in meaning to @ref discard_group_if_non_finite; the run
/// form exists because a history long enough to live in an array cannot be named
/// cell by cell without the length being written down twice.
/// @return true when the run was discarded.
template <typename Iterator, typename T>
bool discard_run_if_non_finite(Iterator first, Iterator last, T post_reset_value) noexcept {
  if (std::all_of(first, last, [](T cell) { return std::isfinite(cell); })) {
    return false;
  }
  std::fill(first, last, post_reset_value);
  return true;
}

}  // namespace sonare
