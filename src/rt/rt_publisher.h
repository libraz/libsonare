#pragma once

/// @file rt_publisher.h
/// @brief Lock-free single-reader publisher for handing immutable snapshots to
///        a real-time audio thread without shared_ptr refcount churn, locks, or
///        heap allocation/free on the audio thread.
///
/// The atomic `shared_ptr` free functions take a global spinlock in both standard
/// libraries and may free on the calling thread, so they are forbidden in a
/// render callback. This replaces them with a wait-free hand-off: `publish()`
/// passes the snapshot through an SPSC ring, coalescing into a single pending
/// slot when the ring is full. Ownership is partitioned by ring position and
/// atomic slot state, so producer and consumer never touch one `shared_ptr`
/// concurrently.
///
/// `acquire()`, at block start, adopts the newest snapshot in one call however
/// long the burst: it swaps the snapshot it held into the slot it adopted from
/// and leaves superseded ones where they are. Every slot the audio thread hands
/// back is emptied by the control thread on its next `publish()`, so no alloc
/// and no free happen on the audio thread. `current()` is then a raw pointer
/// valid for the whole block, and a block with no pending publish does no
/// refcount work at all.
///
/// For state read from MORE than one thread, use `RtSnapshot<T>` instead:
/// readers take a lock-free pin and the control thread frees a superseded
/// snapshot only after every pin that could observe it is released.

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace sonare::rt {

/// Single-producer (control thread) / single-consumer (audio thread) publisher
/// of `std::shared_ptr<const T>` snapshots.
///
/// @tparam T Immutable snapshot type. Snapshots are shared as `const T`.
template <typename T>
class RtPublisher {
 public:
  /// Capacity of the hand-off ring (power of two). A longer burst between two
  /// acquire() calls coalesces into the pending slot, so it still ends on the
  /// newest snapshot.
  static constexpr size_t kCapacity = 64;
  static_assert((kCapacity & (kCapacity - 1)) == 0, "kCapacity must be a power of two");

  RtPublisher() = default;
  RtPublisher(const RtPublisher&) = delete;
  RtPublisher& operator=(const RtPublisher&) = delete;
  RtPublisher(RtPublisher&&) = delete;
  RtPublisher& operator=(RtPublisher&&) = delete;

  /// Publish a new snapshot from the CONTROL thread. May allocate/free (it
  /// releases the snapshots the audio thread handed back here). When the
  /// hand-off ring is full, the newest snapshot overwrites the coalesced
  /// pending slot instead of being dropped. Not real-time safe.
  bool publish(std::shared_ptr<const T> snapshot) {
    reclaim_retired();
    if (!snapshot) return false;
    // After an overflow pending_slot_ is the newest; coalesce there so the ring never outruns it.
    if (pending_state_.load(std::memory_order_acquire) != kPendingEmpty) {
      publish_pending(std::move(snapshot));
      return true;
    }
    // Retain a control-thread copy before handing ownership to the ring. The
    // ring keeps no copy of its own, so the audio thread's swap never performs
    // a shared_ptr refcount operation (and thus never frees) there.
    std::shared_ptr<const T> retained = snapshot;
    if (!publish_ring_.push(std::move(snapshot))) {
      publish_pending(std::move(retained));
      return true;
    }
    control_current_ = std::move(retained);
    return true;
  }

  /// Adopt the newest published snapshot on the AUDIO thread. Wait-free: one
  /// call reaches the newest snapshot published before it, whatever the burst
  /// size, parking the previously held snapshot in the vacated hand-off slot and
  /// leaving superseded ones in place for the control thread to free (no alloc,
  /// no free here). Call once at block start.
  ///
  /// Single-consumer contract: this is the consumer side of the SPSC publish
  /// ring, so at most one thread may be inside acquire() at any instant. That is
  /// normally the audio thread; a control thread may also call it to read back
  /// its own just-published snapshot, but ONLY while the audio thread is
  /// quiescent (never concurrently). A concurrent call races the ring's tail and
  /// the slot swap — a debug-only flag below catches that violation.
  void acquire() noexcept {
#ifndef NDEBUG
    const bool reentered = in_acquire_.exchange(true, std::memory_order_acq_rel);
    assert(!reentered &&
           "RtPublisher::acquire() is single-consumer; a concurrent acquire() was detected");
#endif
    commit_adoption(begin_adoption());
#ifndef NDEBUG
    in_acquire_.store(false, std::memory_order_release);
#endif
  }

  /// Adopt the newest snapshot accepted by the CONTROL thread while the AUDIO
  /// consumer is quiescent, then free what the adoption displaced.
  ///
  /// This method is CONTROL-only and must not run concurrently with the audio
  /// thread or another publisher. It is intended for serialized control
  /// operations, such as replacing an instrument between audio blocks.
  void acquire_control_quiescent() noexcept {
    acquire();
    reclaim_retired();
  }

  /// Adopt the newest snapshot on the AUDIO thread, like acquire() above, but
  /// additionally reports the swap: @p on_adopt is invoked as
  /// `on_adopt(const T* previous, const T* next)` exactly once, and only when
  /// this call actually adopted a newer snapshot (a no-op call never invokes
  /// it). Covers both the hand-off ring and the coalesced pending slot, so a
  /// burst of publishes that overflows the ring is still observed.
  ///
  /// The snapshot held before this call (`previous`) stays alive through
  /// @p on_adopt: it is parked in the vacated slot only after the callback
  /// returns, and the slot is handed back to the control thread (which frees
  /// it) only after that. A control thread coalescing into the pending slot
  /// meanwhile yields until the callback returns, so keep @p on_adopt short.
  ///
  /// Same single-consumer contract as acquire() above.
  template <class F>
  void acquire(F&& on_adopt) noexcept {
#ifndef NDEBUG
    const bool reentered = in_acquire_.exchange(true, std::memory_order_acq_rel);
    assert(!reentered &&
           "RtPublisher::acquire() is single-consumer; a concurrent acquire() was detected");
#endif
    std::shared_ptr<const T> previous = std::move(audio_current_);
    const Adoption adoption = begin_adoption();
    if (adoption.vacated != nullptr) {
      on_adopt(previous.get(), audio_current_.get());
      // The vacated slot holds the empty pointer swapped out of audio_current_.
      *adoption.vacated = std::move(previous);
    } else {
      audio_current_ = std::move(previous);
    }
    commit_adoption(adoption);
#ifndef NDEBUG
    in_acquire_.store(false, std::memory_order_release);
#endif
  }

  /// Raw pointer to the snapshot currently held by the audio thread, or nullptr
  /// if nothing has been adopted yet. Valid for the whole block; the audio
  /// thread only changes it inside acquire(). RT-safe.
  const T* current() const noexcept { return audio_current_.get(); }

  /// Most recently published snapshot as seen by the control thread.
  const std::shared_ptr<const T>& control_current() const noexcept { return control_current_; }

  /// Release every retained snapshot from the CONTROL thread after the audio
  /// consumer has been stopped. This is intended for unload/teardown paths:
  /// unlike publish(), it also drops the audio-owned current snapshot, so it
  /// must never run concurrently with acquire()/current() on the audio thread.
  /// Not real-time safe.
  void reclaim() noexcept {
    publish_ring_.clear_all();
    // The audio thread is quiescent by contract, so no pending-slot reader can
    // race this control-thread cleanup.
    pending_slot_.reset();
    pending_state_.store(kPendingEmpty, std::memory_order_release);
    control_current_.reset();
    audio_current_.reset();
  }

 private:
  // SPSC ring of shared_ptr<const T>: push() runs on the control thread, the
  // consumer_* calls and slot() on the audio thread. Positions in [tail, head)
  // belong to the consumer and every other position to the producer, so a slot
  // the consumer hands back (by advancing tail_) is emptied by the producer.
  class SpscPtrRing {
   public:
    bool push(std::shared_ptr<const T>&& item) noexcept {
      const size_t head = head_.load(std::memory_order_relaxed);
      const size_t tail = tail_.load(std::memory_order_acquire);
      if (head - tail >= kCapacity) {
        return false;
      }
      slots_[head & kMask] = std::move(item);
      head_.store(head + 1, std::memory_order_release);
      return true;
    }

    size_t consumer_tail() const noexcept { return tail_.load(std::memory_order_relaxed); }
    size_t consumer_head() const noexcept { return head_.load(std::memory_order_acquire); }
    std::shared_ptr<const T>& slot(size_t position) noexcept { return slots_[position & kMask]; }
    // Consumer: hands every position before @p end back to the producer.
    void release_until(size_t end) noexcept { tail_.store(end, std::memory_order_release); }

    // Producer: frees whatever the consumer left in the slots it handed back.
    void clear_free_slots() noexcept {
      const size_t head = head_.load(std::memory_order_relaxed);
      const size_t free_end = tail_.load(std::memory_order_acquire) + kCapacity;
      for (size_t position = head; position != free_end; ++position) {
        slots_[position & kMask].reset();
      }
    }

    // Producer, with the consumer quiescent: drops every entry, read or not.
    void clear_all() noexcept {
      tail_.store(head_.load(std::memory_order_relaxed), std::memory_order_release);
      for (auto& slot : slots_) {
        slot.reset();
      }
    }

   private:
    static constexpr size_t kMask = kCapacity - 1;
    std::array<std::shared_ptr<const T>, kCapacity> slots_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
  };

  // An adoption between begin_adoption() and commit_adoption(): the slot now
  // holding the displaced snapshot (null when nothing newer was available) and
  // the ring position read up to.
  struct Adoption {
    std::shared_ptr<const T>* vacated = nullptr;
    size_t ring_end = 0;
    bool ring_read = false;
    bool from_pending = false;
  };

  // Consumer side. Swaps the newest unadopted snapshot into audio_current_; the
  // vacated slot stays consumer-owned until commit_adoption().
  Adoption begin_adoption() noexcept {
    Adoption adoption;
    uint8_t expected = kPendingReady;
    adoption.from_pending = pending_state_.compare_exchange_strong(
        expected, kPendingReading, std::memory_order_acq_rel, std::memory_order_acquire);
    // Read after the claim, so every entry pushed before the pending slot filled is visible.
    const size_t tail = publish_ring_.consumer_tail();
    adoption.ring_end = publish_ring_.consumer_head();
    adoption.ring_read = adoption.ring_end != tail;
    if (adoption.from_pending) {
      // While the slot is non-empty publish() never pushes to the ring, so every
      // unread ring entry is older and is skipped.
      adoption.vacated = &pending_slot_;
    } else if (adoption.ring_read) {
      adoption.vacated = &publish_ring_.slot(adoption.ring_end - 1);
    } else {
      return adoption;
    }
    audio_current_.swap(*adoption.vacated);
    return adoption;
  }

  // Consumer side. Hands the vacated slot and every skipped entry back to the
  // control thread, which frees them in reclaim_retired().
  void commit_adoption(const Adoption& adoption) noexcept {
    if (adoption.ring_read) {
      publish_ring_.release_until(adoption.ring_end);
    }
    if (adoption.from_pending) {
      pending_state_.store(kPendingEmpty, std::memory_order_release);
    }
  }

  // Control side. Frees the snapshots the audio thread handed back, so no free
  // ever runs on the audio thread. The audio thread touches pending_slot_ only
  // after claiming it from kPendingReady, which only this thread sets.
  void reclaim_retired() noexcept {
    publish_ring_.clear_free_slots();
    if (pending_state_.load(std::memory_order_acquire) == kPendingEmpty) {
      pending_slot_.reset();
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

  // Hand-off: control -> audio. Its vacated slots carry retirements back.
  SpscPtrRing publish_ring_;

  static constexpr uint8_t kPendingEmpty = 0;
  static constexpr uint8_t kPendingWriting = 1;
  static constexpr uint8_t kPendingReady = 2;
  static constexpr uint8_t kPendingReading = 3;
  alignas(64) std::atomic<uint8_t> pending_state_{kPendingEmpty};
  std::shared_ptr<const T> pending_slot_;

  std::shared_ptr<const T> control_current_;  // control thread
  std::shared_ptr<const T> audio_current_;    // audio thread

#ifndef NDEBUG
  // Debug-only single-consumer guard for acquire() (see its contract above).
  std::atomic<bool> in_acquire_{false};
#endif
};

/// Multi-reader, lock-free snapshot holder for immutable state that is read
/// concurrently from more than one thread (e.g. the audio thread AND a control
/// thread) but only published from the control thread.
///
/// A reader takes a `Reader` pin from read(): one epoch load, one counter
/// increment and one pointer load, with no lock, allocation, shared_ptr refcount
/// or free on the reader. The pin keeps the snapshot it observed alive until the
/// pin is destroyed, however many publishes follow. The control thread retires
/// each superseded snapshot tagged with the current epoch and frees it only
/// after two epoch advances, each taken when the counter of the reader phase it
/// retires has drained to zero; a stalled reader therefore defers reclamation
/// rather than invalidating its pointer.
///
/// This replaces `std::atomic_load/store(std::shared_ptr*)`, whose libstdc++/
/// libc++ implementation takes a global spinlock and may free on the caller.
///
/// @tparam T Immutable snapshot type.
template <typename T>
class RtSnapshot {
 public:
  /// Scoped pin on the snapshot current when read() was called. Not copyable or
  /// movable; bind it to a local (`const auto snapshot = holder.read();`).
  class Reader {
   public:
    ~Reader() { counter_->fetch_sub(1, std::memory_order_release); }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    Reader(Reader&&) = delete;
    Reader& operator=(Reader&&) = delete;

    /// Pinned snapshot, or nullptr if none had been published.
    const T* get() const noexcept { return snapshot_; }
    const T& operator*() const noexcept { return *snapshot_; }
    const T* operator->() const noexcept { return snapshot_; }
    explicit operator bool() const noexcept { return snapshot_ != nullptr; }

   private:
    friend class RtSnapshot;
    Reader(std::atomic<uint32_t>* counter, const T* snapshot) noexcept
        : counter_(counter), snapshot_(snapshot) {}

    std::atomic<uint32_t>* counter_;
    const T* snapshot_;
  };

  RtSnapshot() = default;
  RtSnapshot(const RtSnapshot&) = delete;
  RtSnapshot& operator=(const RtSnapshot&) = delete;
  RtSnapshot(RtSnapshot&&) = delete;
  RtSnapshot& operator=(RtSnapshot&&) = delete;

  /// Publish a new snapshot from the CONTROL thread. May allocate/free. Never
  /// blocks: the previous snapshot is retired and freed (on the control thread,
  /// here or in a later publish/try_reclaim) once no pin can still observe it.
  void publish(std::shared_ptr<const T> snapshot) {
    std::shared_ptr<const T> previous = std::move(owner_);
    owner_ = std::move(snapshot);
    current_.store(owner_.get(), std::memory_order_seq_cst);
    if (previous) {
      retired_.push_back(Retired{epoch_.load(std::memory_order_relaxed), std::move(previous)});
    }
    try_reclaim();
  }

  /// Pin and return the current snapshot. Safe to call from any reader thread,
  /// including while already holding a pin. RT-safe: wait-free on targets with
  /// native atomic add, never allocates or frees.
  Reader read() const noexcept {
    std::atomic<uint32_t>* counter = &readers_[epoch_.load(std::memory_order_seq_cst) & 1u];
    counter->fetch_add(1, std::memory_order_seq_cst);
    return Reader(counter, current_.load(std::memory_order_seq_cst));
  }

  /// CONTROL thread: free every retired snapshot no pin can still observe, never
  /// blocking. Returns true when nothing superseded remains alive.
  bool try_reclaim() {
    for (int step = 0; step < 2 && !retired_.empty(); ++step) {
      free_expired();
      if (retired_.empty()) break;
      const uint32_t epoch = epoch_.load(std::memory_order_relaxed);
      // Advancing to epoch + 1 retires the phase of epoch - 1; its pins must have drained.
      if (readers_[(epoch + 1u) & 1u].load(std::memory_order_seq_cst) != 0) break;
      epoch_.store(epoch + 1u, std::memory_order_seq_cst);
    }
    free_expired();
    return retired_.empty();
  }

  /// CONTROL thread: block until every pin taken before this call that could
  /// observe a superseded snapshot is released, then free those snapshots. Must
  /// not be called while the calling thread holds a pin on this holder.
  void synchronize() {
    while (!try_reclaim()) {
      std::this_thread::yield();
    }
  }

 private:
  struct Retired {
    uint32_t epoch;
    std::shared_ptr<const T> snapshot;
  };

  // Retired entries are in epoch order; one is unobservable once the epoch has
  // advanced twice past its tag.
  void free_expired() {
    const uint32_t epoch = epoch_.load(std::memory_order_relaxed);
    auto it = retired_.begin();
    while (it != retired_.end() && static_cast<uint32_t>(epoch - it->epoch) >= 2u) {
      ++it;
    }
    retired_.erase(retired_.begin(), it);
  }

  std::atomic<const T*> current_{nullptr};
  std::atomic<uint32_t> epoch_{0};
  mutable std::array<std::atomic<uint32_t>, 2> readers_{};
  std::shared_ptr<const T> owner_;  // control thread only
  std::vector<Retired> retired_;    // control thread only
};

}  // namespace sonare::rt
