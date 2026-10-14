#pragma once

// Private cooperative-budget bridge. This file intentionally lives below
// src/ rather than include/ and is not part of the installed API. The driver
// and built-in modules use one absolute deadline through the inline TLS state;
// a direct module call creates the same state locally from its request.

#include <chrono>
#include <cstdint>
#include <limits>

#include "midi/assist/assist_request.h"

namespace sonare::midi::assist::detail {

#if defined(__GNUC__) || defined(__clang__)
#define SONARE_ASSIST_BUDGET_DETAIL_HIDDEN __attribute__((visibility("hidden")))
#else
#define SONARE_ASSIST_BUDGET_DETAIL_HIDDEN
#endif

using AssistClock = std::chrono::steady_clock;
using AssistTimePoint = AssistClock::time_point;
using AssistNowFn = AssistTimePoint (*)() noexcept;

struct SONARE_ASSIST_BUDGET_DETAIL_HIDDEN DeadlineState {
  AssistTimePoint deadline = AssistTimePoint::max();
  bool has_deadline = false;

  bool active() const noexcept { return has_deadline; }
};

// Inline function-local TLS has one inline-variable identity across all TUs,
// unlike a namespace-scope `static thread_local` in every including TU.
SONARE_ASSIST_BUDGET_DETAIL_HIDDEN inline DeadlineState*& current_deadline_slot() noexcept {
  static thread_local DeadlineState* current = nullptr;
  return current;
}

SONARE_ASSIST_BUDGET_DETAIL_HIDDEN inline AssistNowFn& current_now_slot() noexcept {
  static thread_local AssistNowFn current = nullptr;
  return current;
}

SONARE_ASSIST_BUDGET_DETAIL_HIDDEN inline AssistTimePoint now() noexcept {
  const AssistNowFn override_now = current_now_slot();
  return override_now != nullptr ? override_now() : AssistClock::now();
}

SONARE_ASSIST_BUDGET_DETAIL_HIDDEN inline AssistTimePoint saturating_deadline(
    AssistTimePoint start, uint32_t max_time_ms) noexcept {
  if (max_time_ms == 0) return AssistTimePoint::max();
  const auto delta =
      std::chrono::duration_cast<AssistClock::duration>(std::chrono::milliseconds(max_time_ms));
  if (start > AssistTimePoint::max() - delta) return AssistTimePoint::max();
  return start + delta;
}

SONARE_ASSIST_BUDGET_DETAIL_HIDDEN inline DeadlineState effective_deadline(
    const AssistRequest& request) noexcept {
  DeadlineState state;
  if (request.budget.max_time_ms != 0) {
    state.deadline = saturating_deadline(now(), request.budget.max_time_ms);
    state.has_deadline = true;
  }
  const DeadlineState* parent = current_deadline_slot();
  if (parent != nullptr && parent->active() &&
      (!state.active() || parent->deadline < state.deadline)) {
    state.deadline = parent->deadline;
    state.has_deadline = true;
  }
  return state;
}

class SONARE_ASSIST_BUDGET_DETAIL_HIDDEN DeadlineScope final {
 public:
  explicit DeadlineScope(DeadlineState& state) noexcept : previous_(current_deadline_slot()) {
    current_deadline_slot() = &state;
  }

  DeadlineScope(const DeadlineScope&) = delete;
  DeadlineScope& operator=(const DeadlineScope&) = delete;

  ~DeadlineScope() noexcept { current_deadline_slot() = previous_; }

 private:
  DeadlineState* previous_ = nullptr;
};

class SONARE_ASSIST_BUDGET_DETAIL_HIDDEN RequestDeadlineScope final {
 public:
  explicit RequestDeadlineScope(const AssistRequest& request) noexcept
      : state_(effective_deadline(request)), scope_(state_) {}

  RequestDeadlineScope(const RequestDeadlineScope&) = delete;
  RequestDeadlineScope& operator=(const RequestDeadlineScope&) = delete;

 private:
  DeadlineState state_;
  DeadlineScope scope_;
};

class SONARE_ASSIST_BUDGET_DETAIL_HIDDEN NowFnScope final {
 public:
  explicit NowFnScope(AssistNowFn now_fn) noexcept : previous_(current_now_slot()) {
    current_now_slot() = now_fn;
  }

  NowFnScope(const NowFnScope&) = delete;
  NowFnScope& operator=(const NowFnScope&) = delete;

  ~NowFnScope() noexcept { current_now_slot() = previous_; }

 private:
  AssistNowFn previous_ = nullptr;
};

SONARE_ASSIST_BUDGET_DETAIL_HIDDEN inline const DeadlineState* current_deadline() noexcept {
  return current_deadline_slot();
}

SONARE_ASSIST_BUDGET_DETAIL_HIDDEN inline bool deadline_expired() noexcept {
  const DeadlineState* state = current_deadline();
  return state != nullptr && state->active() && now() >= state->deadline;
}

// Returns 0 for no active deadline and for an already exhausted deadline.
// A positive sub-millisecond remainder is rounded upward to one millisecond.
SONARE_ASSIST_BUDGET_DETAIL_HIDDEN inline uint32_t remaining_time_ms() noexcept {
  const DeadlineState* state = current_deadline();
  if (state == nullptr || !state->active()) return 0;
  const AssistTimePoint current = now();
  if (current >= state->deadline) return 0;
  // Avoid `deadline - current` when a valid monotonic clock starts below zero
  // and the deadline is at/near the positive end of the duration. The direct
  // subtraction would overflow even though the ordering above is valid. The
  // only branch that can exceed duration::max() is negative-to-nonnegative;
  // all other ordered pairs have a representable difference.
  using Duration = AssistClock::duration;
  using Rep = Duration::rep;
  const Rep current_count = current.time_since_epoch().count();
  const Rep deadline_count = state->deadline.time_since_epoch().count();
  Duration remainder;
  if (current_count < 0 && deadline_count >= 0 &&
      deadline_count > std::numeric_limits<Rep>::max() + current_count) {
    remainder = Duration::max();
  } else {
    remainder = Duration(deadline_count - current_count);
  }
  auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remainder).count();
  constexpr auto kMax = static_cast<decltype(milliseconds)>(std::numeric_limits<uint32_t>::max());
  // Avoid converting a very large millisecond count back to the clock
  // duration: for a nanosecond clock that multiplication would itself
  // overflow before the result can be clamped to the public uint32_t field.
  if (milliseconds >= kMax) return std::numeric_limits<uint32_t>::max();
  if (std::chrono::duration_cast<AssistClock::duration>(std::chrono::milliseconds(milliseconds)) <
      remainder) {
    ++milliseconds;
  }
  if (milliseconds < 1) milliseconds = 1;
  if (milliseconds > kMax) return std::numeric_limits<uint32_t>::max();
  return static_cast<uint32_t>(milliseconds);
}

SONARE_ASSIST_BUDGET_DETAIL_HIDDEN inline uint32_t saturating_add_u32(uint32_t left,
                                                                      uint32_t right) noexcept {
  constexpr uint32_t kMax = std::numeric_limits<uint32_t>::max();
  if (right > kMax - left) return kMax;
  return left + right;
}

}  // namespace sonare::midi::assist::detail

#undef SONARE_ASSIST_BUDGET_DETAIL_HIDDEN
