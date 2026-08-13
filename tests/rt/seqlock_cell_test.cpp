#include "rt/seqlock_cell.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <thread>

namespace {

// POD snapshot carrying its value twice so a torn read (the two halves
// disagreeing) is detectable by a concurrent reader.
struct Pair {
  uint64_t a = 0;
  uint64_t b = 0;
};

}  // namespace

TEST_CASE("SeqlockCell load returns the published value", "[rt][seqlock]") {
  sonare::rt::SeqlockCell<Pair> cell;
  REQUIRE(cell.load().a == 0u);

  cell.store({7, 7});
  const Pair v = cell.load();
  CHECK(v.a == 7u);
  CHECK(v.b == 7u);
}

TEST_CASE("SeqlockCell explicit-initial constructor seeds both value and cache", "[rt][seqlock]") {
  sonare::rt::SeqlockCell<Pair> cell{Pair{3, 3}};
  // Spinning and non-spinning reads agree on the seed before any store.
  CHECK(cell.load().a == 3u);
  CHECK(cell.try_load().a == 3u);
}

TEST_CASE("SeqlockCell try_load reflects the latest store", "[rt][seqlock]") {
  sonare::rt::SeqlockCell<Pair> cell;
  cell.store({1, 1});
  CHECK(cell.try_load().a == 1u);
  cell.store({2, 2});
  CHECK(cell.try_load().a == 2u);
}

TEST_CASE("SeqlockCell never tears under a concurrent writer", "[.][slow][rt][seqlock]") {
  sonare::rt::SeqlockCell<Pair> cell;
  std::atomic<bool> stop{false};

  std::thread writer([&] {
    for (uint64_t i = 1; !stop.load(std::memory_order_relaxed); ++i) {
      cell.store({i, i});
    }
  });

  // The reader must only ever observe a consistent (a == b) snapshot, whether
  // it spins (load) or falls back to the cached value (try_load).
  for (int i = 0; i < 200000; ++i) {
    const Pair s = cell.load();
    REQUIRE(s.a == s.b);
    const Pair t = cell.try_load();
    REQUIRE(t.a == t.b);
  }

  stop.store(true, std::memory_order_relaxed);
  writer.join();
}

TEST_CASE(
    "SeqlockCell try_load_into never reports success on a torn read and never mutates *out "
    "on conflict",
    "[.][slow][rt][seqlock]") {
  // try_load_into() exists precisely so a caller can tell "nothing new" apart
  // from "conflicted, retry me" (unlike try_load(), which silently
  // substitutes a stale cached value on conflict — see
  // RealtimeVoiceChanger::adopt_snapshot_for_block for the real-world bug
  // that distinction fixes: naively trusting an unconditional read let a
  // torn read get marked "applied" and permanently dropped an update).
  //
  // This test races a real writer thread against try_load_into() and checks
  // both halves of its contract on every call: a call that returns true must
  // never be torn, and a call that returns false must leave the caller's
  // output buffer completely untouched (no silent substitution of a stale or
  // partial value). Whether this particular run actually produces a
  // conflicting read depends on OS scheduling and is not guaranteed on every
  // machine; the assertions below hold regardless of whether one occurs.
  sonare::rt::SeqlockCell<Pair> cell;
  cell.store({1, 1});
  std::atomic<bool> stop{false};
  std::atomic<int> conflicts_observed{0};

  std::thread writer([&] {
    for (uint64_t i = 2; !stop.load(std::memory_order_relaxed); ++i) {
      cell.store({i, i});
    }
  });

  constexpr Pair kSentinel{0xDEADBEEFu, 0xCAFEBABEu};
  for (int i = 0; i < 500000; ++i) {
    Pair out = kSentinel;
    if (cell.try_load_into(&out)) {
      REQUIRE(out.a == out.b);
    } else {
      REQUIRE(out.a == kSentinel.a);
      REQUIRE(out.b == kSentinel.b);
      conflicts_observed.fetch_add(1, std::memory_order_relaxed);
    }
  }

  stop.store(true, std::memory_order_relaxed);
  writer.join();

  // Not a correctness requirement (see comment above), but report whether
  // this run actually exercised the conflict path, since a run that never
  // does only re-proves the uncontended case already covered by the "never
  // tears" test above.
  INFO("conflicting reads observed: " << conflicts_observed.load());
}
