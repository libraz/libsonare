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
///  - @ref SeqlockCell::load() spins until it observes a consistent (untorn)
///    snapshot. It keeps no state, so any number of threads may call it. Use it
///    where the reader can tolerate a bounded spin (e.g. a control/host thread
///    polling the meter).
///
///  - @ref SeqlockCell::Reader makes a SINGLE non-spinning attempt; on a
///    detected conflict it reports the last consistent value it cached instead
///    of spinning up to a scheduler tick (which would risk an xrun if the
///    writer were preempted mid-update). Use it on the audio thread.
///
/// The fallback cache is what makes the non-spinning path single-reader: it is
/// written on every successful read, so two threads sharing one cache would
/// race on it. That cache therefore lives in the Reader handle rather than in
/// the cell, and the handle is move-only — one Reader per reading thread, which
/// the cell hands out through @ref SeqlockCell::reader(). Two readers get two
/// caches instead of one shared one, so the constraint is carried by the type
/// rather than by a comment a caller has to find.
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
  explicit SeqlockCell(const T& initial) noexcept { store_words(initial); }

  // Pinned in place. The atomic words already made the cell non-copyable, but
  // saying so is what documents why an owner holding both a cell and a Reader
  // built from it cannot be moved: the handle stores the cell's address.
  SeqlockCell(const SeqlockCell&) = delete;
  SeqlockCell& operator=(const SeqlockCell&) = delete;

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

  /// @brief Non-spinning reader handle: owns the stale-value cache the audio
  ///        thread falls back on, so one handle is one reader.
  ///
  /// Move-only on purpose. Copying would duplicate the cache silently and let
  /// two threads believe they each own one; moving transfers it. Construct one
  /// per reading thread (control thread, before that thread starts reading) and
  /// keep it alive alongside the cell — a Reader does not extend the cell's
  /// lifetime.
  class Reader {
   public:
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    Reader(Reader&&) noexcept = default;
    Reader& operator=(Reader&&) noexcept = default;

    /// @brief Single non-spinning attempt. On a conflict (writer mid-update or
    ///        a torn read) returns the last consistent value this reader
    ///        cached; RT-safe (no spin, no alloc).
    /// @details Callers that must distinguish "nothing new" from "conflicted,
    ///          retry me later" — e.g. a reader driving change detection off a
    ///          separate version counter, which must not mark a torn read as
    ///          applied — should use @ref try_load_into instead: silently
    ///          substituting the cached value here is exactly what makes that
    ///          distinction impossible.
    T try_load() noexcept {
      T value{};
      if (try_load_into(&value)) {
        return value;
      }
      return cached_;
    }

    /// @brief Single non-spinning attempt that reports whether the read was
    ///        consistent, instead of silently substituting a stale value on
    ///        conflict. On success, writes the fresh value into `*out`, updates
    ///        this reader's cache, and returns true. On a conflict (writer
    ///        mid-update or a torn read) leaves `*out` untouched and returns
    ///        false. RT-safe (no spin, no alloc, no exception).
    bool try_load_into(T* out) noexcept {
      if (!cell_->try_copy(out)) return false;
      cached_ = *out;
      return true;
    }

   private:
    friend class SeqlockCell;
    /// Seeds the cache from the cell's current value, so a conflict on the very
    /// first read reports what was published rather than a zeroed T.
    explicit Reader(const SeqlockCell& cell) noexcept : cell_(&cell), cached_(cell.load()) {}

    const SeqlockCell* cell_;
    T cached_;
  };

  /// @brief Creates a reader handle. Control thread: @ref load() spins, and the
  ///        handle is meant to be built once and then used by one thread.
  Reader reader() const noexcept { return Reader(*this); }

 private:
  /// One non-spinning copy attempt with no fallback. Returns false and leaves
  /// `*out` untouched when the writer was mid-update or the copy tore.
  bool try_copy(T* out) const noexcept {
    const uint32_t g1 = guard_.load(std::memory_order_acquire);
    if ((g1 & 1u) != 0u) return false;  // writer mid-update
    const T copy = load_words();
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint32_t g2 = guard_.load(std::memory_order_acquire);
    if (g1 != g2) return false;  // torn read
    *out = copy;
    return true;
  }

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
};

}  // namespace sonare::rt
