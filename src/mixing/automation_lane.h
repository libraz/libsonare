#pragma once

/// @file automation_lane.h
/// @brief Bounded SPSC automation event lane for sample-accurate block splitting.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sonare::mixing {

enum class AutomationCurveType {
  Linear,
  Exponential,
};

enum class AutomationTargetKind {
  Fader,
  Pan,
  Send,
  Width,
  InsertParameter,
};

struct AutomationTarget {
  AutomationTargetKind kind = AutomationTargetKind::Fader;
  uint32_t strip_id = 0;
  uint32_t insert_index = 0;
  uint32_t param_id = 0;

  friend bool operator==(const AutomationTarget& lhs, const AutomationTarget& rhs) noexcept {
    return lhs.kind == rhs.kind && lhs.strip_id == rhs.strip_id &&
           lhs.insert_index == rhs.insert_index && lhs.param_id == rhs.param_id;
  }
};

struct AutomationEvent {
  int64_t sample_pos = 0;
  float value = 0.0f;
  AutomationCurveType curve = AutomationCurveType::Linear;
  AutomationTarget target{};
};

struct AutomationBlockEvent {
  AutomationEvent event{};
  int offset = 0;
};

class AutomationLane {
 public:
  explicit AutomationLane(size_t capacity = 1024);

  size_t capacity() const noexcept { return capacity_; }
  bool empty() const noexcept;
  bool push(const AutomationEvent& event) noexcept;
  void clear() noexcept;

  template <typename Callback>
  size_t consume_block(int64_t block_start, int num_samples, Callback&& callback) {
    if (num_samples <= 0) {
      return 0;
    }

    const int64_t block_end = block_start + static_cast<int64_t>(num_samples);
    size_t consumed = 0;
    for (;;) {
      const size_t tail = tail_.load(std::memory_order_relaxed);
      const size_t head = head_.load(std::memory_order_acquire);
      if (tail == head) {
        return consumed;
      }

      const AutomationEvent& event = buffer_[tail];
      if (event.sample_pos >= block_end) {
        return consumed;
      }

      const size_t next_tail = increment(tail);
      tail_.store(next_tail, std::memory_order_release);

      if (event.sample_pos < block_start) {
        continue;
      }

      callback(AutomationBlockEvent{event, static_cast<int>(event.sample_pos - block_start)});
      ++consumed;
    }
  }

 private:
  size_t increment(size_t index) const noexcept;

  size_t capacity_ = 0;
  std::vector<AutomationEvent> buffer_;
  std::atomic<size_t> head_{0};
  std::atomic<size_t> tail_{0};
  int64_t last_pushed_sample_ = 0;
  bool has_last_pushed_sample_ = false;
};

}  // namespace sonare::mixing
