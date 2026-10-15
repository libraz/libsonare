#include "rt/rt_publisher.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

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

  // The two superseded snapshots stay in their hand-off slots. They are
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

TEST_CASE("RtPublisher never lets a pending snapshot override a newer ring publish",
          "[rt][publisher]") {
  sonare::rt::RtPublisher<int> publisher;
  REQUIRE(publisher.publish(std::make_shared<const int>(0)));
  publisher.acquire();

  // Fill the hand-off ring and coalesce later values into the pending slot.
  for (int value = 1; value <= 70; ++value) {
    REQUIRE(publisher.publish(std::make_shared<const int>(value)));
  }
  // One acquire reaches the coalesced value without another publish to unblock it.
  publisher.acquire();
  REQUIRE(publisher.current() != nullptr);
  REQUIRE(*publisher.current() == 70);
  for (int block = 0; block < 4; ++block) {
    publisher.acquire();
    REQUIRE(*publisher.current() == 70);
  }

  // A later publication through the ring is newer than the coalesced value.
  REQUIRE(publisher.publish(std::make_shared<const int>(71)));
  publisher.acquire();
  REQUIRE(publisher.current() != nullptr);
  REQUIRE(*publisher.current() == 71);
}

TEST_CASE("RtPublisher adopts the last snapshot of any burst without a further publish",
          "[rt][publisher]") {
  using Publisher = sonare::rt::RtPublisher<CountedSnapshot>;
  // Just past the ring plus the pending slot, and well past twice the ring.
  const uint64_t burst = GENERATE(static_cast<uint64_t>(Publisher::kCapacity + 6),
                                  static_cast<uint64_t>(3 * Publisher::kCapacity + 5));
  CountedSnapshot::live.store(0);
  CountedSnapshot::destroyed.store(0);
  Publisher publisher;

  REQUIRE(publisher.publish(make_snapshot(0)));
  publisher.acquire();
  for (uint64_t value = 1; value <= burst; ++value) {
    REQUIRE(publisher.publish(make_snapshot(value)));
  }
  REQUIRE(publisher.control_current()->value == burst);

  // No further publish: the audio thread alone must reach the newest snapshot
  // and stay there, and it must not free anything while doing so.
  const int destroyed_before_acquire = CountedSnapshot::destroyed.load();
  for (int block = 0; block < 4; ++block) {
    publisher.acquire();
    REQUIRE(publisher.current() != nullptr);
    REQUIRE(publisher.current() == publisher.control_current().get());
    REQUIRE(publisher.current()->value == burst);
  }
  REQUIRE(CountedSnapshot::destroyed.load() == destroyed_before_acquire);

  // The next control-thread publish frees every intermediate the burst left
  // behind: only the adopted snapshot and the new one remain.
  REQUIRE(publisher.publish(make_snapshot(burst + 1)));
  REQUIRE(CountedSnapshot::live.load() - CountedSnapshot::destroyed.load() == 2);
  publisher.acquire();
  REQUIRE(publisher.current()->value == burst + 1);
}

TEST_CASE("RtPublisher quiescent acquire drains a full ring before pending", "[rt][publisher]") {
  sonare::rt::RtPublisher<int> publisher;
  constexpr int kFinalValue = static_cast<int>(sonare::rt::RtPublisher<int>::kCapacity) + 1;
  REQUIRE(publisher.publish(std::make_shared<const int>(0)));
  publisher.acquire();

  for (int value = 1; value <= static_cast<int>(publisher.kCapacity); ++value) {
    REQUIRE(publisher.publish(std::make_shared<const int>(value)));
  }
  // The next publication is coalesced in the pending slot while the ring is
  // exactly full, matching the rebind path's staged replacement.
  REQUIRE(publisher.publish(std::make_shared<const int>(kFinalValue)));

  publisher.acquire_control_quiescent();
  REQUIRE(publisher.current() != nullptr);
  REQUIRE(*publisher.current() == kFinalValue);
  REQUIRE(publisher.control_current() != nullptr);
  REQUIRE(*publisher.control_current() == kFinalValue);
}

