#include "feature/segment.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace sonare {

namespace {

inline float column_norm(const float* X, int rows, int cols, int j) {
  float s = 0.0f;
  for (int r = 0; r < rows; ++r) {
    float v = X[r * cols + j];
    s += v * v;
  }
  return std::sqrt(s);
}

float cosine_sim(const float* X, int rows, int X_cols, const float* Y, int Y_cols, int i, int j) {
  float dot = 0.0f;
  for (int r = 0; r < rows; ++r) {
    dot += X[r * X_cols + i] * Y[r * Y_cols + j];
  }
  float nx = column_norm(X, rows, X_cols, i);
  float ny = column_norm(Y, rows, Y_cols, j);
  if (nx == 0.0f || ny == 0.0f) return 0.0f;
  return dot / (nx * ny);
}

float euclidean_dist(const float* X, int rows, int X_cols, const float* Y, int Y_cols, int i,
                     int j) {
  float s = 0.0f;
  for (int r = 0; r < rows; ++r) {
    float d = X[r * X_cols + i] - Y[r * Y_cols + j];
    s += d * d;
  }
  return std::sqrt(s);
}

}  // namespace

std::vector<float> cross_similarity(const float* X, int X_rows, int X_cols, const float* Y,
                                    int Y_rows, int Y_cols, int k, const std::string& metric) {
  if (X == nullptr || Y == nullptr) throw std::invalid_argument("cross_similarity: null input");
  if (X_rows != Y_rows) throw std::invalid_argument("cross_similarity: feature dims must match");
  if (X_cols <= 0 || Y_cols <= 0) return {};
  std::vector<float> out(static_cast<size_t>(X_cols) * Y_cols, 0.0f);
  bool use_cosine = (metric == "cosine");
  for (int i = 0; i < X_cols; ++i) {
    for (int j = 0; j < Y_cols; ++j) {
      float sim = use_cosine ? cosine_sim(X, X_rows, X_cols, Y, Y_cols, i, j)
                             : -euclidean_dist(X, X_rows, X_cols, Y, Y_cols, i, j);
      out[i * Y_cols + j] = sim;
    }
  }
  if (k > 0 && k < Y_cols) {
    // Keep only top-k per row.
    std::vector<size_t> order(Y_cols);
    for (int i = 0; i < X_cols; ++i) {
      std::iota(order.begin(), order.end(), size_t{0});
      std::partial_sort(order.begin(), order.begin() + k, order.end(),
                        [&](size_t a, size_t b) {
                          return out[i * Y_cols + a] > out[i * Y_cols + b];
                        });
      std::vector<float> keep(Y_cols, 0.0f);
      for (int q = 0; q < k; ++q) keep[order[q]] = out[i * Y_cols + order[q]];
      for (int j = 0; j < Y_cols; ++j) out[i * Y_cols + j] = keep[j];
    }
  }
  return out;
}

std::vector<float> recurrence_matrix(const float* data, int rows, int cols, int k, int width,
                                     bool sym, const std::string& metric) {
  std::vector<float> out = cross_similarity(data, rows, cols, data, rows, cols, 0, metric);
  // Zero out the central diagonal band.
  for (int i = 0; i < cols; ++i) {
    for (int j = 0; j < cols; ++j) {
      if (std::abs(i - j) < width) out[i * cols + j] = 0.0f;
    }
  }
  if (k > 0 && k < cols) {
    // Per-row top-k filtering (replicates librosa's `mode="affinity"` k-NN).
    std::vector<size_t> order(cols);
    for (int i = 0; i < cols; ++i) {
      std::iota(order.begin(), order.end(), size_t{0});
      std::partial_sort(order.begin(), order.begin() + k, order.end(),
                        [&](size_t a, size_t b) {
                          return out[i * cols + a] > out[i * cols + b];
                        });
      std::vector<float> keep(cols, 0.0f);
      for (int q = 0; q < k; ++q) keep[order[q]] = out[i * cols + order[q]];
      for (int j = 0; j < cols; ++j) out[i * cols + j] = keep[j];
    }
  }
  if (sym) {
    for (int i = 0; i < cols; ++i) {
      for (int j = i + 1; j < cols; ++j) {
        float a = out[i * cols + j];
        float b = out[j * cols + i];
        float v = std::min(a, b);
        out[i * cols + j] = v;
        out[j * cols + i] = v;
      }
    }
  }
  return out;
}

std::vector<float> recurrence_to_lag(const float* rec, int n, bool pad) {
  if (rec == nullptr || n <= 0) return {};
  int n_lags = pad ? (2 * n - 1) : n;
  std::vector<float> out(static_cast<size_t>(n) * n_lags, 0.0f);
  int offset = pad ? (n - 1) : 0;
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      int lag = j - i;
      int col = lag + offset;
      if (!pad) {
        if (lag < 0) col = lag + n;  // librosa wraps for non-padded.
      }
      if (col >= 0 && col < n_lags) out[i * n_lags + col] = rec[i * n + j];
    }
  }
  return out;
}

