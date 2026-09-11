#include "util/sequence.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "util/exception.h"

namespace sonare {

namespace {

enum class DtwMetric { Euclidean, Cosine, Manhattan, Chebyshev };

DtwMetric parse_dtw_metric(const std::string& metric) {
  if (metric == "cosine") return DtwMetric::Cosine;
  if (metric == "manhattan") return DtwMetric::Manhattan;
  if (metric == "chebyshev") return DtwMetric::Chebyshev;
  return DtwMetric::Euclidean;
}

float column_dot(const float* X, int rows, int cols, int i, const float* Y, int Y_cols, int j) {
  float s = 0.0f;
  for (int r = 0; r < rows; ++r) {
    s += X[r * cols + i] * Y[r * Y_cols + j];
  }
  return s;
}

float column_norm(const float* X, int rows, int cols, int i) {
  float s = 0.0f;
  for (int r = 0; r < rows; ++r) {
    float v = X[r * cols + i];
    s += v * v;
  }
  return std::sqrt(s);
}

/// @brief Every column norm of a [rows x cols] row-major matrix, in one pass.
/// @details A cosine cost needs both operands' norms, but each depends on one
///          index only, so deriving them inside the O(X_cols * Y_cols) pair loop
///          re-runs the same X_cols + Y_cols reductions once per pair. Hoisting
///          is exact rather than an approximation: the values come from the same
///          column_norm() over the same data, so every pairwise result is
///          bit-identical. feature/segment.cpp hoists the same way.
std::vector<float> column_norms(const float* X, int rows, int cols) {
  std::vector<float> norms(static_cast<size_t>(cols));
  for (int j = 0; j < cols; ++j) {
    norms[static_cast<size_t>(j)] = column_norm(X, rows, cols, j);
  }
  return norms;
}

float pairwise_cost(const float* X, int rows, int X_cols, int i, const float* Y, int Y_cols, int j,
                    DtwMetric metric, float nx, float ny) {
  switch (metric) {
    case DtwMetric::Cosine: {
      if (nx == 0.0f || ny == 0.0f) return 1.0f;
      return 1.0f - column_dot(X, rows, X_cols, i, Y, Y_cols, j) / (nx * ny);
    }
    case DtwMetric::Manhattan: {
      float s = 0.0f;
      for (int r = 0; r < rows; ++r) {
        s += std::fabs(X[r * X_cols + i] - Y[r * Y_cols + j]);
      }
      return s;
    }
    case DtwMetric::Chebyshev: {
      float m = 0.0f;
      for (int r = 0; r < rows; ++r) {
        m = std::max(m, std::fabs(X[r * X_cols + i] - Y[r * Y_cols + j]));
      }
      return m;
    }
    case DtwMetric::Euclidean:
      break;
  }
  float s = 0.0f;
  for (int r = 0; r < rows; ++r) {
    float d = X[r * X_cols + i] - Y[r * Y_cols + j];
    s += d * d;
  }
  return std::sqrt(s);
}

}  // namespace

DtwResult dtw(const float* X, int X_rows, int X_cols, const float* Y, int Y_rows, int Y_cols,
              const std::string& metric, bool subseq,
              const std::vector<std::pair<int, int>>& step_sizes_sigma,
              const std::vector<float>& weights_add) {
  if (X == nullptr || Y == nullptr)
    throw SonareException(ErrorCode::InvalidParameter, "dtw: null input");
  if (X_rows != Y_rows)
    throw SonareException(ErrorCode::InvalidParameter, "dtw: feature dims must match");
  if (X_cols <= 0 || Y_cols <= 0) return {};
  // The accumulation and backpointer matrices are indexed with the int
  // expression `i * Y_cols + j`; reject a cell count that would overflow int
  // (UB) before any allocation. The largest index is X_cols * Y_cols - 1, so
  // guard that product. (~46k x 46k cells is already far beyond any musical
  // alignment.)
  if (static_cast<int64_t>(X_cols) * static_cast<int64_t>(Y_cols) >
      static_cast<int64_t>(std::numeric_limits<int>::max())) {
    throw SonareException(ErrorCode::InvalidParameter, "dtw: cost matrix too large");
  }

  // Resolve the step pattern. Default is symmetric P0: {(1,1),(1,0),(0,1)}.
  std::vector<std::pair<int, int>> steps =
      step_sizes_sigma.empty() ? std::vector<std::pair<int, int>>{{1, 1}, {1, 0}, {0, 1}}
                               : step_sizes_sigma;
  for (const auto& s : steps) {
    if (s.first < 0 || s.second < 0 || (s.first == 0 && s.second == 0)) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "dtw: step sizes must be non-negative and not (0,0)");
    }
  }
  std::vector<float> weights = weights_add;
  if (weights.empty()) {
    weights.assign(steps.size(), 1.0f);
  } else if (weights.size() != steps.size()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "dtw: weights_add must have the same length as step_sizes_sigma");
  }

  DtwResult result;
  result.accumulated_cost.assign(static_cast<size_t>(X_cols) * Y_cols,
                                 std::numeric_limits<float>::infinity());
  auto& D = result.accumulated_cost;

  // Each local cost is consumed by the single cell that owns it, so it is
  // derived in place instead of materialised into an [X_cols x Y_cols] cache.
  // Only the cosine norms are worth hoisting, and they are one value per column.
  const DtwMetric resolved_metric = parse_dtw_metric(metric);
  std::vector<float> X_norms(static_cast<size_t>(X_cols), 0.0f);
  std::vector<float> Y_norms(static_cast<size_t>(Y_cols), 0.0f);
  if (resolved_metric == DtwMetric::Cosine) {
    X_norms = column_norms(X, X_rows, X_cols);
    Y_norms = column_norms(Y, Y_rows, Y_cols);
  }
  auto local_cost = [&](int i, int j) {
    return pairwise_cost(X, X_rows, X_cols, i, Y, Y_cols, j, resolved_metric,
                         X_norms[static_cast<size_t>(i)], Y_norms[static_cast<size_t>(j)]);
  };

  // Initialise. `subseq` lets the path start at any column of Y by zeroing the
  // first-row prior; otherwise the path must start at (0, 0).
  for (int j = 0; j < Y_cols; ++j) {
    D[0 * Y_cols + j] = subseq
                            ? local_cost(0, j)
                            : (j == 0 ? local_cost(0, 0) : std::numeric_limits<float>::infinity());
  }
  for (int i = 1; i < X_cols; ++i) {
    D[i * Y_cols + 0] = std::numeric_limits<float>::infinity();
  }
  // Forward recursion using the supplied step pattern.
  // Backpointers store the index into `steps` for traceback.
  std::vector<int> back(static_cast<size_t>(X_cols) * Y_cols, -1);
  for (int i = 0; i < X_cols; ++i) {
    for (int j = 0; j < Y_cols; ++j) {
      if (i == 0 && j == 0) continue;
      if (i == 0 && subseq) continue;
      const float local = local_cost(i, j);
      float best = std::numeric_limits<float>::infinity();
      int best_step = -1;
      for (size_t s = 0; s < steps.size(); ++s) {
        const int pi = i - steps[s].first;
        const int pj = j - steps[s].second;
        if (pi < 0 || pj < 0) continue;
        const float prev = D[pi * Y_cols + pj];
        if (!std::isfinite(prev)) continue;
        const float candidate = prev + weights[s] * local;
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

  // Traceback.
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
    if (s < 0) {
      // Defensive: shouldn't happen on well-formed inputs, but bail to avoid
      // infinite loops if the recursion couldn't reach (i, j) via the steps.
      break;
    }
    i -= steps[s].first;
    j -= steps[s].second;
    if (i < 0 || j < 0) break;
    result.path.push_back({i, j});
  }
  std::reverse(result.path.begin(), result.path.end());
  return result;
}

RqaResult rqa(const float* rec, int n) {
  RqaResult out;
  if (rec == nullptr || n <= 0) return out;
  // rec is an n x n matrix indexed with the int expression `i * n + j`; reject a
  // size whose element count would overflow int (UB) before iterating.
  if (static_cast<int64_t>(n) * static_cast<int64_t>(n) >
      static_cast<int64_t>(std::numeric_limits<int>::max())) {
    throw SonareException(ErrorCode::InvalidParameter, "rqa: recurrence matrix too large");
  }
  int n2 = n * n;
  int n_rec = 0;
  for (int i = 0; i < n2; ++i)
    if (rec[i] > 0.0f) ++n_rec;
  out.recurrence_rate = static_cast<float>(n_rec) / static_cast<float>(n2);

  // Collect diagonal line lengths (lines of consecutive ones along main diagonal direction).
  std::vector<int> lengths;
  for (int d = -(n - 1); d < n; ++d) {
    int run = 0;
    for (int i = std::max(0, -d); i + 1 <= n && i + d < n && i + d >= 0; ++i) {
      if (rec[i * n + (i + d)] > 0.0f) {
        ++run;
      } else {
        if (run >= 2) lengths.push_back(run);
        run = 0;
      }
    }
    if (run >= 2) lengths.push_back(run);
  }
  int sum_diag = 0;
  for (int L : lengths) sum_diag += L;
  out.determinism = (n_rec > 0) ? static_cast<float>(sum_diag) / static_cast<float>(n_rec) : 0.0f;
  out.max_diagonal_length = lengths.empty() ? 0 : *std::max_element(lengths.begin(), lengths.end());
  out.average_diagonal_length =
      lengths.empty() ? 0.0f : static_cast<float>(sum_diag) / static_cast<float>(lengths.size());
  return out;
}

std::vector<int> viterbi(const float* log_prob, int n_states, int n_steps, const float* transition,
                         const float* p_init) {
  if (log_prob == nullptr || transition == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "viterbi: null input");
  }
  if (n_states <= 0 || n_steps <= 0) return {};
  // The emissions are indexed with the int expression `s * n_steps + t`, the
  // backpointers with `t * n_states + s`, and the transition matrix with
  // `p * n_states + s`; reject sizes whose element counts would overflow int
  // (UB) before iterating, matching dtw/rqa above.
  const int64_t int_max = static_cast<int64_t>(std::numeric_limits<int>::max());
  if (static_cast<int64_t>(n_states) * static_cast<int64_t>(n_steps) > int_max ||
      static_cast<int64_t>(n_states) * static_cast<int64_t>(n_states) > int_max) {
    throw SonareException(ErrorCode::InvalidParameter, "viterbi: trellis too large");
  }
  const float minus_inf = -std::numeric_limits<float>::infinity();
  // The update reads step t-1's scores and nothing older, and the backtrack pass reads
  // backpointers rather than scores, so only two score rows are ever live. Backpointers stay
  // flat, step-major [n_steps x n_states], so the forward sweep over states writes contiguously
  // and the backtrack pass reads one element per step.
  std::vector<float> prev_scores(static_cast<size_t>(n_states), minus_inf);
  std::vector<float> curr_scores(static_cast<size_t>(n_states), minus_inf);
  std::vector<int> backtrack(static_cast<size_t>(n_states) * n_steps, 0);

  // Initial step.
  for (int s = 0; s < n_states; ++s) {
    float init =
        p_init ? std::log(std::max(p_init[s], 1e-30f)) : -std::log(static_cast<float>(n_states));
    prev_scores[static_cast<size_t>(s)] = init + log_prob[s * n_steps + 0];
  }
  // Recursion. Predecessor-major with per-state accumulators: `transition` is row-major
  // [n_states x n_states], so a predecessor's outgoing row is contiguous. Each state still sees
  // its candidates in predecessor order 0..n_states-1, so the strict `>` keeps the same
  // first-wins predecessor on a tie and every score is unchanged bit for bit.
  std::vector<float> best(static_cast<size_t>(n_states), minus_inf);
  std::vector<int> best_prev(static_cast<size_t>(n_states), 0);
  for (int t = 1; t < n_steps; ++t) {
    std::fill(best.begin(), best.end(), minus_inf);
    std::fill(best_prev.begin(), best_prev.end(), 0);
    for (int p = 0; p < n_states; ++p) {
      const float previous = prev_scores[static_cast<size_t>(p)];
      const float* row = transition + static_cast<size_t>(p) * n_states;
      for (int s = 0; s < n_states; ++s) {
        float prob = std::max(row[s], 1e-30f);
        float score = previous + std::log(prob);
        if (score > best[static_cast<size_t>(s)]) {
          best[static_cast<size_t>(s)] = score;
          best_prev[static_cast<size_t>(s)] = p;
        }
      }
    }
    for (int s = 0; s < n_states; ++s) {
      curr_scores[static_cast<size_t>(s)] =
          best[static_cast<size_t>(s)] + log_prob[s * n_steps + t];
      backtrack[t * n_states + s] = best_prev[static_cast<size_t>(s)];
    }
    prev_scores.swap(curr_scores);
  }
  // Backtrack. After the last swap prev_scores holds step n_steps-1; for n_steps == 1 it holds
  // step 0.
  std::vector<int> path(n_steps, 0);
  int best_last = 0;
  float best_score = prev_scores[0];
  for (int s = 1; s < n_states; ++s) {
    if (prev_scores[static_cast<size_t>(s)] > best_score) {
      best_score = prev_scores[static_cast<size_t>(s)];
      best_last = s;
    }
  }
  path[n_steps - 1] = best_last;
  for (int t = n_steps - 1; t > 0; --t) {
    path[t - 1] = backtrack[t * n_states + path[t]];
  }
  return path;
}

