#include "automation/automation_engine.h"

#include <algorithm>
#include <memory>

namespace sonare::automation {

void AutomationBoundaryList::clear() noexcept {
  size = 0;
  overflowed = false;
}

bool AutomationBoundaryList::add(double value) noexcept {
  if (size >= ppq.size()) {
    overflowed = true;
    return false;
  }
  ppq[size++] = value;
  return true;
}

void AutomationBoundaryList::sort_unique() noexcept {
  std::sort(ppq.begin(), ppq.begin() + static_cast<std::ptrdiff_t>(size));
  size_t out = 0;
  for (size_t i = 0; i < size; ++i) {
    if (out == 0 || ppq[i] != ppq[out - 1]) {
      ppq[out++] = ppq[i];
    }
  }
  size = out;
}

void AutomationEngine::prepare(double sample_rate, const transport::TempoMap* tempo_map) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  tempo_map_ = tempo_map;
}

void AutomationEngine::set_lanes(std::vector<AutomationLane> lanes) {
  std::sort(lanes.begin(), lanes.end(), [](const AutomationLane& a, const AutomationLane& b) {
    return a.target_param_id() < b.target_param_id();
  });
  lane_count_.store(lanes.size(), std::memory_order_relaxed);
  lanes_.publish(std::make_shared<const std::vector<AutomationLane>>(std::move(lanes)));
}

bool AutomationEngine::bind_target(uint32_t param_id, rt::ProcessorBase* processor) noexcept {
  if (param_id == 0 || processor == nullptr) return false;  // 0 is reserved as invalid/none.
  const size_t bound = bound_count_.load(std::memory_order_relaxed);
  // Reuse an existing slot for the same param_id, or the first free slot within
  // the bound range, so a rebind after a clear lands deterministically and does
  // not grow bound_count_ unnecessarily.
  for (size_t i = 0; i < bound; ++i) {
    if (targets_[i].param_id == param_id) {
      targets_[i].processor = processor;
      return true;
    }
  }
  for (size_t i = 0; i < bound; ++i) {
    if (targets_[i].processor == nullptr) {
      targets_[i].param_id = param_id;
      targets_[i].processor = processor;
      return true;
    }
  }
  if (bound >= targets_.size()) {
    bind_target_overflow_count_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  // Append a new slot: fully populate it, then publish the grown count with
  // release so the audio thread (acquire load in target_for) only observes it
  // once the slot is complete.
  targets_[bound].param_id = param_id;
  targets_[bound].processor = processor;
  bound_count_.store(bound + 1, std::memory_order_release);
  return true;
}

void AutomationEngine::clear_targets() noexcept {
  // Clear slot contents but keep bound_count_ so target_for() keeps scanning
  // the full range and skips the cleared (null) slots instead of stopping at
  // the first one; freshly bound targets reuse the cleared slots.
  const size_t bound = bound_count_.load(std::memory_order_relaxed);
  for (size_t i = 0; i < bound; ++i) {
    targets_[i] = {};
  }
}

void AutomationEngine::apply(const transport::TransportState& state, int sub_block_offset,
                             int sub_block_len) noexcept {
  if (sub_block_len <= 0 || !tempo_map_ || !(sample_rate_ > 0.0)) return;

  // Map the sub-block's timeline sample to a PPQ position. The tempo map owns
  // the sample<->PPQ relationship, but its result is only meaningful once a
  // valid sample rate has been prepared, so sample_rate_ gates the conversion.
  const int64_t timeline_sample = state.sample_position + sub_block_offset;
  const double ppq = tempo_map_->sample_to_ppq(timeline_sample);
  // Lanes are acquired exactly once per block via acquire_lanes(); they are
  // never swapped mid-block, so apply() reads the already-current set without
  // re-acquiring (which would otherwise allow a mid-block swap between
  // sub-blocks and violate the single-acquisition contract).
  const std::vector<AutomationLane>* lanes = lanes_.current();
  if (!lanes) {
    if (lane_count_.load(std::memory_order_relaxed) > 0) {
      stale_lane_apply_count_.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }
  for (const AutomationLane& lane : *lanes) {
    rt::ProcessorBase* processor = target_for(lane.target_param_id());
    if (!processor) {
      unknown_target_count_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (!processor->parameter_is_realtime_safe(lane.target_param_id())) {
      non_realtime_safe_rejection_count_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    processor->set_parameter(lane.target_param_id(), lane.value_at(ppq));
  }
}

bool AutomationEngine::set_parameter(uint32_t param_id, float value) noexcept {
  rt::ProcessorBase* processor = target_for(param_id);
  if (!processor) {
    unknown_target_count_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (!processor->parameter_is_realtime_safe(param_id)) {
    non_realtime_safe_rejection_count_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  processor->set_parameter(param_id, value);
  return true;
}

void AutomationEngine::collect_boundaries(double block_start_ppq, double block_end_ppq,
                                          AutomationBoundaryList* out) noexcept {
  if (!out) return;
  out->clear();
  const double lo = std::min(block_start_ppq, block_end_ppq);
  const double hi = std::max(block_start_ppq, block_end_ppq);
  const std::vector<AutomationLane>* lanes = lanes_.current();
  if (!lanes) {
    if (lane_count_.load(std::memory_order_relaxed) > 0) {
      stale_lane_apply_count_.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }
  for (const AutomationLane& lane : *lanes) {
    double next = lane.next_breakpoint_after(lo);
    while (next <= hi) {
      if (!out->add(next)) break;
      next = lane.next_breakpoint_after(next);
    }
  }
  out->sort_unique();
}

size_t AutomationEngine::lane_count() const noexcept {
  return lane_count_.load(std::memory_order_relaxed);
}

rt::ProcessorBase* AutomationEngine::target_for(uint32_t param_id) const noexcept {
  if (param_id == 0) return nullptr;  // 0 is reserved as the invalid/none id.
  // Scan the full bound range and SKIP null/cleared slots instead of stopping
  // at the first one. Stopping early silently dropped any target bound after a
  // cleared earlier slot (e.g. after a clear+rebind). bound_count_ is read with
  // acquire to pair with bind_target's release publish.
  const size_t bound = bound_count_.load(std::memory_order_acquire);
  for (size_t i = 0; i < bound; ++i) {
    const Target& target = targets_[i];
    if (target.processor != nullptr && target.param_id == param_id) {
      return target.processor;
    }
  }
  return nullptr;
}

}  // namespace sonare::automation