std::vector<float> lag_to_recurrence(const float* lag, int n_rows, int n_lags) {
  if (lag == nullptr || n_rows <= 0 || n_lags <= 0) return {};
  std::vector<float> out(static_cast<size_t>(n_rows) * n_rows, 0.0f);
  for (int i = 0; i < n_rows; ++i) {
    for (int col = 0; col < n_lags; ++col) {
      int lag_val = col;
      if (n_lags == n_rows) {
        if (lag_val >= n_rows / 2) lag_val -= n_rows;
      } else {
        lag_val = col - (n_rows - 1);
      }
      int j = i + lag_val;
      if (j >= 0 && j < n_rows) out[i * n_rows + j] = lag[i * n_lags + col];
    }
  }
  return out;
}

std::vector<int> subsegment(const float* data, int rows, int cols,
                            const std::vector<int>& boundaries, int n_segments) {
  if (data == nullptr || rows <= 0 || cols <= 0 || n_segments <= 0) return boundaries;
  std::vector<int> sorted = boundaries;
  std::sort(sorted.begin(), sorted.end());
  if (sorted.empty() || sorted.front() != 0) sorted.insert(sorted.begin(), 0);
  if (sorted.back() != cols) sorted.push_back(cols);
  std::vector<int> out;
  out.reserve(sorted.size() * n_segments);
  for (size_t s = 0; s + 1 < sorted.size(); ++s) {
    int a = sorted[s];
    int b = sorted[s + 1];
    int len = b - a;
    int chunk = std::max(1, len / n_segments);
    for (int q = 0; q < n_segments; ++q) {
      int idx = a + q * chunk;
      if (idx < b && std::find(out.begin(), out.end(), idx) == out.end()) {
        out.push_back(idx);
      }
    }
  }
  if (std::find(out.begin(), out.end(), cols) == out.end()) out.push_back(cols);
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<int> agglomerative(const float* data, int rows, int cols, int k) {
  if (data == nullptr) throw std::invalid_argument("agglomerative: null input");
  if (k <= 0) throw std::invalid_argument("agglomerative: k must be positive");
  k = std::min(k, cols);
  std::vector<int> labels(cols, 0);
  for (int i = 0; i < cols; ++i) labels[i] = i;
  std::vector<std::vector<int>> clusters(cols);
  for (int i = 0; i < cols; ++i) clusters[i] = {i};

  auto distance = [&](const std::vector<int>& A, const std::vector<int>& B) {
    float sum = 0.0f;
    int n = 0;
    for (int a : A) {
      for (int b : B) {
        sum += euclidean_dist(data, rows, cols, data, cols, a, b);
        ++n;
      }
    }
    return (n > 0) ? sum / n : 0.0f;
  };

  while (static_cast<int>(clusters.size()) > k) {
    int best_a = 0, best_b = 1;
    float best_d = std::numeric_limits<float>::infinity();
    for (size_t a = 0; a < clusters.size(); ++a) {
      for (size_t b = a + 1; b < clusters.size(); ++b) {
        float d = distance(clusters[a], clusters[b]);
        if (d < best_d) {
          best_d = d;
          best_a = static_cast<int>(a);
          best_b = static_cast<int>(b);
        }
      }
    }
    // Merge best_b into best_a, remove best_b.
    clusters[best_a].insert(clusters[best_a].end(), clusters[best_b].begin(),
                            clusters[best_b].end());
    clusters.erase(clusters.begin() + best_b);
  }
  for (size_t c = 0; c < clusters.size(); ++c) {
    for (int idx : clusters[c]) labels[idx] = static_cast<int>(c);
  }
  return labels;
}

std::vector<float> path_enhance(const float* rec, int n, int win, int max_ratio, int min_ratio,
                                int n_filters) {
  if (rec == nullptr || n <= 0 || win <= 0) return {};
  (void)max_ratio;
  (void)min_ratio;
  (void)n_filters;
  // Gaussian smoothing along the main diagonal.
  std::vector<float> out(static_cast<size_t>(n) * n, 0.0f);
  int half = win / 2;
  // Pre-compute a 1D Gaussian kernel.
  std::vector<float> kernel(win);
  float sigma = std::max(1.0f, static_cast<float>(win) / 6.0f);
  float ksum = 0.0f;
  for (int i = 0; i < win; ++i) {
    float d = static_cast<float>(i - half);
    kernel[i] = std::exp(-0.5f * d * d / (sigma * sigma));
    ksum += kernel[i];
  }
  if (ksum > 0.0f) for (float& k : kernel) k /= ksum;
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      float acc = 0.0f;
      for (int q = 0; q < win; ++q) {
        int ii = i + q - half;
        int jj = j + q - half;
        if (ii >= 0 && ii < n && jj >= 0 && jj < n) {
          acc += kernel[q] * rec[ii * n + jj];
        }
      }
      out[i * n + j] = acc;
    }
  }
  return out;
}

}  // namespace sonare
