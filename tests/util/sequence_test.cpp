/// @file sequence_test.cpp
/// @brief Unit tests for util/sequence (DTW / Viterbi / RQA).

#include "util/sequence.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;

TEST_CASE("dtw aligns identical sequences with zero cost", "[util][sequence][dtw]") {
  std::vector<float> X{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  // 3 features x 3 samples (identity).
  auto r = dtw(X.data(), 3, 3, X.data(), 3, 3);
  REQUIRE(r.distance < 1e-3f);
  REQUIRE(r.path.size() >= 3);
  REQUIRE(r.path.front().first == 0);
  REQUIRE(r.path.front().second == 0);
  REQUIRE(r.path.back().first == 2);
  REQUIRE(r.path.back().second == 2);
}

TEST_CASE("dtw subseq finds best alignment within Y", "[util][sequence][dtw]") {
  std::vector<float> X{1.0f, 0.0f};                          // 2 features x 1 sample
  std::vector<float> Y{0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f};  // 2 x 3 samples
  auto r = dtw(X.data(), 2, 1, Y.data(), 2, 3, "euclidean", /*subseq=*/true);
  REQUIRE(r.path.size() >= 1);
}

TEST_CASE("rqa on a perfect identity matrix returns rate=1/n", "[util][sequence][rqa]") {
  std::vector<float> R{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  auto stats = rqa(R.data(), 3);
  REQUIRE_THAT(stats.recurrence_rate, WithinAbs(3.0f / 9.0f, 1e-6f));
  REQUIRE(stats.max_diagonal_length == 3);
}

TEST_CASE("viterbi finds a single dominant state", "[util][sequence][viterbi]") {
  // 2 states, 4 time steps. Emission log-probs strongly favour state 0.
  std::vector<float> log_prob{
      0.0f,   0.0f,   0.0f,   0.0f,    // state 0
      -10.0f, -10.0f, -10.0f, -10.0f,  // state 1
  };
  std::vector<float> trans{0.9f, 0.1f, 0.1f, 0.9f};
  auto path = viterbi(log_prob.data(), 2, 4, trans.data(), nullptr);
  REQUIRE(path.size() == 4);
  for (int s : path) REQUIRE(s == 0);
}

TEST_CASE("viterbi_discriminative respects state priors", "[util][sequence][viterbi]") {
  std::vector<float> posteriors{
      0.9f, 0.9f, 0.9f, 0.1f, 0.1f, 0.1f,
  };
  std::vector<float> trans{0.9f, 0.1f, 0.1f, 0.9f};
  std::vector<float> prior{0.5f, 0.5f};
  auto path = viterbi_discriminative(posteriors.data(), 2, 3, trans.data(), prior.data());
  REQUIRE(path.size() == 3);
  for (int s : path) REQUIRE(s == 0);
}
