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
///          Between begin_block() and end_block() publishes are staged and folded to the deepest
///          value per insert, so a host block split into sub-blocks reads like the meter record;
///          a board nothing published to during the block keeps its count and reads 0.
class InsertGainReductionBoard {
 public:
  /// Upper bound on entries; matches SONARE_METER_MAX_INSERTS.
  static constexpr size_t kCapacity = 128;

  /// Opens a host block: publishes until end_block() are staged rather than stored.
  void begin_block() noexcept {
    in_block_ = true;
    staged_ = false;
  }

  /// Publishes the strip or bus's current per-insert values; @p source provides
  /// `size_t insert_gain_reduction_db(float* out, size_t capacity) const noexcept`.
  template <typename Source>
  void publish(const Source& source) noexcept {
    std::array<float, kCapacity> values;
    const size_t count =
        std::min(source.insert_gain_reduction_db(values.data(), values.size()), kCapacity);
    if (!in_block_) {
      store(values.data(), count);
      return;
    }
    if (staged_ && count == staged_count_) {
      for (size_t i = 0; i < count; ++i) staged_values_[i] = std::min(staged_values_[i], values[i]);
    } else {
      std::copy_n(values.begin(), count, staged_values_.begin());
      staged_count_ = count;
    }
    staged_ = true;
  }

  /// Marks the board empty (unused slot).
  void clear() noexcept {
    if (!in_block_) {
      count_.store(0, std::memory_order_relaxed);
      return;
    }
    staged_count_ = 0;
    staged_ = true;
  }

  /// Closes the host block and stores what it staged.
  void end_block() noexcept {
    if (!in_block_) return;
    in_block_ = false;
    if (staged_) {
      store(staged_values_.data(), staged_count_);
      return;
    }
    // The strip did not run this block, so no insert reduced anything.
    const size_t count = count_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count; ++i) values_[i].store(0.0f, std::memory_order_relaxed);
  }

  /// Copies min(@p capacity, count) entries into @p out and returns the full count.
  size_t read(float* out, size_t capacity) const noexcept {
    const size_t count = count_.load(std::memory_order_relaxed);
    const size_t n = count < capacity ? count : capacity;
    for (size_t i = 0; i < n; ++i) out[i] = values_[i].load(std::memory_order_relaxed);
    return count;
  }

 private:
  void store(const float* values, size_t count) noexcept {
    for (size_t i = 0; i < count; ++i) values_[i].store(values[i], std::memory_order_relaxed);
    count_.store(static_cast<uint32_t>(count), std::memory_order_relaxed);
  }

  std::atomic<uint32_t> count_{0};
  std::array<std::atomic<float>, kCapacity> values_{};
  // Writer-only staging for the open host block.
  std::array<float, kCapacity> staged_values_{};
  size_t staged_count_ = 0;
  bool in_block_ = false;
  bool staged_ = false;
};

}  // namespace sonare::engine
