#include "rt/rt_publisher.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <thread>

namespace {

// Snapshot payload with a global destruction counter so tests can observe
// when retired snapshots are actually freed (always on the control thread).
struct CountedSnapshot {
  explicit CountedSnapshot(uint64_t v) : value(v) { ++live; }
  CountedSnapshot(const CountedSnapshot&) = delete;
  CountedSnapshot& operator=(const CountedSnapshot&) = delete;
  ~CountedSnapshot() { ++destroyed; }

  uint64_t value = 0;
  // The snapshot embeds its own value redundantly so a concurrent reader can
  // detect a torn read (the two copies must always agree).
  uint64_t mirror = value;

  static std::atomic<int> live;
  static std::atomic<int> destroyed;
};

std::atomic<int> CountedSnapshot::live{0};
std::atomic<int> CountedSnapshot::destroyed{0};

std::shared_ptr<const CountedSnapshot> make_snapshot(uint64_t value) {
  auto snapshot = std::make_shared<CountedSnapshot>(value);
  snapshot->mirror = value;
  return snapshot;
}

// Snapshot payload whose destructor records whether it ran on a designated
// "consumer" thread, for the RtPublisher callback-acquire double-defer
// regression test below.
struct ThreadTaggedSnapshot {
  static std::atomic<std::thread::id> consumer_thread_id;
  static std::atomic<bool> destroyed_on_consumer_thread;

  int value = 0;
  ~ThreadTaggedSnapshot() {
    if (std::this_thread::get_id() == consumer_thread_id.load(std::memory_order_relaxed)) {
      destroyed_on_consumer_thread.store(true, std::memory_order_relaxed);
    }
  }
};

std::atomic<std::thread::id> ThreadTaggedSnapshot::consumer_thread_id{};
std::atomic<bool> ThreadTaggedSnapshot::destroyed_on_consumer_thread{false};

}  // namespace

TEST_CASE("RtPublisher publish then acquire exposes the published value", "[rt][publisher]") {
  sonare::rt::RtPublisher<int> publisher;

  REQUIRE(publisher.current() == nullptr);

  auto snapshot = std::make_shared<const int>(42);
  REQUIRE(publisher.publish(snapshot));
  // The audio thread has not acquired yet, so current() is still empty.
  REQUIRE(publisher.current() == nullptr);

  publisher.acquire();
  REQUIRE(publisher.current() != nullptr);
  REQUIRE(*publisher.current() == 42);
}

TEST_CASE("RtPublisher tolerates repeated sequential acquire calls", "[rt][publisher]") {
  // The single-consumer debug guard must reset on each return, so a consumer
  // calling acquire() back-to-back (e.g. the audio thread every block, or a
  // control thread reading back between publishes) never trips it.
  sonare::rt::RtPublisher<int> publisher;
  REQUIRE(publisher.publish(std::make_shared<const int>(7)));
  for (int i = 0; i < 1000; ++i) {
    publisher.acquire();
  }
  REQUIRE(publisher.current() != nullptr);
  REQUIRE(*publisher.current() == 7);
}

TEST_CASE("RtPublisher reclaim releases the final snapshot after audio quiescence",
          "[rt][publisher]") {
  CountedSnapshot::destroyed.store(0);
  sonare::rt::RtPublisher<CountedSnapshot> publisher;
  REQUIRE(publisher.publish(make_snapshot(7)));
  publisher.acquire();
  REQUIRE(publisher.current() != nullptr);

  publisher.reclaim();

  REQUIRE(publisher.current() == nullptr);
  REQUIRE(publisher.control_current() == nullptr);
  REQUIRE(CountedSnapshot::destroyed.load() == 1);
}

