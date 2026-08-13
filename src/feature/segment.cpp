#include "feature/segment.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <unordered_map>

#include "core/window.h"
#include "util/constants.h"
#include "util/exception.h"

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

namespace {

// Compute pairwise distance matrix [X_cols x Y_cols] using the given metric.
// "cosine" returns 1 - cos_sim (range [0, 2]); "euclidean" returns L2.
std::vector<float> pairwise_distance(const float* X, int X_rows, int X_cols, const float* Y,
                                     int Y_cols, const std::string& metric) {
  const bool use_cosine = (metric == "cosine");
  std::vector<float> D(static_cast<size_t>(X_cols) * Y_cols, 0.0f);
  for (int i = 0; i < X_cols; ++i) {
    for (int j = 0; j < Y_cols; ++j) {
      if (use_cosine) {
        D[i * Y_cols + j] = 1.0f - cosine_sim(X, X_rows, X_cols, Y, Y_cols, i, j);
      } else {
        D[i * Y_cols + j] = euclidean_dist(X, X_rows, X_cols, Y, Y_cols, i, j);
      }
    }
  }
  return D;
}

// Per-row affinity helpers: pick top-k smallest distances per row, optionally
// excluding self-pairs / width bands, then form `exp(-d / median(kth_dist))`
// and transpose. Mirrors librosa.segment.cross_similarity / recurrence_matrix
// with mode="affinity" and bandwidth="med_k_scalar".
std::vector<float> apply_affinity_kernel(const std::vector<float>& D, int n_rows, int n_cols, int k,
                                         bool exclude_self, int width) {
  // Gather candidates per row: (distance, column-index) excluding self / width band.
  std::vector<std::vector<std::pair<float, int>>> top_per_row(n_rows);
  std::vector<float> kth_dist;
  kth_dist.reserve(n_rows);
  for (int i = 0; i < n_rows; ++i) {
    std::vector<std::pair<float, int>> cands;
    cands.reserve(n_cols);
    for (int j = 0; j < n_cols; ++j) {
      if (exclude_self && i == j) continue;
      if (width > 0 && std::abs(i - j) < width) continue;
      cands.emplace_back(D[static_cast<size_t>(i) * n_cols + j], j);
    }
    const int take = std::min<int>(k, static_cast<int>(cands.size()));
    if (take <= 0) continue;
    std::partial_sort(cands.begin(), cands.begin() + take, cands.end());
    top_per_row[i].assign(cands.begin(), cands.begin() + take);
    kth_dist.push_back(top_per_row[i].back().first);
  }

  // bandwidth = median of (k-th smallest distance per row). librosa uses
  // nanmedian; rows with no neighbours are skipped here (matching the
  // "med_k_scalar" code path's nanmedian behaviour).
  float bw = 1.0f;
  if (!kth_dist.empty()) {
    std::sort(kth_dist.begin(), kth_dist.end());
    const size_t m = kth_dist.size();
    bw = (m % 2 == 1) ? kth_dist[m / 2] : 0.5f * (kth_dist[m / 2 - 1] + kth_dist[m / 2]);
    if (bw <= 0.0f) bw = 1.0f;  // degenerate: avoid div-by-zero (matches "self" pairs all 0)
  }

  // Build pre-transpose matrix.
  std::vector<float> pre(static_cast<size_t>(n_rows) * n_cols, 0.0f);
  for (int i = 0; i < n_rows; ++i) {
    for (const auto& dj : top_per_row[i]) {
      pre[static_cast<size_t>(i) * n_cols + dj.second] = std::exp(-dj.first / bw);
    }
  }

  // Transpose (librosa returns the transpose).
  std::vector<float> out(static_cast<size_t>(n_cols) * n_rows, 0.0f);
  for (int i = 0; i < n_rows; ++i) {
    for (int j = 0; j < n_cols; ++j) {
      out[static_cast<size_t>(j) * n_rows + i] = pre[static_cast<size_t>(i) * n_cols + j];
    }
  }
  return out;
}

}  // namespace