TEST_CASE("RtPublisher concurrent publish/acquire is torn-read and leak free", "[rt][publisher]") {
  constexpr int kIterations = 100000;
  CountedSnapshot::live.store(0);
  CountedSnapshot::destroyed.store(0);

  sonare::rt::RtPublisher<CountedSnapshot> publisher;
  std::atomic<bool> producer_done{false};
  std::atomic<bool> torn{false};
  std::atomic<bool> went_backwards{false};

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
  // that the value and its mirror copy always agree (no torn read) and that it
  // never adopts an older snapshot after a newer one.
  std::thread consumer([&] {
    uint64_t last_value = 0;
    while (true) {
      publisher.acquire();
      const CountedSnapshot* snapshot = publisher.current();
      if (snapshot && snapshot->value != snapshot->mirror) {
        torn.store(true, std::memory_order_relaxed);
      }
      if (snapshot) {
        if (snapshot->value < last_value) went_backwards.store(true, std::memory_order_relaxed);
        last_value = snapshot->value;
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
  REQUIRE_FALSE(went_backwards.load(std::memory_order_relaxed));
  REQUIRE(publisher.current()->value == static_cast<uint64_t>(kIterations));

  // After the consumer stopped, retired snapshots remain in the hand-off slots
  // until the control thread reclaims them on the next publish. Publishing
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

  // Every superseded snapshot is left in a hand-off slot, not freed here. A
  // further publish()/acquire() round trip (letting the control thread
  // reclaim, then draining once more) must still behave normally.
  REQUIRE(publisher.publish(make_snapshot(kBurst + 1)));
  publisher.acquire([](const CountedSnapshot*, const CountedSnapshot*) {});
  REQUIRE(publisher.current()->value == kBurst + 1);
}

TEST_CASE("RtPublisher callback acquire reaches the end of a long burst without a further publish",
          "[rt][publisher]") {
  using Publisher = sonare::rt::RtPublisher<CountedSnapshot>;
  CountedSnapshot::live.store(0);
  CountedSnapshot::destroyed.store(0);
  Publisher publisher;

  REQUIRE(publisher.publish(make_snapshot(0)));
  publisher.acquire([](const CountedSnapshot*, const CountedSnapshot*) {});

  constexpr uint64_t kBurst = 3 * Publisher::kCapacity + 5;
  for (uint64_t value = 1; value <= kBurst; ++value) {
    REQUIRE(publisher.publish(make_snapshot(value)));
  }
  const int destroyed_before_acquire = CountedSnapshot::destroyed.load();

  int call_count = 0;
  uint64_t seen_previous = kBurst;
  for (int block = 0; block < 4; ++block) {
    publisher.acquire([&](const CountedSnapshot* previous, const CountedSnapshot* next) {
      ++call_count;
      REQUIRE(previous != nullptr);
      seen_previous = previous->value;
      REQUIRE(next != nullptr);
      REQUIRE(next->value == kBurst);
    });
    REQUIRE(publisher.current() == publisher.control_current().get());
  }
  REQUIRE(call_count == 1);
  REQUIRE(seen_previous == 0);
  REQUIRE(CountedSnapshot::destroyed.load() == destroyed_before_acquire);

  // The control thread frees the previous snapshot and every skipped one.
  REQUIRE(publisher.publish(make_snapshot(kBurst + 1)));
  REQUIRE(CountedSnapshot::live.load() - CountedSnapshot::destroyed.load() == 2);
}

TEST_CASE("RtPublisher callback acquire never destroys a snapshot on the consuming thread",
          "[rt][publisher]") {
  // The held-back `previous` snapshot and every skipped one must reach the
  // control thread to be freed, never the consuming ("audio") thread. Real
  // contention is what interleaves a reclaim with a callback in flight, so
  // this runs concurrently rather than as a single-threaded sequence.
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

  CountedSnapshot::live.store(0);
  CountedSnapshot::destroyed.store(0);

  Holder snapshot_holder;
  snapshot_holder.publish(make_snapshot(1));

  std::atomic<bool> producer_done{false};
  std::atomic<bool> bad_read{false};

  // The producer runs unthrottled; a pin must keep its snapshot valid however far it laps.
  auto reader = [&] {
    while (!producer_done.load(std::memory_order_acquire)) {
      const auto snapshot = snapshot_holder.read();
      if (!snapshot || snapshot->value != snapshot->mirror) {
        bad_read.store(true, std::memory_order_relaxed);
        return;
      }
    }
  };

  std::thread reader_a(reader);
  std::thread reader_b(reader);

  std::thread producer([&] {
    for (uint64_t i = 2; i <= kIterations; ++i) {
      snapshot_holder.publish(make_snapshot(i));
    }
    producer_done.store(true, std::memory_order_release);
  });

  producer.join();
  reader_a.join();
  reader_b.join();

  REQUIRE_FALSE(bad_read.load(std::memory_order_relaxed));
  // With every pin released, every superseded snapshot is reclaimable.
  REQUIRE(snapshot_holder.try_reclaim());
  REQUIRE(CountedSnapshot::live.load() - CountedSnapshot::destroyed.load() == 1);
  const auto current = snapshot_holder.read();
  REQUIRE(current.get() != nullptr);
  REQUIRE(current->value == kIterations);
}

TEST_CASE("RtSnapshot keeps a stalled reader's snapshot alive across any number of publishes",
          "[rt][publisher]") {
  sonare::rt::RtSnapshot<CountedSnapshot> holder;
  auto first = make_snapshot(1);
  const std::weak_ptr<const CountedSnapshot> first_weak = first;
  holder.publish(std::move(first));

  std::vector<std::weak_ptr<const CountedSnapshot>> later;
  {
    const auto pin = holder.read();
    REQUIRE(pin.get() != nullptr);
    // Well past any fixed retention depth.
    for (uint64_t i = 2; i <= 200; ++i) {
      auto next = make_snapshot(i);
      later.emplace_back(next);
      holder.publish(std::move(next));
    }
    REQUIRE_FALSE(first_weak.expired());
    REQUIRE(pin->value == 1);
    REQUIRE(pin->mirror == 1);
    REQUIRE_FALSE(holder.try_reclaim());
    REQUIRE_FALSE(first_weak.expired());
  }

  REQUIRE(holder.try_reclaim());
  REQUIRE(first_weak.expired());
  // Every superseded snapshot is gone; only the newest is still owned.
  for (size_t i = 0; i + 1 < later.size(); ++i) {
    REQUIRE(later[i].expired());
  }
  REQUIRE_FALSE(later.back().expired());
}

TEST_CASE("RtSnapshot frees a superseded snapshot at publish when no pin is held",
          "[rt][publisher]") {
  sonare::rt::RtSnapshot<CountedSnapshot> holder;
  auto first = make_snapshot(1);
  const std::weak_ptr<const CountedSnapshot> first_weak = first;
  holder.publish(std::move(first));
  {
    const auto pin = holder.read();
    REQUIRE(pin->value == 1);
  }
  holder.publish(make_snapshot(2));
  REQUIRE(first_weak.expired());
}

TEST_CASE("RtSnapshot synchronize returns only after a pin taken on another thread is released",
          "[rt][publisher]") {
  sonare::rt::RtSnapshot<CountedSnapshot> holder;
  auto first = make_snapshot(1);
  const std::weak_ptr<const CountedSnapshot> first_weak = first;
  holder.publish(std::move(first));

  std::atomic<bool> pinned{false};
  std::atomic<bool> release{false};
  std::atomic<uint64_t> value_at_release{0};
  std::thread reader([&] {
    const auto pin = holder.read();
    pinned.store(true, std::memory_order_release);
    while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
    value_at_release.store(pin->value, std::memory_order_relaxed);
  });
  while (!pinned.load(std::memory_order_acquire)) std::this_thread::yield();

  holder.publish(make_snapshot(2));
  REQUIRE_FALSE(first_weak.expired());
  release.store(true, std::memory_order_release);
  holder.synchronize();
  // The reader's last use of the pinned snapshot happens before synchronize returns.
  REQUIRE(value_at_release.load(std::memory_order_relaxed) == 1);
  REQUIRE(first_weak.expired());
  reader.join();
}
