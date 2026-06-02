#pragma once

/// @file automation_lane.h
/// @brief Bounded SPSC automation event lane for sample-accurate block splitting.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "util/automation_curve.h"

namespace sonare::mixing {

/// Alias for the canonical curve enum. Spelled `AutomationCurveType` here for
/// historical reasons (the sample-accurate mixer lane was the first consumer).
using AutomationCurveType = ::sonare::AutomationCurve;

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
  // Delegate to the single canonical curve definition so the engine and mixer
  // lanes share identical Exponential/SCurve/Hold shapes (they previously
  // diverged: this path only log-interpolated strictly-positive endpoints).
  return static_cast<float>(::sonare::interpolate_curve(a.curve, a.value, b.value, t));
}

// Bounded SPSC queue of AutomationEvent breakpoints with sample-accurate
// block consumption.
//
// Threading model:
//   * Producer (control thread): push().
//   * Consumer (audio thread):   consume_block(), discard_before(), clear().
//
// The atomic head_/tail_ pair gives the standard SPSC ring guarantees.
// In addition, the consumer-side fields active_event_ / has_active_event_
// (the "last-seen breakpoint" used to interpolate curves across block
// boundaries) are mutated by BOTH consume_block() and discard_before(),
// so those two MUST run on the same (audio) thread. ChannelStrip enforces
// this by calling both from process_at(), which is invoked only on the
// audio thread (RealtimeEngine, MixingRuntime, MonitorRuntime, and the
// graph-runtime StripNode all dispatch process_at() from the audio
// callback). See channel_strip.cpp for the NOTE at the top of process_at().
class AutomationLane {
 public:
  explicit AutomationLane(size_t capacity = 1024);

  size_t capacity() const noexcept { return capacity_; }
  bool empty() const noexcept;

  // Producer-side. Control thread only.
  bool push(const AutomationEvent& event) noexcept;

  // Consumer-side. Audio thread only. Must be serialized with consume_block()
  // on the same lane (both write active_event_/has_active_event_).
  size_t discard_before(int64_t sample_pos) noexcept;

  // Lane reset. Safe to call when the audio thread is stopped (e.g. during
  // ChannelStrip::reset()).
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

  // Producer-only (control thread); read/written exclusively in push().
  int64_t last_pushed_sample_ = 0;
  bool has_last_pushed_sample_ = false;

  // Consumer-only (audio thread); read/written in consume_block() and
  // discard_before(). Callers must serialize those two on the audio thread.
  AutomationEvent active_event_{};
  bool has_active_event_ = false;
};

}  // namespace sonare::mixing
