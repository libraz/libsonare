#pragma once

/// @file seqlock_cell.h
/// @brief Single-writer / single-reader seqlock for handing a small trivially
///        copyable POD snapshot to the audio thread without a lock or alloc.
///
/// A seqlock pairs the published value with an even/odd guard counter. The
/// WRITER (control thread) bumps the guard to odd before the store and back to
/// even after, so a reader that observes an odd guard — or a guard that moved
/// across its copy — knows it raced an in-progress write and read a torn value.
///
/// Two reader idioms are provided, mirroring the two hand-written seqlocks this
/// primitive replaces (transport LoopState and mixing MeterSnapshot):
///
///  - @ref load() spins until it observes a consistent (untorn) snapshot. Use
///    it where the reader can tolerate a bounded spin (e.g. a control/host
///    thread polling the meter).
///
///  - @ref try_load() makes a SINGLE non-spinning attempt; on a detected
///    conflict it returns the last consistent value it cached instead of
///    spinning up to a scheduler tick (which would risk an xrun if the writer
///    were preempted mid-update). Use it on the audio thread.
///
/// The guard transitions use release on the writer side and acquire on the
/// reader side, with an acquire fence between the value copy and the second
/// guard load, so the copy cannot be reordered past the guard check. The value
/// is stored as lock-free atomic 32-bit words: a guard cannot make concurrent
/// access to a plain POD legal in the C++ memory model, even when torn reads are
/// detected and discarded. `T` must be trivially copyable.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace sonare::rt {

/// @brief Single-writer / single-reader seqlock cell for a POD snapshot.
/// @tparam T trivially copyable snapshot type.
template <typename T>
class SeqlockCell {
 public:
  static_assert(std::is_trivially_copyable<T>::value,
                "SeqlockCell<T> requires a trivially copyable snapshot type");
  static_assert(std::atomic<uint32_t>::is_always_lock_free,
                "SeqlockCell requires lock-free 32-bit atomics");

  SeqlockCell() noexcept { store_words(T{}); }
  explicit SeqlockCell(const T& initial) noexcept : cached_(initial) { store_words(initial); }

  /// @brief Publishes a new snapshot. Control-thread only. The guard is odd for
  ///        the duration of the store so a concurrent reader detects the write.
  void store(const T& value) noexcept {
    guard_.fetch_add(1, std::memory_order_acq_rel);  // now odd: write in progress
    store_words(value);
    guard_.fetch_add(1, std::memory_order_release);  // now even: write complete
  }

  /// @brief Spins until it reads a consistent snapshot. Tolerates a bounded
  ///        spin; do not call on the audio thread.
  T load() const noexcept {
    for (;;) {
      const uint32_t g1 = guard_.load(std::memory_order_acquire);
      if (g1 & 1u) continue;  // writer mid-update
      const T copy = load_words();
      std::atomic_thread_fence(std::memory_order_acquire);
      const uint32_t g2 = guard_.load(std::memory_order_acquire);
      if (g1 == g2) return copy;
    }
  }

  /// @brief Single non-spinning attempt. On a conflict (writer mid-update or a
  ///        torn read) returns the last consistent value cached by a prior
  ///        successful try_load(); RT-safe (no spin, no alloc). The cache is
  ///        touched only on this read path, so no extra synchronization is
  ///        required as long as a single reader thread calls it.
  T try_load() const noexcept {
    const uint32_t g1 = guard_.load(std::memory_order_acquire);
    if ((g1 & 1u) == 0u) {  // not mid-update
      const T copy = load_words();
      std::atomic_thread_fence(std::memory_order_acquire);
      const uint32_t g2 = guard_.load(std::memory_order_acquire);
      if (g1 == g2) {
        cached_ = copy;
        return copy;
      }
    }
    return cached_;
  }

 private:
  static constexpr size_t kWordCount = (sizeof(T) + sizeof(uint32_t) - 1) / sizeof(uint32_t);
  using PackedWords = std::array<uint32_t, kWordCount>;

  void store_words(const T& value) noexcept {
    PackedWords packed{};
    // T is trivially copyable (asserted above); the void* cast keeps GCC's
    // -Wclass-memaccess from flagging the copy when T merely has a user-declared
    // default constructor or default member initializers (non-trivial, but still
    // trivially copyable).
    std::memcpy(packed.data(), static_cast<const void*>(&value), sizeof(T));
    for (size_t index = 0; index < kWordCount; ++index) {
      value_words_[index].store(packed[index], std::memory_order_relaxed);
    }
  }

  T load_words() const noexcept {
    PackedWords packed{};
    for (size_t index = 0; index < kWordCount; ++index) {
      packed[index] = value_words_[index].load(std::memory_order_relaxed);
    }
    T value{};
    std::memcpy(static_cast<void*>(&value), packed.data(), sizeof(T));
    return value;
  }

  std::array<std::atomic<uint32_t>, kWordCount> value_words_{};
  mutable std::atomic<uint32_t> guard_{0};
  // Last torn-free value observed by try_load(); the audio-thread fallback.
  mutable T cached_{};
};

}  // namespace sonare::rt