TEST_CASE("RtPublisher acquire adopts the newest of several pending publishes", "[rt][publisher]") {
  CountedSnapshot::destroyed.store(0);
  sonare::rt::RtPublisher<CountedSnapshot> publisher;

  REQUIRE(publisher.publish(make_snapshot(1)));
  REQUIRE(publisher.publish(make_snapshot(2)));
  REQUIRE(publisher.publish(make_snapshot(3)));

  // A single acquire drains the publish ring to the newest snapshot.
  publisher.acquire();
  REQUIRE(publisher.current() != nullptr);
  REQUIRE(publisher.current()->value == 3);

  // The two superseded snapshots were moved into the retire ring. They are
  // freed on the control thread when the next publish reclaims retired slots.
  REQUIRE(publisher.publish(make_snapshot(4)));
  REQUIRE(CountedSnapshot::destroyed.load() == 2);
}

TEST_CASE("RtPublisher coalesces a full publish ring to the newest snapshot", "[rt][publisher]") {
  CountedSnapshot::destroyed.store(0);
  sonare::rt::RtPublisher<CountedSnapshot> publisher;

  for (size_t i = 1; i <= sonare::rt::RtPublisher<CountedSnapshot>::kCapacity + 10; ++i) {
    REQUIRE(publisher.publish(make_snapshot(static_cast<uint64_t>(i))));
  }

  publisher.acquire();
  REQUIRE(publisher.current() != nullptr);
  REQUIRE(publisher.current()->value == sonare::rt::RtPublisher<CountedSnapshot>::kCapacity + 10);
}

