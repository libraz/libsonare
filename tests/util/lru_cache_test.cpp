/// @file lru_cache_test.cpp
/// @brief Unit tests for util/lru_cache (eviction order, MRU, build semantics).

#include "util/lru_cache.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>

using namespace sonare;

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
