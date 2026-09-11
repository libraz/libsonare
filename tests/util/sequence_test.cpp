/// @file sequence_test.cpp
/// @brief Unit tests for util/sequence (DTW / Viterbi / RQA).

#include "util/sequence.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "util/exception.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;

namespace {

// ---------------------------------------------------------------------------
// Oracles: the traversal shapes dtw() and viterbi() used before the local-cost
// cache, the per-pair column norms and the whole trellis were taken out of them.
// Both are compared with exact `==`; a tolerance would pass against precisely
// the last-place drift these guard against, and a changed tie-break usually
// leaves the path's length alone.
// ---------------------------------------------------------------------------

float oracle_column_dot(const float* X, int rows, int cols, int i, const float* Y, int Y_cols,
                        int j) {
  float s = 0.0f;
  for (int r = 0; r < rows; ++r) {
    s += X[r * cols + i] * Y[r * Y_cols + j];
  }
  return s;
}

float oracle_column_norm(const float* X, int rows, int cols, int i) {
  float s = 0.0f;
  for (int r = 0; r < rows; ++r) {
    float v = X[r * cols + i];
    s += v * v;
  }
  return std::sqrt(s);
}

// Derives both operands' norms per (i, j), as the metric did before the hoist.
float oracle_cost(const std::string& metric, const float* X, int rows, int X_cols, int i,
                  const float* Y, int Y_cols, int j) {
  if (metric == "cosine") {
    float nx = oracle_column_norm(X, rows, X_cols, i);
    float ny = oracle_column_norm(Y, rows, Y_cols, j);
    if (nx == 0.0f || ny == 0.0f) return 1.0f;
    return 1.0f - oracle_column_dot(X, rows, X_cols, i, Y, Y_cols, j) / (nx * ny);
  }
  if (metric == "manhattan") {
    float s = 0.0f;
    for (int r = 0; r < rows; ++r) {
      s += std::fabs(X[r * X_cols + i] - Y[r * Y_cols + j]);
    }
    return s;
  }
  if (metric == "chebyshev") {
    float m = 0.0f;
    for (int r = 0; r < rows; ++r) {
      m = std::max(m, std::fabs(X[r * X_cols + i] - Y[r * Y_cols + j]));
    }
    return m;
  }
  float s = 0.0f;
  for (int r = 0; r < rows; ++r) {
    float d = X[r * X_cols + i] - Y[r * Y_cols + j];
    s += d * d;
  }
  return std::sqrt(s);
}

// The pre-hoist DTW: every local cost written into an [X_cols x Y_cols] cache
// before the recursion reads it. Default additive weights only, so the one
// multiply in the recursion is by 1.0f and cannot round differently here than
// in the library.
DtwResult oracle_dtw(const float* X, int rows, int X_cols, const float* Y, int Y_cols,
                     const std::string& metric, bool subseq,
                     const std::vector<std::pair<int, int>>& step_sizes_sigma = {}) {
  const std::vector<std::pair<int, int>> steps =
      step_sizes_sigma.empty() ? std::vector<std::pair<int, int>>{{1, 1}, {1, 0}, {0, 1}}
                               : step_sizes_sigma;
  const std::vector<float> weights(steps.size(), 1.0f);
  const float inf = std::numeric_limits<float>::infinity();

  DtwResult result;
  result.accumulated_cost.assign(static_cast<size_t>(X_cols) * Y_cols, inf);
  auto& D = result.accumulated_cost;

  std::vector<float> C(static_cast<size_t>(X_cols) * Y_cols, 0.0f);
  for (int i = 0; i < X_cols; ++i) {
    for (int j = 0; j < Y_cols; ++j) {
      C[i * Y_cols + j] = oracle_cost(metric, X, rows, X_cols, i, Y, Y_cols, j);
    }
  }

  for (int j = 0; j < Y_cols; ++j) {
    D[0 * Y_cols + j] = subseq ? C[0 * Y_cols + j] : (j == 0 ? C[0 * Y_cols + 0] : inf);
  }
  for (int i = 1; i < X_cols; ++i) {
    D[i * Y_cols + 0] = inf;
  }

  std::vector<int> back(static_cast<size_t>(X_cols) * Y_cols, -1);
  for (int i = 0; i < X_cols; ++i) {
    for (int j = 0; j < Y_cols; ++j) {
      if (i == 0 && j == 0) continue;
      if (i == 0 && subseq) continue;
      float best = inf;
      int best_step = -1;
      for (size_t s = 0; s < steps.size(); ++s) {
        const int pi = i - steps[s].first;
        const int pj = j - steps[s].second;
        if (pi < 0 || pj < 0) continue;
        const float prev = D[pi * Y_cols + pj];
        if (!std::isfinite(prev)) continue;
        const float candidate = prev + weights[s] * C[i * Y_cols + j];
        if (candidate < best) {
          best = candidate;
          best_step = static_cast<int>(s);
        }
      }
      if (best_step >= 0) {
        D[i * Y_cols + j] = best;
        back[i * Y_cols + j] = best_step;
      }
    }
  }

  int i = X_cols - 1;
  int j = Y_cols - 1;
  if (subseq) {
    float best = D[i * Y_cols + 0];
    int best_j = 0;
    for (int jj = 1; jj < Y_cols; ++jj) {
      if (D[i * Y_cols + jj] < best) {
        best = D[i * Y_cols + jj];
        best_j = jj;
      }
    }
    j = best_j;
  }
  result.distance = D[i * Y_cols + j];
  result.path.push_back({i, j});
  while (!(i == 0 && j == 0) && !(subseq && i == 0)) {
    const int s = back[i * Y_cols + j];
    if (s < 0) break;
    i -= steps[s].first;
    j -= steps[s].second;
    if (i < 0 || j < 0) break;
    result.path.push_back({i, j});
  }
  std::reverse(result.path.begin(), result.path.end());
  return result;
}

// The pre-reduction Viterbi: the whole [n_states x n_steps] trellis kept live,
// state-major backpointers, and the predecessor scan nested inside the state
// scan rather than outside it.
std::vector<int> oracle_viterbi(const float* log_prob, int n_states, int n_steps,
                                const float* transition, const float* p_init) {
  const float minus_inf = -std::numeric_limits<float>::infinity();
  std::vector<float> trellis(static_cast<size_t>(n_states) * n_steps, minus_inf);
  std::vector<int> backtrack(static_cast<size_t>(n_states) * n_steps, 0);

  for (int s = 0; s < n_states; ++s) {
    float init =
        p_init ? std::log(std::max(p_init[s], 1e-30f)) : -std::log(static_cast<float>(n_states));
    trellis[s * n_steps + 0] = init + log_prob[s * n_steps + 0];
  }
  for (int t = 1; t < n_steps; ++t) {
    for (int s = 0; s < n_states; ++s) {
      float best = minus_inf;
      int best_prev = 0;
      for (int p = 0; p < n_states; ++p) {
        float prob = std::max(transition[p * n_states + s], 1e-30f);
        float score = trellis[p * n_steps + (t - 1)] + std::log(prob);
        if (score > best) {
          best = score;
          best_prev = p;
        }
      }
      trellis[s * n_steps + t] = best + log_prob[s * n_steps + t];
      backtrack[s * n_steps + t] = best_prev;
    }
  }
  std::vector<int> path(n_steps, 0);
  int best_last = 0;
  float best_score = trellis[0 * n_steps + (n_steps - 1)];
  for (int s = 1; s < n_states; ++s) {
    if (trellis[s * n_steps + (n_steps - 1)] > best_score) {
      best_score = trellis[s * n_steps + (n_steps - 1)];
      best_last = s;
    }
  }
  path[n_steps - 1] = best_last;
  for (int t = n_steps - 1; t > 0; --t) {
    path[t - 1] = backtrack[path[t] * n_steps + t];
  }
  return path;
}

// Entries are the integers 0..4. Every metric then reduces to exact sums of
// exactly representable terms plus one correctly-rounded square root, so the
// comparison cannot fail for a reason other than the traversal.
std::vector<float> integer_matrix(int rows, int cols, uint32_t seed) {
  std::vector<float> m(static_cast<size_t>(rows) * cols);
  uint32_t state = seed;
  for (float& v : m) {
    state = state * 1664525u + 1013904223u;
    v = static_cast<float>((state >> 16) % 5u);
  }
  return m;
}

// Entries in [0, 1). The sums here are inexact, which is the point: a hoist that
// summed a column in a different order would land a rounding step away, and the
// integer fixture above is blind to that because its sums are exact. Exact `==`
// is still the right comparison across translation units -- the two summations
// are the same statement over the same order, and nothing in this tree is built
// with fast-math, so neither reassociation nor a differing contraction is on the
// table.
std::vector<float> unit_matrix(int rows, int cols, uint32_t seed) {
  std::vector<float> m(static_cast<size_t>(rows) * cols);
  uint32_t state = seed;
  for (float& v : m) {
    state = state * 1664525u + 1013904223u;
    v = static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
  }
  return m;
}

void require_same_dtw(const DtwResult& got, const DtwResult& want) {
  REQUIRE(got.distance == want.distance);
  REQUIRE(got.accumulated_cost.size() == want.accumulated_cost.size());
  for (size_t k = 0; k < want.accumulated_cost.size(); ++k) {
    CAPTURE(k);
    REQUIRE(got.accumulated_cost[k] == want.accumulated_cost[k]);
  }
  REQUIRE(got.path.size() == want.path.size());
  for (size_t k = 0; k < want.path.size(); ++k) {
    CAPTURE(k);
    REQUIRE(got.path[k].first == want.path[k].first);
    REQUIRE(got.path[k].second == want.path[k].second);
  }
}

}  // namespace

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

