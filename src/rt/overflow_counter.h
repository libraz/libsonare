#pragma once

/// @file overflow_counter.h
/// @brief Realtime-safe overflow / drop telemetry counter shared by the
///        fixed-capacity RT producers (MIDI FX, MIDI routing, clock generation,
///        capture punch).
///
/// Several audio-thread producers write into a fixed-capacity output and DROP
/// any surplus once the buffer fills, surfacing the loss as a monotonically
/// increasing counter that a control / host thread polls. The producer (audio
/// thread) and the observer (control thread) run concurrently, so the counter
/// MUST be atomic to avoid a data race; the access is pure telemetry, so the
/// relaxed memory order is sufficient (no other state is published through it).
///
/// This is intentionally a tiny wrapper over `std::atomic<uint32_t>` rather than
/// raw atomics scattered across modules, so the increment / load / reset idiom
/// (and its relaxed-ordering rationale) lives in exactly one place. It is
/// trivially RT-safe: every operation is a single relaxed atomic with no lock,
/// no allocation and no I/O.

#include <atomic>
#include <cstdint>

namespace sonare::rt {

/// @brief Relaxed-atomic, RT-safe monotonically-increasing drop/overflow count.
///
/// The counter is NOT copyable (it owns an atomic); the owning RT object holds
/// it by value as a member. All operations are relaxed atomics: the count is
/// telemetry only and does not order any other memory.
class OverflowCounter {
 public:
  OverflowCounter() = default;

  /// @brief AUDIO thread: record one dropped item. RT-safe (single relaxed
  ///        atomic increment).
  void bump() noexcept { count_.fetch_add(1, std::memory_order_relaxed); }

  /// @brief AUDIO thread: record `n` dropped items in one step. RT-safe.
  void add(uint32_t n) noexcept { count_.fetch_add(n, std::memory_order_relaxed); }

  /// @brief ANY thread: current total drops since construction / reset.
  uint32_t load() const noexcept { return count_.load(std::memory_order_relaxed); }

  /// @brief AUDIO/CONTROL thread: clear the counter back to zero.
  void reset() noexcept { count_.store(0, std::memory_order_relaxed); }

 private:
  std::atomic<uint32_t> count_{0};
};

}  // namespace sonare::rt
