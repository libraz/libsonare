#pragma once

/// @file automation_lane.h
/// @brief Bounded SPSC automation event lane for sample-accurate block splitting.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sonare::mixing {

enum class AutomationCurveType {
  Linear,
  Exponential,
  Hold,
  SCurve,
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

inline float interpolate_automation_value(const AutomationEvent& a, const AutomationEvent& b,
                                          double sample_pos) noexcept {
  const double span = static_cast<double>(b.sample_pos - a.sample_pos);
  if (span <= 0.0) return b.value;
  const double t = std::clamp((sample_pos - static_cast<double>(a.sample_pos)) / span, 0.0, 1.0);
  switch (a.curve) {
    case AutomationCurveType::Hold:
      return a.value;
    case AutomationCurveType::Exponential:
      if (a.value > 0.0f && b.value > 0.0f) {
        return static_cast<float>(
            std::exp(std::log(a.value) + (std::log(b.value) - std::log(a.value)) * t));
      }
      return static_cast<float>(a.value + (b.value - a.value) * t);
    case AutomationCurveType::SCurve: {
      const double shaped = t * t * (3.0 - 2.0 * t);
      return static_cast<float>(a.value + (b.value - a.value) * shaped);
    }
    case AutomationCurveType::Linear:
    default:
      return static_cast<float>(a.value + (b.value - a.value) * t);
  }
}

class AutomationLane {
 public:
  explicit AutomationLane(size_t capacity = 1024);

  size_t capacity() const noexcept { return capacity_; }
  bool empty() const noexcept;
  bool push(const AutomationEvent& event) noexcept;
  size_t discard_before(int64_t sample_pos) noexcept;
  void clear() noexcept;

  template <typename Callback>
  size_t consume_block(int64_t block_start, int num_samples, Callback&& callback) {
    if (num_samples <= 0) {
      return 0;
    }

    const int64_t block_end = block_start + static_cast<int64_t>(num_samples);
    size_t consumed = 0;

    auto emit_curve_events = [&](const AutomationEvent& start, const AutomationEvent& end,
                                 int64_t emit_start, int64_t emit_end) {
      if (!(start.target == end.target) || start.curve == AutomationCurveType::Hold ||
          end.sample_pos <= start.sample_pos) {
        return;
      }
      const int64_t first_sample = std::max({emit_start, block_start, start.sample_pos + 1});
      const int64_t last_sample = std::min({emit_end, block_end - 1, end.sample_pos - 1});
      if (first_sample > last_sample) {
        return;
      }

      constexpr int64_t kMaxSyntheticCurveEvents = 64;
      const int64_t available = last_sample - first_sample + 1;
      const int64_t step = std::max<int64_t>(
          1, (available + kMaxSyntheticCurveEvents - 1) / kMaxSyntheticCurveEvents);
      for (int64_t sample = first_sample; sample <= last_sample; sample += step) {
        AutomationEvent shaped = start;
        shaped.sample_pos = sample;
        shaped.value = interpolate_automation_value(start, end, static_cast<double>(sample));
        callback(AutomationBlockEvent{shaped, static_cast<int>(sample - block_start)});
        ++consumed;
      }
    };

    for (;;) {
      const size_t tail = tail_.load(std::memory_order_relaxed);
      const size_t head = head_.load(std::memory_order_acquire);
      if (tail == head) {
        return consumed;
      }

      const AutomationEvent& event = buffer_[tail];
      if (event.sample_pos >= block_end) {
        if (has_active_event_ && active_event_.sample_pos < block_start) {
          emit_curve_events(active_event_, event, block_start, block_end - 1);
        }
        return consumed;
      }

      if (has_active_event_ && active_event_.sample_pos < block_start &&
          event.sample_pos > block_start) {
        emit_curve_events(active_event_, event, block_start, event.sample_pos - 1);
      }

      const size_t next_tail = increment(tail);
      tail_.store(next_tail, std::memory_order_release);

      if (event.sample_pos < block_start) {
        active_event_ = event;
        has_active_event_ = true;
        continue;
      }

      callback(AutomationBlockEvent{event, static_cast<int>(event.sample_pos - block_start)});
      ++consumed;
      active_event_ = event;
      has_active_event_ = true;

      const size_t peek_tail = next_tail;
      const size_t latest_head = head_.load(std::memory_order_acquire);
      if (peek_tail != latest_head) {
        const AutomationEvent& next_event = buffer_[peek_tail];
        emit_curve_events(event, next_event, event.sample_pos + 1, block_end - 1);
      }
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
  AutomationEvent active_event_{};
  bool has_active_event_ = false;
};

}  // namespace sonare::mixing
