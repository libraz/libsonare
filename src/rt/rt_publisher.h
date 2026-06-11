#pragma once

/// @file rt_publisher.h
/// @brief Lock-free single-reader publisher for handing immutable snapshots to
///        a real-time audio thread without shared_ptr refcount churn, locks, or
///        heap allocation/free on the audio thread.
///
/// Motivation: the free-function `std::atomic_load/store(std::shared_ptr*)`
/// overloads use a global spinlock in libstdc++/libc++ and may free memory on
/// the calling thread. Calling them on the audio thread is therefore forbidden
/// (a lock + potential free in the render callback). `RtPublisher<T>` replaces
/// that pattern with a wait-free hand-off:
///
///  - CONTROL thread calls `publish(snapshot)`:
///      (a) drains the retire ring, freeing previously-retired shared_ptrs on
///          the control thread (never the audio thread);
///      (b) hands the new snapshot to the audio thread through an SPSC ring of
///          `shared_ptr<const T>`. If the ring is full, the latest snapshot is
///          coalesced into a single pending slot that the audio thread adopts
///          after draining the ring. Ownership is partitioned by atomic slot
///          state, so the producer and consumer never touch the same
///          `shared_ptr` object concurrently.
///
///  - AUDIO thread calls `acquire()` at block start. It drains the publish ring
///      to the newest pending snapshot; the OLD owning snapshot and every
///      superseded one is moved into the retire ring (wait-free push, no alloc,
///      no free here) and the newest is adopted. `current()` then returns a raw
///      `const T*` valid for the whole block because the audio thread holds one
///      owning copy as a member that only changes inside `acquire()`.
///
/// The audio-thread read path performs NO heap allocation, NO lock, and NO
/// shared_ptr refcount operation per block when no publish is pending (the
/// owning member is only touched when `acquire()` actually adopts a value).
///
/// For state that must be read concurrently from MORE than one thread (e.g.
/// the audio thread and a control thread both calling `ppq_to_sample`), use
/// `RtSnapshot<T>` instead: readers do a single lock-free pointer load and the
/// control thread owns all snapshot lifetimes via a bounded retention ring.

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <utility>

namespace sonare::rt {

/// Single-producer (control thread) / single-consumer (audio thread) publisher
/// of `std::shared_ptr<const T>` snapshots.
///
/// @tparam T Immutable snapshot type. Snapshots are shared as `const T`.
template <typename T>
class RtPublisher {
 public:
  /// Capacity of each internal ring (power of two). Sized generously so the
  /// control thread can publish a long burst before the audio thread adopts,
  /// and so the audio thread can retire a long burst before the control thread
  /// reclaims, without overrun.
  static constexpr size_t kCapacity = 64;
  static_assert((kCapacity & (kCapacity - 1)) == 0, "kCapacity must be a power of two");

  RtPublisher() = default;
  RtPublisher(const RtPublisher&) = delete;
  RtPublisher& operator=(const RtPublisher&) = delete;
  RtPublisher(RtPublisher&&) = delete;
  RtPublisher& operator=(RtPublisher&&) = delete;

  /// Publish a new snapshot from the CONTROL thread. May allocate/free (it
  /// drains and releases retired snapshots here). When the hand-off ring is
  /// full, the newest snapshot overwrites the coalesced pending slot instead
  /// of being dropped. Not real-time safe.
  bool publish(std::shared_ptr<const T> snapshot) {
    reclaim_retired();
    if (!snapshot) return false;
    // Retain a control-thread copy before handing ownership to the ring. The
    // ring keeps no copy of its own, so the retire ring's audio-thread producer
    // never performs a shared_ptr refcount operation (and thus never frees) on
    // the audio thread.
    std::shared_ptr<const T> retained = snapshot;
    if (!publish_ring_.push(std::move(snapshot))) {
      publish_pending(std::move(retained));
      return true;
    }
    control_current_ = std::move(retained);
    return true;
  }

  /// Adopt the latest pending snapshot on the AUDIO thread. Wait-free: drains
  /// the publish ring to the newest pending value, retiring the previously held
  /// snapshot and any superseded ones (wait-free push, no alloc, no free here).
  /// Call once at block start.
  void acquire() noexcept {
    std::shared_ptr<const T> popped;
    while (!audio_current_ || retire_ring_.can_push()) {
      if (!publish_ring_.pop(popped)) {
        break;
      }
      if (audio_current_) {
        retire_ring_.push(std::move(audio_current_));
      }
      audio_current_ = std::move(popped);
    }
    acquire_pending();
  }

  /// Raw pointer to the snapshot currently held by the audio thread, or nullptr
  /// if nothing has been adopted yet. Valid for the whole block; the audio
  /// thread only changes it inside acquire(). RT-safe.
  const T* current() const noexcept { return audio_current_.get(); }

  /// Owning snapshot the audio thread currently holds. RT-safe to read; do not
  /// copy on the audio thread.
  const std::shared_ptr<const T>& current_shared() const noexcept { return audio_current_; }

  /// Most recently published snapshot as seen by the control thread.
  const std::shared_ptr<const T>& control_current() const noexcept { return control_current_; }

 private:
  // SPSC ring specialised for shared_ptr<const T>. push() runs on the producer
  // thread; pop() runs on the consumer thread. For the publish ring the
  // producer is the control thread and the consumer is the audio thread; for
  // the retire ring the roles are reversed. The ring keeps no extra copy of any
  // element, so a push never triggers a shared_ptr free on its producer thread.
  class SpscPtrRing {
   public:
    bool push(std::shared_ptr<const T>&& item) noexcept {
      const size_t head = head_.load(std::memory_order_relaxed);
      const size_t tail = tail_.load(std::memory_order_acquire);
      if (head - tail >= kCapacity) {
        return false;
      }
      const size_t index = head & (kCapacity - 1);
      slots_[index] = std::move(item);
      head_.store(head + 1, std::memory_order_release);
      return true;
    }

