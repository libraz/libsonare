#include <algorithm>
#include <cmath>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "mixing/channel_strip.h"

namespace sonare::mixing {

bool ChannelStrip::schedule_width_automation(int64_t sample_pos, float width,
                                             AutomationCurveType curve) noexcept {
  return schedule_width_automation_result(sample_pos, width, curve) ==
         AutomationPushResult::Success;
}

AutomationPushResult ChannelStrip::schedule_width_automation_result(
    int64_t sample_pos, float width, AutomationCurveType curve) noexcept {
  if (!std::isfinite(width)) return AutomationPushResult::NonMonotonic;
  AutomationEvent event;
  event.sample_pos = sample_pos;
  event.value = width;
  event.curve = curve;
  event.target.kind = AutomationTargetKind::Width;
  return width_automation_.try_push(event);
}

bool ChannelStrip::schedule_fader_automation(int64_t sample_pos, float fader_db,
                                             AutomationCurveType curve) noexcept {
  return schedule_fader_automation_result(sample_pos, fader_db, curve) ==
         AutomationPushResult::Success;
}

AutomationPushResult ChannelStrip::schedule_fader_automation_result(
    int64_t sample_pos, float fader_db, AutomationCurveType curve) noexcept {
  if (!std::isfinite(fader_db)) return AutomationPushResult::NonMonotonic;
  AutomationEvent event;
  event.sample_pos = sample_pos;
  event.value = fader_db;
  event.curve = curve;
  event.target.kind = AutomationTargetKind::Fader;
  return fader_automation_.try_push(event);
}

bool ChannelStrip::schedule_pan_automation(int64_t sample_pos, float pan,
                                           AutomationCurveType curve) noexcept {
  return schedule_pan_automation_result(sample_pos, pan, curve) == AutomationPushResult::Success;
}

AutomationPushResult ChannelStrip::schedule_pan_automation_result(
    int64_t sample_pos, float pan, AutomationCurveType curve) noexcept {
  if (!std::isfinite(pan)) return AutomationPushResult::NonMonotonic;
  AutomationEvent event;
  event.sample_pos = sample_pos;
  event.value = pan;
  event.curve = curve;
  event.target.kind = AutomationTargetKind::Pan;
  return pan_automation_.try_push(event);
}

bool ChannelStrip::set_insert_bypassed(unsigned int insert_index, bool bypassed,
                                       bool reset_on_bypass) noexcept {
  const size_t idx = insert_index;
  const size_t pre_count = pre_inserts_.size();
  rt::ProcessorBase* insert = nullptr;
  if (idx < pre_count) {
    insert = pre_inserts_[idx].get();
  } else if (idx - pre_count < post_inserts_.size()) {
    insert = post_inserts_[idx - pre_count].get();
  }
  return insert != nullptr && insert->set_bypassed(bypassed, reset_on_bypass);
}

InsertAutomationScheduleResult ChannelStrip::schedule_insert_automation_result(
    unsigned int insert_index, unsigned int param_id, int64_t sample_pos, float value,
    AutomationCurveType curve) noexcept {
  constexpr unsigned int kMaxReasonableParamId = 65535u;
  if (param_id > kMaxReasonableParamId || !std::isfinite(value)) {
    return InsertAutomationScheduleResult::InvalidParameter;
  }
  const size_t idx = insert_index;
  const size_t pre_count = pre_inserts_.size();
  rt::ProcessorBase* insert = nullptr;
  if (idx < pre_count) {
    insert = pre_inserts_[idx].get();
  } else if (idx - pre_count < post_inserts_.size()) {
    insert = post_inserts_[idx - pre_count].get();
  }
  if (insert == nullptr) {
    return InsertAutomationScheduleResult::InvalidParameter;
  }

  try {
    const std::vector<rt::ParamDescriptor> descriptors = insert->parameter_descriptors();
    const bool exists = std::any_of(
        descriptors.begin(), descriptors.end(),
        [param_id](const rt::ParamDescriptor& descriptor) { return descriptor.id == param_id; });
    if (!exists) {
      return InsertAutomationScheduleResult::InvalidParameter;
    }
  } catch (const std::bad_alloc&) {
    return InsertAutomationScheduleResult::OutOfMemory;
  } catch (...) {
    return InsertAutomationScheduleResult::InvalidParameter;
  }
  if (!insert->parameter_is_realtime_safe(param_id)) {
    return InsertAutomationScheduleResult::NotSupported;
  }

  AutomationEvent event;
  event.sample_pos = sample_pos;
  event.value = value;
  event.curve = curve;
  event.target.kind = AutomationTargetKind::InsertParameter;
  event.target.insert_index = insert_index;
  event.target.param_id = param_id;
  // An out-of-order time is a bad argument; only a full ring is capacity exhaustion.
  const auto push_result = [&event](AutomationLane& lane) noexcept {
    switch (lane.try_push(event)) {
      case AutomationPushResult::Success:
        return InsertAutomationScheduleResult::Success;
      case AutomationPushResult::NonMonotonic:
        return InsertAutomationScheduleResult::InvalidParameter;
      case AutomationPushResult::Full:
        break;
    }
    return InsertAutomationScheduleResult::OutOfMemory;
  };
  // Control thread is the sole writer; only the published slots may already be
  // visible to the audio thread, so scan [0, published) for an existing lane.
  const size_t published = insert_automation_size_.load(std::memory_order_relaxed);
  for (size_t li = 0; li < published; ++li) {
    InsertAutomationLane& lane = insert_automation_[li];
    if (lane.target == event.target && lane.lane) {
      return push_result(*lane.lane);
    }
  }
  // Hard cap: the push_back below MUST NOT reallocate, because the audio thread
  // may concurrently index insert_automation_ in process_at(). Capacity is
  // reserved up-front in the constructor (kMaxInsertAutomationLanes).
  if (published >= kMaxInsertAutomationLanes) {
    return InsertAutomationScheduleResult::OutOfMemory;
  }
  // Fully construct the new lane into the reserved slot, then publish the new
  // size with release ordering so the audio thread only observes a complete
  // element. The reader pairs this with an acquire load.
  try {
    auto lane = std::make_unique<AutomationLane>();
    const InsertAutomationScheduleResult first = push_result(*lane);
    if (first != InsertAutomationScheduleResult::Success) return first;
    insert_automation_.push_back({event.target, std::move(lane)});
    insert_automation_size_.store(insert_automation_.size(), std::memory_order_release);
    return InsertAutomationScheduleResult::Success;
  } catch (const std::bad_alloc&) {
    return InsertAutomationScheduleResult::OutOfMemory;
  } catch (...) {
    return InsertAutomationScheduleResult::InvalidParameter;
  }
}

bool ChannelStrip::schedule_insert_automation(unsigned int insert_index, unsigned int param_id,
                                              int64_t sample_pos, float value,
                                              AutomationCurveType curve) noexcept {
  return schedule_insert_automation_result(insert_index, param_id, sample_pos, value, curve) ==
         InsertAutomationScheduleResult::Success;
}

void ChannelStrip::apply_automation_event(const AutomationEvent& event) noexcept {
  switch (event.target.kind) {
    case AutomationTargetKind::Fader:
      set_fader_db(event.value);
      break;
    case AutomationTargetKind::Pan:
      set_pan(event.value);
      break;
    case AutomationTargetKind::Width:
      set_width(event.value);
      break;
    case AutomationTargetKind::InsertParameter: {
      // insert_index addresses the combined sequence [pre_inserts_ ... post_inserts_ ...].
      const size_t idx = event.target.insert_index;
      const size_t pre_count = pre_inserts_.size();
      rt::ProcessorBase* insert = nullptr;
      if (idx < pre_count) {
        insert = pre_inserts_[idx].get();
      } else if (idx - pre_count < post_inserts_.size()) {
        insert = post_inserts_[idx - pre_count].get();
      }
      if (insert != nullptr && insert->parameter_is_realtime_safe(event.target.param_id)) {
        // Ignore unrecognized ids / out-of-range; no-op on failure.
        insert->set_parameter(event.target.param_id, event.value);
      }
      break;
    }
    case AutomationTargetKind::Send:
      // Send automation is consumed separately from the send_automation_ lanes in
      // mix_send_at, not through this function, so this case is intentionally a no-op
      // (kept only for -Wswitch exhaustiveness).
      break;
  }
}

bool ChannelStrip::apply_insert_parameter(unsigned int insert_index, unsigned int param_id,
                                          float value) noexcept {
  // insert_index addresses the combined sequence [pre_inserts_ ... post_inserts_ ...].
  const size_t idx = insert_index;
  const size_t pre_count = pre_inserts_.size();
  rt::ProcessorBase* insert = nullptr;
  if (idx < pre_count) {
    insert = pre_inserts_[idx].get();
  } else if (idx - pre_count < post_inserts_.size()) {
    insert = post_inserts_[idx - pre_count].get();
  }
  if (insert == nullptr || !insert->parameter_is_realtime_safe(param_id)) {
    return false;
  }
  return insert->set_parameter(param_id, value);
}

bool ChannelStrip::constructed_insert_parameter_value(unsigned int insert_index,
                                                      unsigned int param_id,
                                                      float* out) const noexcept {
  const size_t idx = insert_index;
  const size_t pre_count = pre_inserts_.size();
  const rt::ProcessorBase* insert = nullptr;
  if (idx < pre_count) {
    insert = pre_inserts_[idx].get();
  } else if (idx - pre_count < post_inserts_.size()) {
    insert = post_inserts_[idx - pre_count].get();
  }
  return insert != nullptr && insert->constructed_parameter_value(param_id, out);
}

int ChannelStrip::insert_parameter_id_for_key(unsigned int insert_index,
                                              const std::string& key) const noexcept {
  const size_t idx = insert_index;
  const size_t pre_count = pre_inserts_.size();
  const rt::ProcessorBase* insert = nullptr;
  if (idx < pre_count) {
    insert = pre_inserts_[idx].get();
  } else if (idx - pre_count < post_inserts_.size()) {
    insert = post_inserts_[idx - pre_count].get();
  }
  if (insert == nullptr) {
    return -1;
  }
  for (const auto& desc : insert->parameter_descriptors()) {
    if (desc.key == key) {
      return static_cast<int>(desc.id);
    }
  }
  return -1;
}

}  // namespace sonare::mixing
