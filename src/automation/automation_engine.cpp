#include "automation/automation_engine.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "util/constants.h"
#include "util/insertion_sort.h"

namespace sonare::automation {

namespace {

void normalize_lanes(std::vector<AutomationLane>& lanes) {
  // One lane owns a target parameter. Keeping the first caller-supplied lane
  // gives a deterministic result and prevents the audio thread from applying
  // multiple conflicting values with accidental last-wins behavior.
  insertion_sort(lanes.begin(), lanes.end(), [](const AutomationLane& a, const AutomationLane& b) {
    return a.target_param_id() < b.target_param_id();
  });
  lanes.erase(std::unique(lanes.begin(), lanes.end(),
                          [](const AutomationLane& a, const AutomationLane& b) {
                            return a.target_param_id() == b.target_param_id();
                          }),
              lanes.end());
}

}  // namespace

void AutomationBoundaryList::clear() noexcept {
  size = 0;
  overflowed = false;
}

bool AutomationBoundaryList::add(double value) noexcept {
  // Deduplicate before the capacity check; this remains allocation-free.
  for (size_t i = 0; i < size; ++i) {
    if (ppq[i] == value) {
      return true;
    }
  }
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
  sample_rate_ = sample_rate > 0.0 ? sample_rate : constants::kDefaultDawSampleRate;
  tempo_map_ = tempo_map;
}

void AutomationEngine::set_parameter_metadata(std::vector<ParameterInfo> parameters) {
  for (ParameterInfo& info : parameters) {
    info.name = "";
    info.unit = "";
  }
  insertion_sort(parameters.begin(), parameters.end(), parameter_info_id_before);
  parameter_metadata_.publish(
      std::make_shared<const std::vector<ParameterInfo>>(std::move(parameters)));
}

void AutomationEngine::set_lanes(std::vector<AutomationLane> lanes) {
  normalize_lanes(lanes);
  lane_count_.store(lanes.size(), std::memory_order_relaxed);
  lanes_.publish(std::make_shared<const std::vector<AutomationLane>>(std::move(lanes)));
}

bool AutomationEngine::bind_target(uint32_t param_id, rt::ProcessorBase* processor) {
  if (param_id == 0 || processor == nullptr) return false;  // 0 is reserved as invalid/none.
  TargetTable& table = control_targets_;
  // Reuse an existing slot for the same param_id, or the first cleared slot
  // within the bound range, so a rebind after a clear lands deterministically.
  TargetSlot* slot = nullptr;
  for (size_t i = 0; i < table.bound && slot == nullptr; ++i) {
    if (table.slots[i].param_id == param_id) slot = &table.slots[i];
  }
  for (size_t i = 0; i < table.bound && slot == nullptr; ++i) {
    if (table.slots[i].processor == nullptr) slot = &table.slots[i];
  }
  if (slot == nullptr) {
    if (table.bound >= table.slots.size()) {
      bind_target_overflow_count_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    slot = &table.slots[table.bound++];
  }
  slot->param_id = param_id;
  slot->processor = processor;
  publish_targets();
  return true;
}

bool AutomationEngine::unbind_target(uint32_t param_id) {
  for (size_t i = 0; i < control_targets_.bound; ++i) {
    TargetSlot& slot = control_targets_.slots[i];
    if (slot.processor != nullptr && slot.param_id == param_id) {
      slot = TargetSlot{};
      publish_targets();
      return true;
    }
  }
  return false;
}

void AutomationEngine::clear_targets() {
  // Keep `bound` so freshly bound targets reuse the cleared slots.
  for (size_t i = 0; i < control_targets_.bound; ++i) {
    control_targets_.slots[i] = TargetSlot{};
  }
  publish_targets();
}

void AutomationEngine::publish_targets() {
  targets_.publish(std::make_shared<const TargetTable>(control_targets_));
  targets_.synchronize();
}

rt::ProcessorBase* AutomationEngine::realtime_target(const TargetTable* table,
                                                     uint32_t param_id) noexcept {
  if (registered_parameter_rejects_realtime(param_id)) {
    non_realtime_safe_rejection_count_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  rt::ProcessorBase* processor = target_for(table, param_id);
  if (!processor) {
    unknown_target_count_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  if (!processor->parameter_is_realtime_safe(param_id)) {
    non_realtime_safe_rejection_count_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  return processor;
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
  // Pinned for the whole loop, so every processor resolved below outlives its use.
  const auto targets = targets_.read();
  for (const AutomationLane& lane : *lanes) {
    const uint32_t param_id = lane.target_param_id();
    // An empty lane has nothing to play back; value_at() on it reports a
    // constant 0.0, so skip it instead of driving any target to zero.
    if (lane.points().empty()) {
      continue;
    }
    if (routes_to_engine(param_id)) {
      // Engine-namespace lanes route directly to the engine runtime; they never
      // fall through to the bound-processor resolution below.
      const float value = lane.value_at(ppq);
      if (!std::isfinite(value)) {
        continue;
      }
      if (!engine_param_router_(engine_param_router_context_, param_id, value)) {
        unknown_target_count_.fetch_add(1, std::memory_order_relaxed);
      }
      continue;
    }
    rt::ProcessorBase* processor = realtime_target(targets.get(), param_id);
    if (!processor) {
      continue;
    }
    const float value = lane.value_at(ppq);
    // Never feed a non-finite breakpoint value into a processor parameter: a
    // NaN/Inf would poison that processor's state for the rest of the stream.
    if (!std::isfinite(value)) {
      continue;
    }
    processor->set_parameter(param_id, value);
  }
}

bool AutomationEngine::set_parameter(uint32_t param_id, float value) noexcept {
  if (!std::isfinite(value)) {
    return false;
  }
  if (routes_to_engine(param_id)) {
    // Mirror apply(): engine-namespace ids go through the router, never the
    // bound-processor table. A rejected target surfaces like an unknown one.
    const bool routed = engine_param_router_(engine_param_router_context_, param_id, value);
    if (!routed) {
      unknown_target_count_.fetch_add(1, std::memory_order_relaxed);
    }
    return routed;
  }
  const auto targets = targets_.read();
  rt::ProcessorBase* processor = realtime_target(targets.get(), param_id);
  if (!processor) {
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
  // A full boundary list means breakpoints past kCapacity were dropped this
  // block, so sub-block splitting (and thus per-breakpoint parameter updates)
  // is late. Surface it as a counter like the other apply-path diagnostics.
  if (out->overflowed) {
    boundary_overflow_count_.fetch_add(1, std::memory_order_relaxed);
  }
  out->sort_unique();
}

size_t AutomationEngine::lane_count() const noexcept {
  return lane_count_.load(std::memory_order_relaxed);
}

rt::ProcessorBase* AutomationEngine::target_for(const TargetTable* table,
                                                uint32_t param_id) const noexcept {
  if (param_id == 0) return nullptr;  // 0 is reserved as the invalid/none id.
  if (external_target_resolver_ != nullptr) {
    if (rt::ProcessorBase* processor =
            external_target_resolver_(external_target_context_, param_id)) {
      return processor;
    }
  }
  if (table == nullptr) return nullptr;
  // Scan the full bound range and skip cleared slots instead of stopping at the first one.
  for (size_t i = 0; i < table->bound; ++i) {
    const TargetSlot& slot = table->slots[i];
    if (slot.processor != nullptr && slot.param_id == param_id) {
      return slot.processor;
    }
  }
  return nullptr;
}

void AutomationEngine::notify_lane_targets_released(
    const std::vector<AutomationLane>* previous, const std::vector<AutomationLane>* next) noexcept {
  if (lane_release_callback_ == nullptr || previous == nullptr) return;
  // Merge-walk both sorted-by-target_param_id sets (set_lanes sorts and
  // de-duplicates before publishing). `next_index` only ever advances, so the
  // whole walk is O(previous->size() + next->size()) with no allocation.
  size_t next_index = 0;
  const size_t next_size = next != nullptr ? next->size() : 0;
  for (const AutomationLane& old_lane : *previous) {
    if (old_lane.points().empty()) continue;  // never drove anything; nothing to release.
    const uint32_t param_id = old_lane.target_param_id();
    while (next_index < next_size && (*next)[next_index].target_param_id() < param_id) {
      ++next_index;
    }
    const bool still_has_points = next_index < next_size &&
                                  (*next)[next_index].target_param_id() == param_id &&
                                  !(*next)[next_index].points().empty();
    if (!still_has_points) {
      lane_release_callback_(lane_release_context_, param_id);
    }
  }
}

bool AutomationEngine::registered_parameter_rejects_realtime(uint32_t param_id) const noexcept {
  const auto metadata = parameter_metadata_.read();
  if (!metadata) return false;
  const auto found =
      std::lower_bound(metadata->begin(), metadata->end(), param_id,
                       [](const ParameterInfo& info, uint32_t id) { return info.id < id; });
  return found != metadata->end() && found->id == param_id && !found->rt_safe;
}

}  // namespace sonare::automation
