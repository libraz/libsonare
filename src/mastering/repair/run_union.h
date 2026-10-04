#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace sonare::mastering::repair {

/// Merges two ascending, non-overlapping run lists (any type with `start`/`end`)
/// into one ascending list, unioning runs that overlap. Runs that merely touch
/// stay separate. Equal starts resolve to the same result in either order.
template <typename Run>
std::vector<Run> union_sorted_runs(const std::vector<Run>& a, const std::vector<Run>& b) {
  std::vector<Run> result;
  result.reserve(a.size() + b.size());
  size_t i = 0;
  size_t j = 0;
  while (i < a.size() || j < b.size()) {
    const bool take_a = j == b.size() || (i < a.size() && !(b[j].start < a[i].start));
    const Run& run = take_a ? a[i++] : b[j++];
    if (!result.empty() && run.start < result.back().end) {
      result.back().end = std::max(result.back().end, run.end);
      continue;
    }
    result.push_back(run);
  }
  return result;
}

}  // namespace sonare::mastering::repair
