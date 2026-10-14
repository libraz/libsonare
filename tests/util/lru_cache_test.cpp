/// @file lru_cache_test.cpp
/// @brief Unit tests for util/lru_cache (eviction order, MRU, build semantics).

#include "util/lru_cache.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace sonare;

namespace {
struct ThrowingCopyKey {
  int value;
  std::shared_ptr<int> copies_before_throw;

  ThrowingCopyKey(int value, std::shared_ptr<int> remaining)
      : value(value), copies_before_throw(std::move(remaining)) {}

  ThrowingCopyKey(const ThrowingCopyKey& other)
      : value(other.value), copies_before_throw(other.copies_before_throw) {
    if (*copies_before_throw > 0 && --*copies_before_throw == 0) {
      throw std::runtime_error("key copy failed");
    }
  }

  bool operator==(const ThrowingCopyKey& other) const noexcept { return value == other.value; }
};

struct ThrowingCopyKeyHash {
  size_t operator()(const ThrowingCopyKey& key) const noexcept {
    return std::hash<int>{}(key.value);
  }
};
}  // namespace

TEST_CASE("LruCache rolls back recency nodes when map insertion throws", "[util][lru]") {
  for (bool build_under_lock : {false, true}) {
    CAPTURE(build_under_lock);
    auto remaining = std::make_shared<int>(2);
    ThrowingCopyKey a(1, remaining), b(2, remaining), c(3, remaining);
    LruCache<ThrowingCopyKey, int, ThrowingCopyKeyHash> cache(2);
    auto get = [&](const ThrowingCopyKey& key, int value) {
      auto build = [value] { return value; };
      return build_under_lock ? cache.get_or_build_value(key, build)
                              : cache.get_or_build(key, build);
    };

    REQUIRE_THROWS_AS(get(a, 10), std::runtime_error);
    *remaining = 0;
    REQUIRE(get(a, 10) == 10);
    REQUIRE(get(b, 20) == 20);
    REQUIRE(get(a, -1) == 10);
    REQUIRE(get(c, 30) == 30);
    REQUIRE(get(a, -1) == 10);
    REQUIRE(get(b, 22) == 22);
  }
}

TEST_CASE("LruCache builds a missing value exactly once per key", "[util][lru]") {
  LruCache<int, std::string> cache(4);
  int builds = 0;
  auto make = [&](int k) {
    return cache.get_or_build(k, [&] {
      ++builds;
      return "v" + std::to_string(k);
    });
  };

  REQUIRE(make(1) == "v1");
  REQUIRE(make(1) == "v1");
  REQUIRE(make(2) == "v2");
  REQUIRE(builds == 2);  // key 1 built once, key 2 built once
}

TEST_CASE("LruCache evicts the least-recently-used entry at capacity", "[util][lru]") {
  LruCache<int, int> cache(2);
  auto build = [](int v) { return [v] { return v; }; };

  cache.get_or_build(1, build(10));
  cache.get_or_build(2, build(20));
  // Cache holds {2, 1}. Inserting 3 must evict the oldest (1).
  cache.get_or_build(3, build(30));

  int rebuilds = 0;
  auto counting = [&](int k, int v) {
    return cache.get_or_build(k, [&, v] {
      ++rebuilds;
      return v;
    });
  };
  REQUIRE(counting(1, 11) == 11);  // 1 was evicted: rebuilt
  REQUIRE(counting(3, 99) == 30);  // 3 still cached: original value
  REQUIRE(rebuilds == 1);
}

TEST_CASE("LruCache hit promotes a key to most-recently-used", "[util][lru]") {
  LruCache<int, int> cache(2);
  auto build = [](int v) { return [v] { return v; }; };

  cache.get_or_build(1, build(10));
  cache.get_or_build(2, build(20));
  // Touch 1 so it becomes MRU; the LRU entry is now 2.
  REQUIRE(cache.get_or_build(1, build(-1)) == 10);
  // Inserting 3 must evict 2 (now least-recently-used), keeping 1.
  cache.get_or_build(3, build(30));

  int rebuilds = 0;
  REQUIRE(cache.get_or_build(1, [&] {
    ++rebuilds;
    return -1;
  }) == 10);  // 1 survived
  REQUIRE(cache.get_or_build(2, [&] {
    ++rebuilds;
    return 22;
  }) == 22);  // 2 was evicted
  REQUIRE(rebuilds == 1);
}

TEST_CASE("LruCache clear drops all entries", "[util][lru]") {
  LruCache<int, int> cache(4);
  cache.get_or_build(1, [] { return 10; });
  cache.clear();
  int builds = 0;
  REQUIRE(cache.get_or_build(1, [&] {
    ++builds;
    return 11;
  }) == 11);
  REQUIRE(builds == 1);  // entry was cleared, so it rebuilds
}

TEST_CASE("LruCache get_or_build value survives concurrent eviction", "[util][lru]") {
  // Regression: get_or_build must copy the value out under the lock, so a handle
  // returned to one thread cannot be invalidated by another thread evicting the
  // entry. With a shared_ptr value type, the returned handle pins the payload
  // alive even after the map node is erased. We hammer a tiny cache from many
  // threads with more distinct keys than its capacity, forcing constant
  // eviction, and dereference every returned handle. Under the old by-reference
  // contract this raced into a use-after-free (caught by TSan/ASan).
  constexpr int kCapacity = 8;
  constexpr int kThreads = 8;
  constexpr int kIters = 4000;
  constexpr int kKeySpace = 64;  // >> capacity, so eviction is frequent

  LruCache<int, std::shared_ptr<const int>, std::hash<int>> cache(kCapacity);
  std::atomic<bool> corrupted{false};
  std::atomic<int> start{0};

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      while (start.load() == 0) {  // release threads together to maximize overlap
      }
      for (int i = 0; i < kIters; ++i) {
        int key = (i * 7 + t * 13) % kKeySpace;
        std::shared_ptr<const int> handle =
            cache.get_or_build(key, [key] { return std::make_shared<const int>(key * 1000); });
        // The handle must stay valid and consistent while other threads evict.
        if (!handle || *handle != key * 1000) {
          corrupted.store(true);
        }
      }
    });
  }

  start.store(1);
  for (auto& th : threads) th.join();
  REQUIRE_FALSE(corrupted.load());
}

TEST_CASE("LruCache get_or_build_value returns an owning copy", "[util][lru]") {
  LruCache<int, std::shared_ptr<int>> cache(2);
  auto handle = cache.get_or_build_value(1, [] { return std::make_shared<int>(42); });
  REQUIRE(handle);
  REQUIRE(*handle == 42);

  // A hit returns a copy of the same shared object.
  auto again = cache.get_or_build_value(1, [] { return std::make_shared<int>(-1); });
  REQUIRE(again.get() == handle.get());

  // Evicting key 1 must not invalidate the previously returned owning copy.
  cache.get_or_build_value(2, [] { return std::make_shared<int>(2); });
  cache.get_or_build_value(3, [] { return std::make_shared<int>(3); });
  REQUIRE(*handle == 42);
}
