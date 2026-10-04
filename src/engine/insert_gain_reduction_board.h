#pragma once

/// @file insert_gain_reduction_board.h
/// @brief Lock-free publication of one meter target's per-insert gain reduction.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace sonare::engine {

/// @brief Single-writer (audio thread), any-reader board of per-insert gain reduction in dB.
/// @details Entries and the count are independent relaxed atomics: a reader racing a publish may
///          pair the new count with some old entries. It is a meter, so no cross-entry atomicity.
class InsertGainReductionBoard {
 public:
  /// Upper bound on entries; matches SONARE_METER_MAX_INSERTS.
  static constexpr size_t kCapacity = 128;

  /// Publishes the strip or bus's current per-insert values; @p source provides
  /// `size_t insert_gain_reduction_db(float* out, size_t capacity) const noexcept`.
  template <typename Source>
  void publish(const Source& source) noexcept {
    std::array<float, kCapacity> values;
    const size_t count =
        std::min(source.insert_gain_reduction_db(values.data(), values.size()), kCapacity);
    for (size_t i = 0; i < count; ++i) values_[i].store(values[i], std::memory_order_relaxed);
    count_.store(static_cast<uint32_t>(count), std::memory_order_relaxed);
  }

  /// Marks the board empty (unused slot).
  void clear() noexcept { count_.store(0, std::memory_order_relaxed); }

  /// Copies min(@p capacity, count) entries into @p out and returns the full count.
  size_t read(float* out, size_t capacity) const noexcept {
    const size_t count = count_.load(std::memory_order_relaxed);
    const size_t n = count < capacity ? count : capacity;
    for (size_t i = 0; i < n; ++i) out[i] = values_[i].load(std::memory_order_relaxed);
    return count;
  }

 private:
  std::atomic<uint32_t> count_{0};
  std::array<std::atomic<float>, kCapacity> values_{};
};

}  // namespace sonare::engine