TEST_CASE("RtPublisher concurrent publish/acquire is torn-read and leak free", "[rt][publisher]") {
  constexpr int kIterations = 100000;
  CountedSnapshot::live.store(0);
  CountedSnapshot::destroyed.store(0);

  sonare::rt::RtPublisher<CountedSnapshot> publisher;
  std::atomic<bool> producer_done{false};
  std::atomic<bool> torn{false};

  // Control thread: publish a stream of distinct snapshots.
  std::thread producer([&] {
    for (int i = 1; i <= kIterations; ++i) {
      while (!publisher.publish(make_snapshot(static_cast<uint64_t>(i)))) {
        std::this_thread::yield();
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  // Audio thread: continuously acquire and read the current snapshot, checking
  // that the value and its mirror copy always agree (no torn read).
  std::thread consumer([&] {
    while (true) {
      publisher.acquire();
      const CountedSnapshot* snapshot = publisher.current();
      if (snapshot && snapshot->value != snapshot->mirror) {
        torn.store(true, std::memory_order_relaxed);
      }
      if (producer_done.load(std::memory_order_acquire)) {
        // One final drain so the last published snapshot is adopted.
        publisher.acquire();
        break;
      }
    }
  });

  producer.join();
  consumer.join();

  REQUIRE_FALSE(torn.load(std::memory_order_relaxed));

  // After the consumer stopped, retired snapshots remain parked in the retire
  // ring until the control thread reclaims them on the next publish. Publishing
  // once more drains everything except the snapshot the audio thread still
  // owns (current_) and the control thread's own control_current_.
  REQUIRE(publisher.publish(make_snapshot(0)));
  // The audio thread never acquires the sentinel above, so current() still
  // holds the final real snapshot. Live snapshots: audio current_ +
  // control_current_ (the sentinel) + the sentinel may alias. Just assert that
  // the overwhelming majority were freed and nothing is permanently leaked
  // beyond a tiny bounded set.
  const int live = CountedSnapshot::live.load() - CountedSnapshot::destroyed.load();
  REQUIRE(live <= 4);
}

TEST_CASE("RtPublisher callback acquire reports previous and next on adoption", "[rt][publisher]") {
  sonare::rt::RtPublisher<int> publisher;
  int call_count = 0;
  const int* seen_previous = reinterpret_cast<const int*>(1);  // sentinel, overwritten below
  int seen_next = -1;

  // Nothing published yet: the callback must not fire.
  publisher.acquire([&](const int* previous, const int* next) {
    ++call_count;
    (void)previous;
    (void)next;
  });
  REQUIRE(call_count == 0);

  REQUIRE(publisher.publish(std::make_shared<const int>(42)));
  publisher.acquire([&](const int* previous, const int* next) {
    ++call_count;
    seen_previous = previous;
    seen_next = next ? *next : -1;
  });
  REQUIRE(call_count == 1);
  REQUIRE(seen_previous == nullptr);  // no prior snapshot had ever been adopted.
  REQUIRE(seen_next == 42);
  REQUIRE(publisher.current() != nullptr);
  REQUIRE(*publisher.current() == 42);

  // A second acquire with nothing new published must not call back again.
  publisher.acquire([&](const int*, const int*) { ++call_count; });
  REQUIRE(call_count == 1);

  REQUIRE(publisher.publish(std::make_shared<const int>(7)));
  const int* previous_ptr = publisher.current();
  publisher.acquire([&](const int* previous, const int* next) {
    ++call_count;
    seen_previous = previous;
    seen_next = next ? *next : -1;
  });
  REQUIRE(call_count == 2);
  REQUIRE(seen_previous == previous_ptr);
  REQUIRE(seen_next == 7);
}

TEST_CASE("RtPublisher callback acquire keeps the original snapshot valid through the callback",
          "[rt][publisher]") {
  CountedSnapshot::destroyed.store(0);
  sonare::rt::RtPublisher<CountedSnapshot> publisher;
  REQUIRE(publisher.publish(make_snapshot(1)));
  publisher.acquire([](const CountedSnapshot*, const CountedSnapshot*) {});

  REQUIRE(publisher.publish(make_snapshot(2)));
  uint64_t observed_previous_value = 0;
  bool destroyed_before_callback_ran = false;
  publisher.acquire([&](const CountedSnapshot* previous, const CountedSnapshot* next) {
    REQUIRE(previous != nullptr);
    observed_previous_value = previous->value;
    // The callback runs before the previous snapshot is retired, so it must
    // not have been freed yet.
    destroyed_before_callback_ran = CountedSnapshot::destroyed.load() > 0;
    REQUIRE(next != nullptr);
    REQUIRE(next->value == 2);
  });
  REQUIRE(observed_previous_value == 1);
  REQUIRE_FALSE(destroyed_before_callback_ran);
  // The previous snapshot is retired (not freed) right after the callback
  // returns; it is actually destroyed once the control thread reclaims it.
  REQUIRE(publisher.publish(make_snapshot(3)));
  REQUIRE(CountedSnapshot::destroyed.load() == 1);
}

TEST_CASE(
    "RtPublisher callback acquire is called once for a burst overflowing into "
    "the coalesced pending slot",
    "[rt][publisher]") {
  CountedSnapshot::destroyed.store(0);
  sonare::rt::RtPublisher<CountedSnapshot> publisher;

  // Establish an initial "previous" snapshot before the burst, exercising the
  // held-back-original path together with the coalesced path in one call.
  REQUIRE(publisher.publish(make_snapshot(0)));
  publisher.acquire([](const CountedSnapshot*, const CountedSnapshot*) {});

  // Publish more than the ring capacity in one burst: the ring fills, then
  // every further publish coalesces into the single pending slot.
  constexpr size_t kBurst = sonare::rt::RtPublisher<CountedSnapshot>::kCapacity + 1;
  for (size_t i = 1; i <= kBurst; ++i) {
    REQUIRE(publisher.publish(make_snapshot(static_cast<uint64_t>(i))));
  }

  int call_count = 0;
  uint64_t adopted_value = 0;
  publisher.acquire([&](const CountedSnapshot* previous, const CountedSnapshot* next) {
    ++call_count;
    REQUIRE(previous != nullptr);
    REQUIRE(previous->value == 0);
    REQUIRE(next != nullptr);
    adopted_value = next->value;
  });
  // The release is detected exactly once for the whole burst, never zero
  // times and never more than once -- the failure mode this case guards
  // against is losing the coalesced adoption entirely, or double-reporting
  // it split across the drain loop and the pending slot. Both adoption paths
  // ran in this one call, so the burst's true final value is what adopts.
  REQUIRE(call_count == 1);
  REQUIRE(adopted_value == kBurst);
  REQUIRE(publisher.current() != nullptr);
  REQUIRE(publisher.current()->value == kBurst);

  // Nothing was ever freed on this (the calling / "audio") thread: every
  // superseded snapshot is either still held live, parked in the retire ring,
  // or -- for the one snapshot the retire ring had no room left for -- parked
  // in the publisher's own deferred-retire slot. A further publish()/acquire()
  // round trip (letting the control thread reclaim, then draining once more)
  // must still behave normally.
  REQUIRE(publisher.publish(make_snapshot(kBurst + 1)));
  publisher.acquire([](const CountedSnapshot*, const CountedSnapshot*) {});
  REQUIRE(publisher.current()->value == kBurst + 1);
}

TEST_CASE(
    "RtPublisher callback acquire adopts nothing while a deferred retire is "
    "still stuck",
    "[rt][publisher]") {
  // Reach the same "retire ring full, one snapshot deferred" state as the
  // burst test above, then call acquire() again with nothing new published.
  // The deferred snapshot must survive untouched (not overwritten/destroyed)
  // and nothing must be adopted, since a stuck deferral means the retire ring
  // still has no room to retire whatever this call might otherwise adopt.
  CountedSnapshot::destroyed.store(0);
  sonare::rt::RtPublisher<CountedSnapshot> publisher;

  REQUIRE(publisher.publish(make_snapshot(0)));
  publisher.acquire([](const CountedSnapshot*, const CountedSnapshot*) {});

  constexpr size_t kBurst = sonare::rt::RtPublisher<CountedSnapshot>::kCapacity + 1;
  for (size_t i = 1; i <= kBurst; ++i) {
    REQUIRE(publisher.publish(make_snapshot(static_cast<uint64_t>(i))));
  }
  int call_count = 0;
  publisher.acquire([&](const CountedSnapshot*, const CountedSnapshot*) { ++call_count; });
  REQUIRE(call_count == 1);
  REQUIRE(publisher.current()->value == kBurst);
  // Snapshot 0 could not be retired this call (the ring was exactly full) and
  // is now the deferred one; nothing has been destroyed yet.
  REQUIRE(CountedSnapshot::destroyed.load() == 0);

  // A further call, with nothing new published, must not touch current() nor
  // destroy the still-deferred snapshot.
  publisher.acquire([&](const CountedSnapshot*, const CountedSnapshot*) { ++call_count; });
  REQUIRE(call_count == 1);  // the callback did not fire again.
  REQUIRE(publisher.current()->value == kBurst);
  REQUIRE(CountedSnapshot::destroyed.load() == 0);
}

TEST_CASE("RtPublisher callback acquire never destroys a snapshot on the consuming thread",
          "[rt][publisher]") {
  // Regression stress test for a fixed hazard: if flush_deferred_retire() at
  // the top of a call found the retire ring still full (deferred_retire_
  // still occupied) while this call would otherwise still adopt something
  // new, retiring the held-back `previous` snapshot at the end could ALSO
  // fail and overwrite deferred_retire_ -- destroying the OLDER deferred
  // snapshot right there, on this (the consuming/"audio") thread. The fix
  // makes the whole call a no-op whenever a deferral is still stuck, so
  // nothing is ever freed here. Real thread contention is what can put the
  // retire ring in that state between two acquire() calls, so this runs
  // concurrently rather than relying on a single-threaded sequence.
  ThreadTaggedSnapshot::destroyed_on_consumer_thread.store(false);

  sonare::rt::RtPublisher<ThreadTaggedSnapshot> publisher;
  constexpr int kIterations = 100000;
  std::atomic<bool> producer_done{false};

  std::thread producer([&] {
    for (int i = 0; i < kIterations; ++i) {
      auto snapshot = std::make_shared<ThreadTaggedSnapshot>();
      snapshot->value = i;
      while (!publisher.publish(snapshot)) {
        std::this_thread::yield();
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&] {
    ThreadTaggedSnapshot::consumer_thread_id.store(std::this_thread::get_id(),
                                                   std::memory_order_relaxed);
    while (true) {
      publisher.acquire([](const ThreadTaggedSnapshot*, const ThreadTaggedSnapshot*) {});
      if (producer_done.load(std::memory_order_acquire)) {
        publisher.acquire([](const ThreadTaggedSnapshot*, const ThreadTaggedSnapshot*) {});
        break;
      }
    }
  });

  producer.join();
  consumer.join();

  REQUIRE_FALSE(ThreadTaggedSnapshot::destroyed_on_consumer_thread.load());
}

TEST_CASE("RtSnapshot concurrent readers always see a valid pointer", "[rt][publisher]") {
  using Holder = sonare::rt::RtSnapshot<CountedSnapshot>;
  constexpr uint64_t kIterations = 20000;
  // RtSnapshot keeps only kRetain past generations alive (its documented
  // contract: a reader must finish using a loaded pointer before kRetain further
  // publishes occur). Flow-control the producer so it never runs more than
  // kRetain/2 generations ahead of the slowest reader's last-observed value.
  // This bounds the in-flight window regardless of thread scheduling (so the
  // test is robust even when readers are starved under a parallel test run),
  // while still exercising the concurrent lock-free load path.
  constexpr uint64_t kLead = Holder::kRetain / 2;

  CountedSnapshot::live.store(0);
  CountedSnapshot::destroyed.store(0);

  Holder snapshot_holder;
  snapshot_holder.publish(make_snapshot(1));

  std::atomic<bool> producer_done{false};
  std::atomic<bool> bad_read{false};
  std::array<std::atomic<uint64_t>, 2> reader_progress{};
  reader_progress[0].store(1, std::memory_order_relaxed);
  reader_progress[1].store(1, std::memory_order_relaxed);

  auto reader = [&](int id) {
    while (!producer_done.load(std::memory_order_acquire)) {
      const CountedSnapshot* snapshot = snapshot_holder.load();
      // The control thread keeps kRetain generations alive, so a loaded pointer
      // must always be dereferenceable and internally consistent.
      if (!snapshot || snapshot->value != snapshot->mirror) {
        bad_read.store(true, std::memory_order_relaxed);
        return;
      }
      // Report the generation just observed so the producer can avoid lapping
      // the retention window ahead of this reader.
      reader_progress[static_cast<size_t>(id)].store(snapshot->value, std::memory_order_relaxed);
    }
  };

  std::thread reader_a(reader, 0);
  std::thread reader_b(reader, 1);

  std::thread producer([&] {
    for (uint64_t i = 2; i <= kIterations; ++i) {
      // Wait until the slowest reader is within kLead generations before
      // publishing, so the slot a reader may currently hold is never reclaimed.
      while (!bad_read.load(std::memory_order_relaxed)) {
        const uint64_t slowest = std::min(reader_progress[0].load(std::memory_order_relaxed),
                                          reader_progress[1].load(std::memory_order_relaxed));
        if (i - slowest < kLead) break;
        std::this_thread::yield();
      }
      snapshot_holder.publish(make_snapshot(i));
    }
    producer_done.store(true, std::memory_order_release);
  });

  producer.join();
  reader_a.join();
  reader_b.join();

  REQUIRE_FALSE(bad_read.load(std::memory_order_relaxed));
  // The current snapshot is always valid after publishing completes.
  REQUIRE(snapshot_holder.load() != nullptr);
  REQUIRE(snapshot_holder.load()->value == kIterations);
}
