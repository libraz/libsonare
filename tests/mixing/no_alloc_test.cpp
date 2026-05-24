#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <memory>
#include <new>

#include "mastering/dynamics/compressor.h"
#include "mixing/channel_strip.h"

namespace {

std::atomic<bool> g_count_allocations{false};
std::atomic<size_t> g_allocation_count{0};

void note_allocation() noexcept {
  if (g_count_allocations.load(std::memory_order_relaxed)) {
    g_allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
}

void* allocate_bytes(std::size_t size) {
  note_allocation();
  if (void* ptr = std::malloc(size == 0 ? 1 : size)) {
    return ptr;
  }
  throw std::bad_alloc();
}

void* allocate_aligned_bytes(std::size_t size, std::size_t alignment) {
  note_allocation();
  void* ptr = nullptr;
  const std::size_t actual_size = size == 0 ? 1 : size;
  if (posix_memalign(&ptr, alignment, actual_size) == 0 && ptr != nullptr) {
    return ptr;
  }
  throw std::bad_alloc();
}

class AllocationGuard {
 public:
  AllocationGuard() {
    g_allocation_count.store(0, std::memory_order_relaxed);
    g_count_allocations.store(true, std::memory_order_relaxed);
  }
  ~AllocationGuard() { g_count_allocations.store(false, std::memory_order_relaxed); }
  size_t count() const noexcept { return g_allocation_count.load(std::memory_order_relaxed); }
};

}  // namespace

void* operator new(std::size_t size) { return allocate_bytes(size); }
void* operator new[](std::size_t size) { return allocate_bytes(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  return allocate_aligned_bytes(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return allocate_aligned_bytes(size, static_cast<std::size_t>(alignment));
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return allocate_bytes(size);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return allocate_bytes(size);
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::align_val_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept { std::free(ptr); }
void operator delete(void* ptr, const std::nothrow_t&) noexcept { std::free(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept { std::free(ptr); }

TEST_CASE("ChannelStrip process performs no heap allocation after prepare", "[mixing][rt]") {
  constexpr int kBlock = 256;
  sonare::mixing::ChannelStrip strip;
  strip.add_pre_insert(std::make_unique<sonare::mastering::dynamics::Compressor>(
      sonare::mastering::dynamics::CompressorConfig{}));
  strip.prepare(48000.0, kBlock);
  strip.set_polarity_invert(true, false);
  strip.set_channel_delay_samples(3);
  strip.set_fader_db(-3.0f);
  strip.set_pan(0.2f);
  strip.set_width(1.25f);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 1.0f : 0.05f;
    right[static_cast<size_t>(i)] = 0.03f;
  }
  float* channels[] = {left.data(), right.data()};

  strip.process(channels, 2, kBlock);
  strip.reset();

  AllocationGuard guard;
  strip.process(channels, 2, kBlock);
  const size_t allocations = guard.count();

  REQUIRE(allocations == 0);
}