    bool pop(std::shared_ptr<const T>& out) noexcept {
      const size_t tail = tail_.load(std::memory_order_relaxed);
      const size_t head = head_.load(std::memory_order_acquire);
      if (head == tail) {
        return false;
      }
      const size_t index = tail & (kCapacity - 1);
      out = std::move(slots_[index]);
      tail_.store(tail + 1, std::memory_order_release);
      return true;
    }

    bool can_push() const noexcept {
      const size_t head = head_.load(std::memory_order_relaxed);
      const size_t tail = tail_.load(std::memory_order_acquire);
      return head - tail < kCapacity;
    }

   private:
    std::array<std::shared_ptr<const T>, kCapacity> slots_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
  };

  void reclaim_retired() noexcept {
    // The retire ring's producer is the audio thread; the consumer (this) is
    // the control thread, so freeing the popped shared_ptrs here happens off
    // the audio thread.
    std::shared_ptr<const T> dead;
    while (retire_ring_.pop(dead)) {
      dead.reset();
    }
  }

  void publish_pending(std::shared_ptr<const T> snapshot) {
    for (;;) {
      uint8_t state = pending_state_.load(std::memory_order_acquire);
      if (state == kPendingReading) {
        std::this_thread::yield();
        continue;
      }
      if (pending_state_.compare_exchange_weak(state, kPendingWriting, std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
        pending_slot_ = snapshot;
        control_current_ = std::move(snapshot);
        pending_state_.store(kPendingReady, std::memory_order_release);
        return;
      }
    }
  }

  void acquire_pending() noexcept {
    if (audio_current_ && !retire_ring_.can_push()) {
      return;
    }
    uint8_t expected = kPendingReady;
    if (!pending_state_.compare_exchange_strong(
            expected, kPendingReading, std::memory_order_acq_rel, std::memory_order_acquire)) {
      return;
    }
    if (pending_slot_) {
      if (audio_current_) {
        retire_ring_.push(std::move(audio_current_));
      }
      audio_current_ = std::move(pending_slot_);
    }
    pending_state_.store(kPendingEmpty, std::memory_order_release);
  }

  // Hand-off: control -> audio.
  SpscPtrRing publish_ring_;
  // Retirement: audio -> control.
  SpscPtrRing retire_ring_;

  static constexpr uint8_t kPendingEmpty = 0;
  static constexpr uint8_t kPendingWriting = 1;
  static constexpr uint8_t kPendingReady = 2;
  static constexpr uint8_t kPendingReading = 3;
  alignas(64) std::atomic<uint8_t> pending_state_{kPendingEmpty};
  std::shared_ptr<const T> pending_slot_;

  std::shared_ptr<const T> control_current_;  // control thread
  std::shared_ptr<const T> audio_current_;    // audio thread
};

/// Multi-reader, lock-free snapshot holder for immutable state that is read
/// concurrently from more than one thread (e.g. the audio thread AND a control
/// thread) but only published from the control thread.
///
/// Unlike RtPublisher (strictly single-reader), readers here just perform one
/// acquire-load of a raw `const T*`: no lock, no allocation, no shared_ptr
/// refcount, and no free on the reader. The control thread owns all snapshot
/// lifetimes through a bounded retention ring of shared_ptrs; an old snapshot
/// is freed (on the control thread) only after `kRetain` further publishes,
/// which keeps any pointer a reader loaded valid for the duration of its use.
///
/// This replaces `std::atomic_load/store(std::shared_ptr*)`, whose libstdc++/
/// libc++ implementation takes a global spinlock and may free on the caller.
///
/// @tparam T Immutable snapshot type.
template <typename T>
class RtSnapshot {
 public:
  /// Number of past generations kept alive after being superseded. A reader
  /// that loaded a pointer must finish using it before this many subsequent
  /// publishes occur; sized generously so that holds for any realistic block.
  static constexpr size_t kRetain = 64;

  RtSnapshot() = default;
  RtSnapshot(const RtSnapshot&) = delete;
  RtSnapshot& operator=(const RtSnapshot&) = delete;
  RtSnapshot(RtSnapshot&&) = delete;
  RtSnapshot& operator=(RtSnapshot&&) = delete;

  /// Publish a new snapshot from the CONTROL thread. May allocate/free. The
  /// previous snapshot stays alive in the retention ring and is reclaimed only
  /// after kRetain further publishes (always on the control thread).
  void publish(std::shared_ptr<const T> snapshot) {
    // Park the owning shared_ptr; reclaim the slot kRetain generations back
    // (its reader window has long since closed) by overwriting it here.
    const size_t index = static_cast<size_t>(write_index_ & (kRetain - 1));
    retain_[index] = std::move(snapshot);
    write_index_++;
    current_.store(retain_[index].get(), std::memory_order_release);
  }

  /// Lock-free read of the current snapshot pointer, or nullptr if none has
  /// been published. Safe to call from any reader thread. RT-safe.
  const T* load() const noexcept { return current_.load(std::memory_order_acquire); }

 private:
  static_assert((kRetain & (kRetain - 1)) == 0, "kRetain must be a power of two");

  std::atomic<const T*> current_{nullptr};
  std::array<std::shared_ptr<const T>, kRetain> retain_{};  // control thread only
  size_t write_index_ = 0;                                  // control thread only
};

}  // namespace sonare::rt
