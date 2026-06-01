#include "util/sequence.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "util/exception.h"

namespace sonare {

namespace {

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

float pairwise_cost(const float* X, int rows, int X_cols, int i, const float* Y, int Y_cols, int j,
                    const std::string& metric) {
  if (metric == "cosine") {
    float nx = column_norm(X, rows, X_cols, i);
    float ny = column_norm(Y, rows, Y_cols, j);
    if (nx == 0.0f || ny == 0.0f) return 1.0f;
    return 1.0f - column_dot(X, rows, X_cols, i, Y, Y_cols, j) / (nx * ny);
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
  // Default: euclidean.
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

  // Local-cost cache: re-using the metric for each (i, j) lookup is the hot
  // path of the recursion, so we materialise it once.
  std::vector<float> C(static_cast<size_t>(X_cols) * Y_cols, 0.0f);
  for (int i = 0; i < X_cols; ++i) {
    for (int j = 0; j < Y_cols; ++j) {
      C[i * Y_cols + j] = pairwise_cost(X, X_rows, X_cols, i, Y, Y_cols, j, metric);
    }
  }

  // Initialise. `subseq` lets the path start at any column of Y by zeroing the
  // first-row prior; otherwise the path must start at (0, 0).
  for (int j = 0; j < Y_cols; ++j) {
    D[0 * Y_cols + j] = subseq
                            ? C[0 * Y_cols + j]
                            : (j == 0 ? C[0 * Y_cols + 0] : std::numeric_limits<float>::infinity());
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
      float best = std::numeric_limits<float>::infinity();
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
  const float minus_inf = -std::numeric_limits<float>::infinity();
  std::vector<float> trellis(static_cast<size_t>(n_states) * n_steps, minus_inf);
  std::vector<int> backtrack(static_cast<size_t>(n_states) * n_steps, 0);

  // Initial step.
  for (int s = 0; s < n_states; ++s) {
    float init =
        p_init ? std::log(std::max(p_init[s], 1e-30f)) : -std::log(static_cast<float>(n_states));
    trellis[s * n_steps + 0] = init + log_prob[s * n_steps + 0];
  }
  // Recursion.
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
  // Backtrack.
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

std::vector<int> viterbi_discriminative(const float* posteriors, int n_states, int n_steps,
                                        const float* transition, const float* p_state,
                                        const float* p_init) {
  if (posteriors == nullptr || transition == nullptr || p_state == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "viterbi_discriminative: null input");
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
