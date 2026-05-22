/// @file decompose_test.cpp
/// @brief Unit tests for effects/decompose (NMF / nn_filter).

#include "effects/decompose.h"

#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace sonare;

TEST_CASE("decompose returns non-negative W and H of expected shape", "[util][decompose]") {
  // Construct a small non-negative spectrogram.
  std::vector<float> S{
      1.0f, 2.0f, 1.0f, 2.0f, 4.0f, 2.0f, 0.5f, 1.0f, 0.5f,
  };
  auto r = decompose(S.data(), 3, 3, /*n_components=*/2, /*n_iter=*/50);
  REQUIRE(r.W.size() == 3 * 2);
  REQUIRE(r.H.size() == 2 * 3);
  for (float v : r.W) REQUIRE(v >= 0.0f);
  for (float v : r.H) REQUIRE(v >= 0.0f);
}

TEST_CASE("nn_filter preserves shape and stays non-negative", "[util][decompose]") {
  std::vector<float> S{
      1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f,
  };
  auto out = nn_filter(S.data(), 2, 4, "mean", 2, 1);
  REQUIRE(out.size() == 8);
  for (float v : out) REQUIRE(v >= 0.0f);
}

TEST_CASE("nn_filter median aggregator", "[util][decompose]") {
  std::vector<float> S{
      1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f,
  };
  auto out = nn_filter(S.data(), 2, 4, "median", 2, 1);
  REQUIRE(out.size() == 8);
}