TEST_CASE("dtw rejects a cell count that would overflow int", "[util][sequence][dtw]") {
  // X_cols * Y_cols here is 2.5e9 > INT_MAX; the int index `i * Y_cols + j`
  // would overflow (UB). The guard must reject the dimensions before reading any
  // sample (so a tiny buffer is safe to pass).
  std::vector<float> tiny{0.0f};
  REQUIRE_THROWS_AS(dtw(tiny.data(), 1, 50000, tiny.data(), 1, 50000), SonareException);
}

TEST_CASE("rqa rejects a matrix size that would overflow int", "[util][sequence][rqa]") {
  // n * n is 2.5e9 > INT_MAX; the guard must reject before iterating.
  std::vector<float> tiny{0.0f};
  REQUIRE_THROWS_AS(rqa(tiny.data(), 50000), SonareException);
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

TEST_CASE("dtw matches a per-pair-norm oracle on every metric", "[util][sequence][dtw]") {
  const int rows = 4;
  const int X_cols = 7;
  const int Y_cols = 9;

  for (bool exact_fixture : {true, false}) {
    CAPTURE(exact_fixture);
    std::vector<float> X = exact_fixture ? integer_matrix(rows, X_cols, 0x51ed2701u)
                                         : unit_matrix(rows, X_cols, 0x51ed2701u);
    std::vector<float> Y = exact_fixture ? integer_matrix(rows, Y_cols, 0x1d872b41u)
                                         : unit_matrix(rows, Y_cols, 0x1d872b41u);
    // One silent column per side, so the cosine branch that gives up on a zero
    // norm is exercised rather than assumed unreachable.
    for (int r = 0; r < rows; ++r) {
      X[r * X_cols + 2] = 0.0f;
      Y[r * Y_cols + 5] = 0.0f;
    }

    for (const std::string metric : {"euclidean", "cosine", "manhattan", "chebyshev"}) {
      for (bool subseq : {false, true}) {
        CAPTURE(metric);
        CAPTURE(subseq);
        require_same_dtw(dtw(X.data(), rows, X_cols, Y.data(), rows, Y_cols, metric, subseq),
                         oracle_dtw(X.data(), rows, X_cols, Y.data(), Y_cols, metric, subseq));
      }
    }
  }
}

TEST_CASE("dtw matches the oracle under a non-default step pattern", "[util][sequence][dtw]") {
  // {(1,2), (2,1)} leaves cells the recursion cannot reach, so this also covers
  // the traceback bailing out on a cell with no backpointer.
  const int rows = 4;
  const int X_cols = 7;
  const int Y_cols = 9;
  const std::vector<float> X = integer_matrix(rows, X_cols, 0x2ac4f10bu);
  const std::vector<float> Y = integer_matrix(rows, Y_cols, 0x6e19d3c5u);
  const std::vector<std::pair<int, int>> steps{{1, 1}, {1, 2}, {2, 1}};

  require_same_dtw(dtw(X.data(), rows, X_cols, Y.data(), rows, Y_cols, "cosine", false, steps),
                   oracle_dtw(X.data(), rows, X_cols, Y.data(), Y_cols, "cosine", false, steps));
}

TEST_CASE("dtw keeps the first step of an exactly tied predecessor", "[util][sequence][dtw]") {
  // Every X column is (3, 4, 0) and every Y column is silent, so each local cost
  // is exactly 5 and a cell accumulates to (max(i, j) + 1) * 5. Above the
  // diagonal the diagonal step and the left step therefore reach the same float
  // by the same number of additions -- a real tie, not a near miss. The step list
  // is {(1,1), (1,0), (0,1)} and only a strict improvement displaces the
  // incumbent, so the diagonal has to win. Reversing the scan, or relaxing `<`
  // to `<=`, yields the left step and a different path of the same length.
  const int rows = 3;
  const int X_cols = 4;
  const int Y_cols = 6;
  std::vector<float> X(static_cast<size_t>(rows) * X_cols, 0.0f);
  for (int i = 0; i < X_cols; ++i) {
    X[0 * X_cols + i] = 3.0f;
    X[1 * X_cols + i] = 4.0f;
  }
  const std::vector<float> Y(static_cast<size_t>(rows) * Y_cols, 0.0f);

  const DtwResult r = dtw(X.data(), rows, X_cols, Y.data(), rows, Y_cols, "euclidean");
  REQUIRE(r.distance == 30.0f);
  const std::vector<std::pair<int, int>> expected{{0, 0}, {0, 1}, {0, 2}, {1, 3}, {2, 4}, {3, 5}};
  REQUIRE(r.path.size() == expected.size());
  for (size_t k = 0; k < expected.size(); ++k) {
    CAPTURE(k);
    REQUIRE(r.path[k].first == expected[k].first);
    REQUIRE(r.path[k].second == expected[k].second);
  }
}

TEST_CASE("viterbi matches a whole-trellis oracle", "[util][sequence][viterbi]") {
  const int n_states = 5;
  const int n_steps = 11;
  uint32_t state = 0x2f6b1d09u;
  auto next_unit = [&state]() {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
  };
  std::vector<float> log_prob(static_cast<size_t>(n_states) * n_steps);
  for (float& v : log_prob) v = -8.0f * next_unit();
  std::vector<float> transition(static_cast<size_t>(n_states) * n_states);
  for (float& v : transition) v = next_unit();
  std::vector<float> p_init(static_cast<size_t>(n_states));
  for (float& v : p_init) v = next_unit();

  const std::vector<const float*> priors{nullptr, p_init.data()};
  for (const float* init : priors) {
    CAPTURE(init != nullptr);
    const std::vector<int> got =
        viterbi(log_prob.data(), n_states, n_steps, transition.data(), init);
    const std::vector<int> want =
        oracle_viterbi(log_prob.data(), n_states, n_steps, transition.data(), init);
    REQUIRE(got.size() == want.size());
    for (size_t t = 0; t < want.size(); ++t) {
      CAPTURE(t);
      REQUIRE(got[t] == want[t]);
    }
  }
}

TEST_CASE("viterbi keeps the first predecessor of an exactly tied score",
          "[util][sequence][viterbi]") {
  // Uniform transitions and a uniform prior. States 0 and 2 carry the same
  // emission at step 0 and state 1 a worse one, so at step 1 the scores arriving
  // from predecessors 0 and 2 are the same float and the strict `>` has to keep
  // the lower index. Step 2 then singles out state 1, so the answer is read out
  // of the backpointers instead of off a flat last column: last-wins ties would
  // give {2, 2, 1} rather than {0, 0, 1}.
  const int n_states = 3;
  const int n_steps = 3;
  const std::vector<float> log_prob{
      0.0f,  0.0f, -1.0f,  // state 0
      -1.0f, 0.0f, 0.0f,   // state 1
      0.0f,  0.0f, -1.0f,  // state 2
  };
  const std::vector<float> transition(static_cast<size_t>(n_states) * n_states, 0.5f);
  const std::vector<float> p_init(static_cast<size_t>(n_states), 0.5f);

  const std::vector<int> path =
      viterbi(log_prob.data(), n_states, n_steps, transition.data(), p_init.data());
  const std::vector<int> expected{0, 0, 1};
  REQUIRE(path.size() == expected.size());
  for (size_t t = 0; t < expected.size(); ++t) {
    CAPTURE(t);
    REQUIRE(path[t] == expected[t]);
  }
}

TEST_CASE("viterbi rejects dimensions that overflow int indexing", "[util][sequence][viterbi]") {
  // n_states * n_steps (and n_states * n_states) are computed as int expressions
  // when indexing the trellis / transition matrix; a size whose product exceeds
  // INT_MAX is rejected before any allocation or pointer dereference, so the
  // dummy pointers below are never read.
  float dummy = 0.0f;
  REQUIRE_THROWS_AS(viterbi(&dummy, 50000, 50000, &dummy, nullptr), SonareException);
  REQUIRE_THROWS_AS(viterbi_discriminative(&dummy, 50000, 50000, &dummy, &dummy, nullptr),
                    SonareException);

  // A modestly sized valid problem still runs (no false positive from the guard).
  std::vector<float> log_prob(2 * 3, -0.1f);
  std::vector<float> trans{0.9f, 0.1f, 0.1f, 0.9f};
  REQUIRE_NOTHROW(viterbi(log_prob.data(), 2, 3, trans.data(), nullptr));
}
