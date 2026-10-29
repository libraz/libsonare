/// @file lru_zero_capacity_test.cpp
/// @brief Unit tests for uncached behavior at zero LRU capacity.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <stdexcept>

#include "util/lru_cache.h"

using sonare::LruCache;

namespace {

struct RvalueOnlyBuilder {
  int* calls;

  int operator()() && { return ++*calls; }
  int operator()() & = delete;
};

struct LvalueOnlyBuilder {
  int* calls;

  int operator()() & { return ++*calls; }
  int operator()() && = delete;
};

}  // namespace

TEST_CASE("LruCache with zero capacity builds each get_or_build request", "[util][lru]") {
  LruCache<int, int> cache(0);
  int builds = 0;

  REQUIRE(cache.get_or_build(1, [&] { return ++builds; }) == 1);
  REQUIRE(cache.get_or_build(1, [&] { return ++builds; }) == 2);
  REQUIRE(builds == 2);

  // This accessor preserves forwarding to an rvalue-only callable.
  REQUIRE(cache.get_or_build(2, RvalueOnlyBuilder{&builds}) == 3);
}

TEST_CASE("LruCache with zero capacity builds each get_or_build_value request", "[util][lru]") {
  LruCache<int, int> cache(0);
  int builds = 0;

  REQUIRE(cache.get_or_build_value(1, [&] { return ++builds; }) == 1);
  REQUIRE(cache.get_or_build_value(1, [&] { return ++builds; }) == 2);
  REQUIRE(builds == 2);

  // This accessor invokes the named builder as an lvalue, as it does on a miss
  // at positive capacity.
  LvalueOnlyBuilder builder{&builds};
  REQUIRE(cache.get_or_build_value(2, builder) == 3);
}

TEST_CASE("LruCache with zero capacity does not retain returned values", "[util][lru]") {
  LruCache<int, std::shared_ptr<const int>> cache(0);
  std::weak_ptr<const int> first;
  std::weak_ptr<const int> second;

  {
    auto value = cache.get_or_build(1, [&] {
      auto built = std::make_shared<const int>(1);
      first = built;
      return built;
    });
    REQUIRE(*value == 1);
  }
  REQUIRE(first.expired());

  {
    auto value = cache.get_or_build_value(1, [&] {
      auto built = std::make_shared<const int>(2);
      second = built;
      return built;
    });
    REQUIRE(*value == 2);
  }
  REQUIRE(second.expired());
}

TEST_CASE("LruCache with zero capacity retries after builder exceptions", "[util][lru]") {
  for (bool build_under_lock : {false, true}) {
    CAPTURE(build_under_lock);
    LruCache<int, int> cache(0);
    int builds = 0;
    auto build = [&] {
      ++builds;
      if (builds == 1) throw std::runtime_error("builder failed");
      return builds;
    };
    auto get = [&] {
      return build_under_lock ? cache.get_or_build_value(1, build) : cache.get_or_build(1, build);
    };

    REQUIRE_THROWS_AS(get(), std::runtime_error);
    REQUIRE(get() == 2);
    cache.clear();
    REQUIRE(get() == 3);
    REQUIRE(builds == 3);
  }
}
