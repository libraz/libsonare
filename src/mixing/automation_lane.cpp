#include "mixing/automation_lane.h"

#include <algorithm>

namespace sonare::mixing {

AutomationLane::AutomationLane(size_t capacity)
    : capacity_(std::max<size_t>(1, capacity)),
      buffer_(capacity_ + 1),
      last_pushed_sample_(0),
      has_last_pushed_sample_(false) {}

bool AutomationLane::empty() const noexcept {
  return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
}

bool AutomationLane::push(const AutomationEvent& event) noexcept {
  if (has_last_pushed_sample_ && event.sample_pos < last_pushed_sample_) {
    return false;
  }

  const size_t head = head_.load(std::memory_order_relaxed);
  const size_t next_head = increment(head);
  if (next_head == tail_.load(std::memory_order_acquire)) {
    return false;
  }

  buffer_[head] = event;
  head_.store(next_head, std::memory_order_release);
  last_pushed_sample_ = event.sample_pos;
  has_last_pushed_sample_ = true;
  return true;
}

void AutomationLane::clear() noexcept {
  tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
  has_last_pushed_sample_ = false;
  last_pushed_sample_ = 0;
  has_active_event_ = false;
}

size_t AutomationLane::increment(size_t index) const noexcept {
  ++index;
  return index == buffer_.size() ? 0 : index;
}

}  // namespace sonare::mixing
