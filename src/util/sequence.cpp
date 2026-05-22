#include "util/sequence.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

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
  float s = 0.0f;
  for (int r = 0; r < rows; ++r) {
    float d = X[r * X_cols + i] - Y[r * Y_cols + j];
    s += d * d;
  }
  return std::sqrt(s);
}

}  // namespace

DtwResult dtw(const float* X, int X_rows, int X_cols, const float* Y, int Y_rows, int Y_cols,
              const std::string& metric, bool subseq) {
  if (X == nullptr || Y == nullptr) throw std::invalid_argument("dtw: null input");
  if (X_rows != Y_rows) throw std::invalid_argument("dtw: feature dims must match");
  if (X_cols <= 0 || Y_cols <= 0) return {};

  DtwResult result;
  result.accumulated_cost.assign(static_cast<size_t>(X_cols) * Y_cols,
                                 std::numeric_limits<float>::infinity());
  auto& D = result.accumulated_cost;

  for (int j = 0; j < Y_cols; ++j) {
    float c = pairwise_cost(X, X_rows, X_cols, 0, Y, Y_cols, j, metric);
    D[0 * Y_cols + j] = subseq ? c : (j == 0 ? c : D[0 * Y_cols + (j - 1)] + c);
  }
  for (int i = 1; i < X_cols; ++i) {
    D[i * Y_cols + 0] = D[(i - 1) * Y_cols + 0] + pairwise_cost(X, X_rows, X_cols, i, Y, Y_cols, 0,
                                                                 metric);
    for (int j = 1; j < Y_cols; ++j) {
      float c = pairwise_cost(X, X_rows, X_cols, i, Y, Y_cols, j, metric);
      float best = std::min({D[(i - 1) * Y_cols + j], D[i * Y_cols + (j - 1)],
                             D[(i - 1) * Y_cols + (j - 1)]});
      D[i * Y_cols + j] = best + c;
    }
  }

  // Traceback.
  int i = X_cols - 1;
  int j = Y_cols - 1;
  if (subseq) {
    // Choose endpoint along the last row.
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
  while (i > 0 || j > 0) {
    if (i == 0) {
      --j;
    } else if (j == 0) {
      --i;
    } else {
      float a = D[(i - 1) * Y_cols + j];
      float b = D[i * Y_cols + (j - 1)];
      float c = D[(i - 1) * Y_cols + (j - 1)];
      if (c <= a && c <= b) {
        --i;
        --j;
      } else if (a <= b) {
        --i;
      } else {
        --j;
      }
    }
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
  for (int i = 0; i < n2; ++i) if (rec[i] > 0.0f) ++n_rec;
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
    throw std::invalid_argument("viterbi: null input");
  }
  if (n_states <= 0 || n_steps <= 0) return {};
  const float minus_inf = -std::numeric_limits<float>::infinity();
  std::vector<float> trellis(static_cast<size_t>(n_states) * n_steps, minus_inf);
  std::vector<int> backtrack(static_cast<size_t>(n_states) * n_steps, 0);

  // Initial step.
  for (int s = 0; s < n_states; ++s) {
    float init = p_init ? std::log(std::max(p_init[s], 1e-30f))
                        : -std::log(static_cast<float>(n_states));
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
    throw std::invalid_argument("viterbi_discriminative: null input");
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
