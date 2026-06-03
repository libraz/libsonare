#include "rt/overflow_counter.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <thread>

TEST_CASE("OverflowCounter starts at zero and counts bumps", "[rt][overflow]") {
  sonare::rt::OverflowCounter counter;
  CHECK(counter.load() == 0u);

  counter.bump();
  counter.bump();
  CHECK(counter.load() == 2u);

  counter.add(5);
  CHECK(counter.load() == 7u);

  counter.reset();
  CHECK(counter.load() == 0u);
}

// The counter exists to be incremented on the audio thread while a control
// thread polls it; the load/store/fetch_add are relaxed atomics so concurrent
// access is defined behaviour (no data race). This pins that contract: every
// bump must be observed, with no lost updates.
TEST_CASE("OverflowCounter loses no increments under a concurrent reader", "[rt][overflow]") {
  sonare::rt::OverflowCounter counter;
  std::atomic<bool> stop{false};

  std::thread reader([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      // Polling the telemetry concurrently must never tear or trip TSan.
      (void)counter.load();
    }
  });

  constexpr uint32_t kBumps = 100000;
  for (uint32_t i = 0; i < kBumps; ++i) counter.bump();

  stop.store(true, std::memory_order_relaxed);
  reader.join();

  CHECK(counter.load() == kBumps);
}