std::vector<int> viterbi_discriminative(const float* posteriors, int n_states, int n_steps,
                                        const float* transition, const float* p_state,
                                        const float* p_init) {
  if (posteriors == nullptr || transition == nullptr || p_state == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "viterbi_discriminative: null input");
  }
  if (n_states <= 0 || n_steps <= 0) return {};
  // Mirror viterbi's overflow guard: this function indexes `s * n_steps + t` with
  // int arithmetic in the loop below, before delegating to viterbi().
  if (static_cast<int64_t>(n_states) * static_cast<int64_t>(n_steps) >
      static_cast<int64_t>(std::numeric_limits<int>::max())) {
    throw SonareException(ErrorCode::InvalidParameter, "viterbi_discriminative: trellis too large");
  }
  std::vector<float> log_prob(static_cast<size_t>(n_states) * n_steps);
  for (int s = 0; s < n_states; ++s) {
    float lp_state = std::log(std::max(p_state[s], 1e-30f));
    for (int t = 0; t < n_steps; ++t) {
      float post = std::max(posteriors[s * n_steps + t], 1e-30f);
      log_prob[s * n_steps + t] = std::log(post) - lp_state;
    }
  }
  return viterbi(log_prob.data(), n_states, n_steps, transition, p_init);
}

}  // namespace sonare
