#include "rt/rt_publisher.h"

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

TEST_CASE("RtSnapshot concurrent readers always see a valid pointer", "[rt][publisher]") {
  constexpr int kIterations = 100000;
  CountedSnapshot::live.store(0);
  CountedSnapshot::destroyed.store(0);

  sonare::rt::RtSnapshot<CountedSnapshot> snapshot_holder;
  snapshot_holder.publish(make_snapshot(1));

  std::atomic<bool> producer_done{false};
  std::atomic<bool> bad_read{false};

  auto reader = [&] {
    while (!producer_done.load(std::memory_order_acquire)) {
      const CountedSnapshot* snapshot = snapshot_holder.load();
      // The control thread keeps kRetain generations alive, so a loaded pointer
      // must always be dereferenceable and internally consistent.
      if (!snapshot || snapshot->value != snapshot->mirror) {
        bad_read.store(true, std::memory_order_relaxed);
        return;
      }
    }
  };

  std::thread reader_a(reader);
  std::thread reader_b(reader);

  std::thread producer([&] {
    for (int i = 2; i <= kIterations; ++i) {
      snapshot_holder.publish(make_snapshot(static_cast<uint64_t>(i)));
    }
    producer_done.store(true, std::memory_order_release);
  });

  producer.join();
  reader_a.join();
  reader_b.join();

  REQUIRE_FALSE(bad_read.load(std::memory_order_relaxed));
  // The current snapshot is always valid after publishing completes.
  REQUIRE(snapshot_holder.load() != nullptr);
  REQUIRE(snapshot_holder.load()->value == static_cast<uint64_t>(kIterations));
}
