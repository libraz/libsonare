/// @file parallel_for_test.cpp
/// @brief Unit tests for util/parallel_for (every worker receives a usable functor).

#include "util/parallel_for.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace sonare;

namespace {

// Holds state a move empties, so a worker handed a moved-from copy covers nothing.
struct StatefulRange {
  std::vector<int> token = std::vector<int>(1, 1);
  std::atomic<int>* covered = nullptr;
  void operator()(int start, int end) const {
    if (!token.empty()) covered->fetch_add(end - start);
  }
};

}  // namespace

TEST_CASE("parallel_for hands every worker a usable rvalue functor", "[parallel_for]") {
  std::atomic<int> covered{0};
  parallel_for(8, 4, StatefulRange{std::vector<int>(1, 1), &covered});
  REQUIRE(covered.load() == 8);
}

TEST_CASE("parallel_for covers the range once with an lvalue functor", "[parallel_for]") {
  std::atomic<int> covered{0};
  const StatefulRange fn{std::vector<int>(1, 1), &covered};
  parallel_for(9, 4, fn);
  REQUIRE(covered.load() == 9);
  REQUIRE(fn.token.size() == 1);
}