std::vector<float> cross_similarity(const float* X, int X_rows, int X_cols, const float* Y,
                                    int Y_rows, int Y_cols, int k, const std::string& metric,
                                    const std::string& mode) {
  if (X == nullptr || Y == nullptr)
    throw SonareException(ErrorCode::InvalidParameter, "cross_similarity: null input");
  if (X_rows != Y_rows)
    throw SonareException(ErrorCode::InvalidParameter, "cross_similarity: feature dims must match");
  if (mode != "connectivity" && mode != "affinity") {
    throw SonareException(ErrorCode::InvalidParameter,
                          "cross_similarity: mode must be 'connectivity' or 'affinity'");
  }
  if (X_cols <= 0 || Y_cols <= 0) return {};
  // The output is indexed with the int expression `i * Y_cols + j`; reject a size
  // whose element count would overflow int (UB) before iterating.
  if (static_cast<int64_t>(X_cols) * static_cast<int64_t>(Y_cols) >
      static_cast<int64_t>(std::numeric_limits<int>::max())) {
    throw SonareException(ErrorCode::InvalidParameter, "cross_similarity: matrix too large");
  }

  if (mode == "affinity") {
    // librosa default: k = min(n_ref, 2*ceil(sqrt(n_ref))).
    int kk = k;
    if (kk <= 0) {
      kk =
          std::min(Y_cols, static_cast<int>(2 * std::ceil(std::sqrt(static_cast<double>(Y_cols)))));
    }
    const bool exclude_self = (X == Y);
    // When self-similar, librosa's sklearn kNN search returns top-k INCLUDING
    // the self-pair (distance 0). After `eliminate_zeros` this leaves at most
    // (k-1) non-self entries per row, which is also what drives bandwidth.
    // Use kk-1 effective neighbours to match that behaviour.
    const int kk_eff = exclude_self ? std::max(0, kk - 1) : kk;
    auto D = pairwise_distance(X, X_rows, X_cols, Y, Y_cols, metric);
    return apply_affinity_kernel(D, X_cols, Y_cols, kk_eff, exclude_self, /*width=*/0);
  }

  // mode == "connectivity": raw similarity (cosine sim or -euclidean), top-k trimmed.
  std::vector<float> out(static_cast<size_t>(X_cols) * Y_cols, 0.0f);
  const bool use_cosine = (metric == "cosine");
  for (int i = 0; i < X_cols; ++i) {
    for (int j = 0; j < Y_cols; ++j) {
      float sim = use_cosine ? cosine_sim(X, X_rows, X_cols, Y, Y_cols, i, j)
                             : -euclidean_dist(X, X_rows, X_cols, Y, Y_cols, i, j);
      out[i * Y_cols + j] = sim;
    }
  }
  if (k > 0 && k < Y_cols) {
    std::vector<size_t> order(Y_cols);
    for (int i = 0; i < X_cols; ++i) {
      std::iota(order.begin(), order.end(), size_t{0});
      std::partial_sort(order.begin(), order.begin() + k, order.end(), [&](size_t a, size_t b) {
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
                                     bool sym, const std::string& metric, const std::string& mode) {
  if (mode != "connectivity" && mode != "affinity") {
    throw SonareException(ErrorCode::InvalidParameter,
                          "recurrence_matrix: mode must be 'connectivity' or 'affinity'");
  }
  if (data == nullptr)
    throw SonareException(ErrorCode::InvalidParameter, "recurrence_matrix: null input");
  if (cols <= 0) return {};
  // The output is indexed with the int expression `i * cols + j`; reject a size
  // whose element count would overflow int (UB) before iterating.
  if (static_cast<int64_t>(cols) * static_cast<int64_t>(cols) >
      static_cast<int64_t>(std::numeric_limits<int>::max())) {
    throw SonareException(ErrorCode::InvalidParameter, "recurrence_matrix: matrix too large");
  }
  if (width < 1) width = 1;

  if (mode == "affinity") {
    int kk = k;
    if (kk <= 0) {
      const int eff = std::max(1, cols - 2 * width + 1);
      kk = std::min(cols - 1, 2 * static_cast<int>(std::ceil(std::sqrt(static_cast<double>(eff)))));
    }
    auto D = pairwise_distance(data, rows, cols, data, cols, metric);
    auto out = apply_affinity_kernel(D, cols, cols, kk, /*exclude_self=*/true, width);
    if (sym) {
      for (int i = 0; i < cols; ++i) {
        for (int j = i + 1; j < cols; ++j) {
          const float a = out[i * cols + j];
          const float b = out[j * cols + i];
          const float v = std::min(a, b);
          out[i * cols + j] = v;
          out[j * cols + i] = v;
        }
      }
    }
    return out;
  }

  std::vector<float> out = cross_similarity(data, rows, cols, data, rows, cols, 0, metric);
  // Zero out the central diagonal band.
  for (int i = 0; i < cols; ++i) {
    for (int j = 0; j < cols; ++j) {
      if (std::abs(i - j) < width) out[i * cols + j] = 0.0f;
    }
  }
  if (k > 0 && k < cols) {
    std::vector<size_t> order(cols);
    for (int i = 0; i < cols; ++i) {
      std::iota(order.begin(), order.end(), size_t{0});
      std::partial_sort(order.begin(), order.begin() + k, order.end(),
                        [&](size_t a, size_t b) { return out[i * cols + a] > out[i * cols + b]; });
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
  // The output is indexed with the int expression `i * n_lags + col`; reject a
  // size whose element count would overflow int (UB) before iterating.
  if (static_cast<int64_t>(n) * static_cast<int64_t>(n_lags) >
      static_cast<int64_t>(std::numeric_limits<int>::max())) {
    throw SonareException(ErrorCode::InvalidParameter, "recurrence_to_lag: matrix too large");
  }
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

namespace {

/// @brief Ward clustering of consecutive columns under a time-contiguity constraint.
/// @details librosa.segment.subsegment clusters each parent span with a chain
///          connectivity graph (sklearn's grid_to_graph), so only temporally
///          adjacent clusters may merge. Two properties follow, and both are
///          load-bearing for a boundary detector: the span is cut into exactly
///          @p k runs, and every run is a contiguous stretch of time.
///          Unconstrained clustering gives neither -- one label may recur after
///          an interruption, which produces more than @p k - 1 label
///          transitions and "segments" that do not describe a span.
///
///          Merges the adjacent pair with the smallest Ward cost
///          `(na * nb / (na + nb)) * ||ca - cb||^2` until @p k runs remain,
///          tracking the active runs as a linked list so a merge does not have
///          to shift the remaining centroids.
/// @param block Column-major-indexed feature block [rows x len] row-major
/// @param rows Feature dimension
/// @param len Number of columns in the span
/// @param k Requested run count; clamped to [1, len]
/// @return Offsets relative to the span start where each run begins, ascending.
///         The first entry is always 0 and the result holds min(k, len) entries.
std::vector<int> contiguous_ward_runs(const float* block, int rows, int len, int k) {
  if (len <= 0 || rows <= 0) return {};
  k = std::clamp(k, 1, len);

  std::vector<int> start(len);
  std::vector<int> count(len, 1);
  std::vector<int> next(len);
  std::vector<int> prev(len);
  std::vector<double> centroid(static_cast<size_t>(len) * static_cast<size_t>(rows));
  for (int c = 0; c < len; ++c) {
    start[c] = c;
    next[c] = (c + 1 < len) ? c + 1 : -1;
    prev[c] = c - 1;
    for (int r = 0; r < rows; ++r) {
      centroid[static_cast<size_t>(c) * rows + r] = block[static_cast<size_t>(r) * len + c];
    }
  }

  auto ward_cost = [&](int i, int j) {
    double squared = 0.0;
    for (int r = 0; r < rows; ++r) {
      const double diff =
          centroid[static_cast<size_t>(i) * rows + r] - centroid[static_cast<size_t>(j) * rows + r];
      squared += diff * diff;
    }
    const auto ni = static_cast<double>(count[i]);
    const auto nj = static_cast<double>(count[j]);
    return (ni * nj / (ni + nj)) * squared;
  };

  int head = 0;
  int n_runs = len;
  while (n_runs > k) {
    int best = -1;
    double best_cost = 0.0;
    for (int i = head; i != -1 && next[i] != -1; i = next[i]) {
      const double cost = ward_cost(i, next[i]);
      if (best < 0 || cost < best_cost) {
        best = i;
        best_cost = cost;
      }
    }
    if (best < 0) break;

    const int victim = next[best];
    const auto ni = static_cast<double>(count[best]);
    const auto nj = static_cast<double>(count[victim]);
    for (int r = 0; r < rows; ++r) {
      centroid[static_cast<size_t>(best) * rows + r] =
          (ni * centroid[static_cast<size_t>(best) * rows + r] +
           nj * centroid[static_cast<size_t>(victim) * rows + r]) /
          (ni + nj);
    }
    count[best] += count[victim];
    next[best] = next[victim];
    if (next[victim] != -1) prev[next[victim]] = best;
    --n_runs;
  }

  std::vector<int> runs;
  runs.reserve(static_cast<size_t>(n_runs));
  for (int i = head; i != -1; i = next[i]) runs.push_back(start[i]);
  return runs;
}

}  // namespace

std::vector<int> subsegment(const float* data, int rows, int cols,
                            const std::vector<int>& boundaries, int n_segments) {
  if (data == nullptr || rows <= 0 || cols <= 0 || n_segments <= 0) return boundaries;

  // Normalize the parent boundaries: clamp into [0, cols], add the 0 and cols
  // endpoints, and drop duplicates so each [a, b) parent span is well-formed.
  std::vector<int> sorted;
  sorted.reserve(boundaries.size() + 2);
  for (int frame : boundaries) {
    if (frame >= 0 && frame <= cols) sorted.push_back(frame);
  }
  std::sort(sorted.begin(), sorted.end());
  if (sorted.empty() || sorted.front() != 0) sorted.insert(sorted.begin(), 0);
  if (sorted.back() != cols) sorted.push_back(cols);
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

  // Refine each parent span by clustering its column feature vectors under a
  // time-contiguity constraint and emitting a sub-boundary at each run start,
  // mirroring librosa.segment.subsegment. The constraint is what bounds the
  // output: every parent span contributes exactly min(n_segments, len)
  // boundaries, and each one opens a contiguous stretch of time. The
  // unconstrained agglomerative helper cannot provide either guarantee, so it
  // is deliberately not reused here.
  std::vector<int> out;
  std::vector<float> block;
  for (size_t s = 0; s + 1 < sorted.size(); ++s) {
    const int a = sorted[s];
    const int b = sorted[s + 1];
    const int len = b - a;
    if (len <= 0) continue;
    out.push_back(a);  // the parent boundary is always retained

    const int n_seg = std::min(n_segments, len);
    if (n_seg < 2 || len < 2) continue;  // nothing to subdivide

    // Extract the row-major rows x len column block for [a, b).
    block.assign(static_cast<size_t>(rows) * static_cast<size_t>(len), 0.0f);
    for (int r = 0; r < rows; ++r) {
      for (int c = 0; c < len; ++c) {
        block[static_cast<size_t>(r) * len + c] = data[static_cast<size_t>(r) * cols + (a + c)];
      }
    }

    const std::vector<int> runs = contiguous_ward_runs(block.data(), rows, len, n_seg);
    // runs.front() is the span start, which was already emitted as the parent
    // boundary above.
    for (size_t i = 1; i < runs.size(); ++i) out.push_back(a + runs[i]);
  }
  out.push_back(cols);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<int> agglomerative(const float* data, int rows, int cols, int k,
                               const std::string& linkage) {
  if (data == nullptr)
    throw SonareException(ErrorCode::InvalidParameter, "agglomerative: null input");
  if (k <= 0)
    throw SonareException(ErrorCode::InvalidParameter, "agglomerative: k must be positive");
  if (linkage != "average" && linkage != "single" && linkage != "complete" && linkage != "ward") {
    throw SonareException(ErrorCode::InvalidParameter,
                          "agglomerative: linkage must be average/single/complete/ward");
  }
  k = std::min(k, cols);
  if (cols <= 0) return {};
  if (k >= cols) {
    std::vector<int> labels(cols);
    for (int i = 0; i < cols; ++i) labels[i] = i;
    return labels;
  }

  // Pairwise squared-euclidean distance matrix. We track an `active` flag so we
  // can mark merged clusters as removed in-place rather than reshuffling.
  std::vector<float> D(static_cast<size_t>(cols) * cols, 0.0f);
  for (int i = 0; i < cols; ++i) {
    for (int j = i + 1; j < cols; ++j) {
      float s = 0.0f;
      for (int r = 0; r < rows; ++r) {
        const float diff = data[r * cols + i] - data[r * cols + j];
        s += diff * diff;
      }
      D[i * cols + j] = s;
      D[j * cols + i] = s;
    }
  }

  std::vector<char> active(cols, 1);
  std::vector<int> size(cols, 1);
  std::vector<int> parent(cols);
  for (int i = 0; i < cols; ++i) parent[i] = i;

  int n_clusters = cols;
  while (n_clusters > k) {
    // Find the closest pair of active clusters.
    int best_a = -1, best_b = -1;
    float best_d = std::numeric_limits<float>::infinity();
    for (int a = 0; a < cols; ++a) {
      if (!active[a]) continue;
      for (int b = a + 1; b < cols; ++b) {
        if (!active[b]) continue;
        const float d = D[a * cols + b];
        if (d < best_d) {
          best_d = d;
          best_a = a;
          best_b = b;
        }
      }
    }
    if (best_a < 0) break;

    // Lance-Williams update of D(best_a, *) and deactivate best_b.
    const int sa = size[best_a];
    const int sb = size[best_b];
    for (int c = 0; c < cols; ++c) {
      if (!active[c] || c == best_a || c == best_b) continue;
      const float d_ac = D[best_a * cols + c];
      const float d_bc = D[best_b * cols + c];
      const float d_ab = D[best_a * cols + best_b];
      float d_new = 0.0f;
      if (linkage == "single") {
        d_new = std::min(d_ac, d_bc);
      } else if (linkage == "complete") {
        d_new = std::max(d_ac, d_bc);
      } else if (linkage == "ward") {
        const int sc = size[c];
        const float total = static_cast<float>(sa + sb + sc);
        d_new = ((sa + sc) * d_ac + (sb + sc) * d_bc - sc * d_ab) / total;
      } else {  // "average"
        d_new = (sa * d_ac + sb * d_bc) / static_cast<float>(sa + sb);
      }
      D[best_a * cols + c] = d_new;
      D[c * cols + best_a] = d_new;
    }
    size[best_a] = sa + sb;
    active[best_b] = 0;
    // Union-find: point best_b's tree root at best_a.
    parent[best_b] = best_a;
    --n_clusters;
  }

  // Final labels: walk parent pointers to a root, then renumber roots to [0, k).
  auto root = [&](int x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  };
  std::vector<int> labels(cols, 0);
  std::unordered_map<int, int> root_to_label;
  int next_label = 0;
  for (int i = 0; i < cols; ++i) {
    const int r = root(i);
    auto it = root_to_label.find(r);
    if (it == root_to_label.end()) {
      root_to_label[r] = next_label;
      labels[i] = next_label++;
    } else {
      labels[i] = it->second;
    }
  }
  return labels;
}

std::vector<float> path_enhance(const float* rec, int n, int win, int max_ratio, int min_ratio,
                                int n_filters) {
  if (rec == nullptr || n <= 0 || win <= 0) return {};
  const int half = win / 2;

  // 1D smoothing window applied along each diagonal direction. librosa's
  // path_enhance uses a normalized Hann window along the diagonal, so we use the
  // shared periodic-Hann window (sym=False, matching get_window(fftbins=True))
  // and normalize by its sum.
  std::vector<float> kernel = create_window(WindowType::Hann, win, /*periodic=*/true);
  {
    float ksum = 0.0f;
    for (float k : kernel) ksum += k;
    if (ksum > 0.0f)
      for (float& k : kernel) k /= ksum;
  }

  // Tempo ratios (diagonal slopes), geometrically spaced in [min_ratio, max_ratio].
  // librosa defaults min_ratio to 1/max_ratio when unset (min_ratio <= 0 here).
  const int n_f = std::max(1, n_filters);
  std::vector<float> ratios;
  ratios.reserve(n_f);
  if (n_f == 1) {
    // Single filter: pure main diagonal (matches the original implementation).
    ratios.push_back(1.0f);
  } else {
    float lo = static_cast<float>(min_ratio);
    float hi = static_cast<float>(max_ratio);
    if (hi <= 0.0f) hi = 2.0f;
    if (lo <= 0.0f) lo = 1.0f / hi;
    if (lo > hi) std::swap(lo, hi);
    const float log_lo = std::log(lo);
    const float log_hi = std::log(hi);
    for (int k = 0; k < n_f; ++k) {
      const float t = static_cast<float>(k) / static_cast<float>(n_f - 1);
      ratios.push_back(std::exp(log_lo + t * (log_hi - log_lo)));
    }
  }

  // For each ratio, smooth along a sheared diagonal (slope = ratio) and take the
  // elementwise maximum across all filtered versions (multi-angle enhancement).
  // The reduction direction follows the longer axis so r == 1 reduces exactly to
  // the original main-diagonal smoothing.
  std::vector<float> out(static_cast<size_t>(n) * n, 0.0f);
  for (size_t f = 0; f < ratios.size(); ++f) {
    const float r = ratios[f];
    for (int i = 0; i < n; ++i) {
      for (int j = 0; j < n; ++j) {
        float acc = 0.0f;
        for (int q = 0; q < win; ++q) {
          const float s = static_cast<float>(q - half);
          int di;
          int dj;
          if (r >= 1.0f) {
            di = q - half;
            dj = static_cast<int>(std::lround(s * r));
          } else {
            di = static_cast<int>(std::lround(s / r));
            dj = q - half;
          }
          const int ii = i + di;
          const int jj = j + dj;
          if (ii >= 0 && ii < n && jj >= 0 && jj < n) {
            acc += kernel[q] * rec[ii * n + jj];
          }
        }
        const size_t idx = static_cast<size_t>(i) * n + j;
        out[idx] = (f == 0) ? acc : std::max(out[idx], acc);
      }
    }
  }
  return out;
}

}  // namespace sonare
