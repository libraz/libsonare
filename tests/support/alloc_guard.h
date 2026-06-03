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
