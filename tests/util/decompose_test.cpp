/// @file decompose_test.cpp
/// @brief Unit tests for effects/decompose (NMF / nn_filter).

#include "effects/decompose.h"

#include <catch2/catch_approx.hpp>
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

TEST_CASE("decompose nndsvd init produces valid non-negative factorization", "[util][decompose]") {
  std::vector<float> S{
      1.0f, 2.0f, 1.0f, 2.0f, 4.0f, 2.0f, 0.5f, 1.0f, 0.5f,
  };
  auto r = decompose(S.data(), 3, 3, /*n_components=*/2, /*n_iter=*/50, "mu", 2.0f, "nndsvd");
  REQUIRE(r.W.size() == 3 * 2);
  REQUIRE(r.H.size() == 2 * 3);
  for (float v : r.W) REQUIRE(v >= 0.0f);
  for (float v : r.H) REQUIRE(v >= 0.0f);
}

TEST_CASE("decompose rejects an unknown init strategy", "[util][decompose]") {
  std::vector<float> S{1.0f, 2.0f, 3.0f, 4.0f};
  REQUIRE_THROWS(decompose(S.data(), 2, 2, /*n_components=*/1, /*n_iter=*/10, "mu", 2.0f, "bogus"));
}

TEST_CASE("nn_filter rejects a negative width", "[util][decompose]") {
  std::vector<float> S{
      1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f,
  };
  REQUIRE_THROWS(nn_filter(S.data(), 2, 4, "mean", 2, /*width=*/-1));
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

TEST_CASE("nn_filter median averages the two central values for an even count",
          "[util][decompose]") {
  // 1 feature, 5 frames, all parallel so cosine similarity is uniform; with
  // width=1 and k=2, column 2 selects the two lowest-index neighbours {0, 1}.
  // numpy.median of two values is their mean (15), not the upper one (20).
  std::vector<float> S{10.0f, 20.0f, 1.0f, 1.0f, 1.0f};
  auto out = nn_filter(S.data(), /*n_features=*/1, /*n_frames=*/5, "median", /*k=*/2, /*width=*/1);
  REQUIRE(out.size() == 5);
  REQUIRE(out[2] == Catch::Approx(15.0f));
}
