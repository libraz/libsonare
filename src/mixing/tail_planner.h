#pragma once

/// @file tail_planner.h
/// @brief Longest audible processor tail over a mixer's routing DAG.
///
/// Callers describe the audible paths as they stand at query time; each path
/// carries the tail its source adds on the tap it leaves from (the whole chain
/// for a main or post-fader output, the pre-fader prefix for a pre-fader send,
/// the source's output for a monitored sidechain key). Serial stages add,
/// merging paths take the longest, and INT_MAX stays unbounded. Nothing is
/// cached, so automation that changes a processor's tail is seen by the next
/// query.

#include <cstddef>
#include <vector>

#include "mixing/tail_utils.h"

namespace sonare::mixing {

class MixerTailPlanner {
 public:
  explicit MixerTailPlanner(size_t node_count) : arriving_(node_count, 0) {}

  /// Reserves room for @p count paths. Control thread; may allocate.
  void reserve(size_t count) { paths_.reserve(count); }

  /// An audible path from @p source to @p dest, adding @p tap_tail.
  void add_path(size_t source, size_t dest, int tap_tail) {
    paths_.push_back({source, dest, tap_tail});
  }

  /// A path entering @p dest from outside the planned nodes, carrying @p tail.
  void add_entry(size_t dest, int tail) noexcept {
    arriving_[dest] = combine_tail_samples(arriving_[dest], tail, TailTopology::kParallel);
  }

  /// Longest tail arriving at @p node's input. Paths must form a DAG; relaxing
  /// once per node settles any DAG regardless of the order paths were added in.
  int arriving(size_t node) {
    for (size_t pass = 0; pass < arriving_.size(); ++pass) {
      bool changed = false;
      for (const Path& path : paths_) {
        const int through =
            combine_tail_samples(arriving_[path.source], path.tap_tail, TailTopology::kSerial);
        if (through > arriving_[path.dest]) {
          arriving_[path.dest] = through;
          changed = true;
        }
      }
      if (!changed) break;
    }
    return arriving_[node];
  }

  /// Tail leaving @p node through a tap that adds @p tap_tail.
  int leaving(size_t node, int tap_tail) {
    return combine_tail_samples(arriving(node), tap_tail, TailTopology::kSerial);
  }

 private:
  struct Path {
    size_t source;
    size_t dest;
    int tap_tail;
  };
  std::vector<int> arriving_;
  std::vector<Path> paths_;
};

}  // namespace sonare::mixing
