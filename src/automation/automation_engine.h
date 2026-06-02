#pragma once

/// @file automation_engine.h
/// @brief RT-safe application of prepared PPQ automation lanes.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "automation/automation_lane.h"
#include "rt/processor_base.h"
#include "rt/rt_publisher.h"
#include "transport/tempo_map.h"
#include "transport/transport_state.h"

namespace sonare::automation {

struct AutomationBoundaryList {
  static constexpr size_t kCapacity = 64;
  std::array<double, kCapacity> ppq{};
  size_t size = 0;
  bool overflowed = false;

  void clear() noexcept;
  bool add(double value) noexcept;
  void sort_unique() noexcept;
};

class AutomationEngine {
 public:
  void prepare(double sample_rate, const transport::TempoMap* tempo_map);
  void set_lanes(std::vector<AutomationLane> lanes);
  bool bind_target(uint32_t param_id, rt::ProcessorBase* processor) noexcept;
  void clear_targets() noexcept;

  // Unbind the slot currently holding @p param_id, leaving bound_count_
  // untouched so the slot becomes an interior gap that target_for() must skip
  // over (not stop at). Returns true if a matching bound slot was found. Useful
  // for selectively removing a single automation target without disturbing the
  // others' slot assignments.
  bool unbind_target(uint32_t param_id) noexcept {
    const size_t bound = bound_count_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < bound; ++i) {
      if (targets_[i].processor.load(std::memory_order_relaxed) != nullptr &&
          targets_[i].param_id.load(std::memory_order_relaxed) == param_id) {
        // Publish the cleared slot atomically. The audio thread's target_for()
        // reads processor first; storing nullptr there with release ensures it
        // never observes a stale processor pointer paired with a torn param_id
        // (which would produce a garbage vtable call). param_id is cleared after
        // so a concurrent reader that already loaded the (now-null) processor
        // simply skips the slot.
        targets_[i].processor.store(nullptr, std::memory_order_release);
        targets_[i].param_id.store(0, std::memory_order_relaxed);
        return true;
      }
    }
    return false;
  }

  /// Adopt the latest published lane set on the audio thread. Call once at
  /// block start before apply / collect_boundaries. RT-safe, no alloc.
  void acquire_lanes() noexcept { lanes_.acquire(); }

  void apply(const transport::TransportState& state, int sub_block_offset,
             int sub_block_len) noexcept;
  /// Immediately set a bound parameter to @p value, mirroring apply()'s
  /// per-lane resolution and RT-safety gating. Returns true on success;
  /// failures bump the same internal counters apply() uses.
  bool set_parameter(uint32_t param_id, float value) noexcept;
  void collect_boundaries(double block_start_ppq, double block_end_ppq,
                          AutomationBoundaryList* out) noexcept;

  size_t lane_count() const noexcept;
  uint32_t unknown_target_count() const noexcept {
    return unknown_target_count_.load(std::memory_order_relaxed);
  }
  uint32_t non_realtime_safe_rejection_count() const noexcept {
    return non_realtime_safe_rejection_count_.load(std::memory_order_relaxed);
  }
  uint32_t bind_target_overflow_count() const noexcept {
    return bind_target_overflow_count_.load(std::memory_order_relaxed);
  }
  uint32_t stale_lane_apply_count() const noexcept {
    return stale_lane_apply_count_.load(std::memory_order_relaxed);
  }
  // Number of blocks whose boundary collection overflowed AutomationBoundaryList
  // (kCapacity exceeded), so some breakpoint sub-block splits were dropped.
  uint32_t boundary_overflow_count() const noexcept {
    return boundary_overflow_count_.load(std::memory_order_relaxed);
  }

 private:
  // param_id 0 is reserved as the invalid/none sentinel: an unbound slot keeps
  // the default (param_id 0, processor null) and must never match a real lane.
  // Both fields are atomic because the control thread (bind/unbind/clear) and
  // the audio thread (target_for) touch them concurrently; a non-atomic
  // pointer write would tear and yield a garbage vtable call on the reader.
  struct Target {
    std::atomic<uint32_t> param_id{0};
    std::atomic<rt::ProcessorBase*> processor{nullptr};
  };

  rt::ProcessorBase* target_for(uint32_t param_id) const noexcept;

  double sample_rate_ = 48000.0;
  const transport::TempoMap* tempo_map_ = nullptr;
  mutable rt::RtPublisher<std::vector<AutomationLane>> lanes_;
  std::atomic<size_t> lane_count_{0};
  std::array<Target, 128> targets_{};
  // Number of slots in targets_ that have ever been bound (highest occupied
  // index + 1). target_for() scans [0, bound_count_) and *skips* null/cleared
  // slots rather than terminating at the first one, so a target bound to a slot
  // after an earlier cleared slot is still resolved. Published with release on
  // bind/clear, read with acquire on the audio thread.
  std::atomic<size_t> bound_count_{0};
  std::atomic<uint32_t> unknown_target_count_{0};
  std::atomic<uint32_t> non_realtime_safe_rejection_count_{0};
  std::atomic<uint32_t> bind_target_overflow_count_{0};
  std::atomic<uint32_t> stale_lane_apply_count_{0};
  std::atomic<uint32_t> boundary_overflow_count_{0};
};

}  // namespace sonare::automation
