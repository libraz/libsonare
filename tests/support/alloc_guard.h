#pragma once

/// @file alloc_guard.h
/// @brief Shared heap-allocation counting for RT no-alloc tests.
///
/// The global `operator new` / `operator delete` overrides that feed these
/// counters are defined ONCE, in tests/mixing/no_alloc_test.cpp (defining them
/// in more than one translation unit would be an ODR / duplicate-symbol error).
/// This header exposes only the counters and the scoped @ref AllocationGuard so
/// that multiple no-alloc test TUs share a single, consistent mechanism.

#include <atomic>
#include <cstddef>

namespace sonare::test {

// inline (C++17) gives these a single definition across all TUs.
inline std::atomic<bool> g_count_allocations{false};
inline std::atomic<std::size_t> g_allocation_count{0};

inline void note_allocation() noexcept {
  if (g_count_allocations.load(std::memory_order_relaxed)) {
    g_allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
}

// Failure injection for the paths that can only be reached through a heap
// failure. Thresholded rather than blanket: the interesting allocation is a
// delay line or a scratch bank, and failing every allocation while armed would
// take Catch2's own bookkeeping down with it and report a crash instead of the
// return value under test. Zero means armed for nothing.
inline std::atomic<std::size_t> g_fail_allocations_from_bytes{0};

// One-shot failure injection for atomic-result tests. Unlike the threshold
// hook above, this lets a test fail the Nth allocation while leaving all other
// allocations available, so it can reach the output-copy allocation after
// the core work has completed.
inline std::atomic<std::size_t> g_fail_allocation_number{0};
inline std::atomic<std::size_t> g_allocation_attempts{0};

/// Whether an allocation of @p size must fail. Called from the global
/// `operator new` overrides before any memory is taken.
inline bool allocation_should_fail_once() noexcept {
  const std::size_t target = g_fail_allocation_number.load(std::memory_order_relaxed);
  if (target == 0) return false;
  const std::size_t attempt = g_allocation_attempts.fetch_add(1, std::memory_order_relaxed) + 1;
  return attempt == target;
}

inline bool allocation_should_fail(std::size_t size) noexcept {
  const std::size_t threshold = g_fail_allocations_from_bytes.load(std::memory_order_relaxed);
  return (threshold != 0 && size >= threshold) || allocation_should_fail_once();
}

/// Scoped guard: makes every allocation of @p from_bytes or larger throw
/// `std::bad_alloc` while it is alive. Use the smallest threshold that still
/// clears whatever the surrounding test framework allocates, so the failure
/// lands on the buffer under test and nowhere else.
class AllocationFailureGuard {
 public:
  explicit AllocationFailureGuard(std::size_t from_bytes) {
    g_fail_allocations_from_bytes.store(from_bytes, std::memory_order_relaxed);
  }
  ~AllocationFailureGuard() { g_fail_allocations_from_bytes.store(0, std::memory_order_relaxed); }

  AllocationFailureGuard(const AllocationFailureGuard&) = delete;
  AllocationFailureGuard& operator=(const AllocationFailureGuard&) = delete;
};

/// Scoped guard: fails exactly the @p nth allocation observed by the global
/// test allocator, then allows all later allocations. Keep the guard around
/// only the API call under test; destroy it before assertions so Catch2's own
/// allocations cannot consume or trigger the injected failure.
class AllocationFailureAtGuard {
 public:
  explicit AllocationFailureAtGuard(std::size_t nth) {
    g_allocation_attempts.store(0, std::memory_order_relaxed);
    g_fail_allocation_number.store(nth, std::memory_order_relaxed);
  }
  ~AllocationFailureAtGuard() {
    g_fail_allocation_number.store(0, std::memory_order_relaxed);
    g_allocation_attempts.store(0, std::memory_order_relaxed);
  }

  AllocationFailureAtGuard(const AllocationFailureAtGuard&) = delete;
  AllocationFailureAtGuard& operator=(const AllocationFailureAtGuard&) = delete;
};

/// Scoped guard: zeroes and arms the global allocation counter on construction,
/// disarms on destruction. count() reports allocations observed while armed.
class AllocationGuard {
 public:
  AllocationGuard() {
    g_allocation_count.store(0, std::memory_order_relaxed);
    g_count_allocations.store(true, std::memory_order_relaxed);
  }
  ~AllocationGuard() { g_count_allocations.store(false, std::memory_order_relaxed); }
  std::size_t count() const noexcept { return g_allocation_count.load(std::memory_order_relaxed); }
};

}  // namespace sonare::test
