/// @file audio_ops.cpp
/// @brief Implementation of time-domain audio operations.

#include "core/audio_ops.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "util/exception.h"
#include "util/lpc.h"
#include "util/math_utils.h"

namespace sonare {

std::vector<float> mu_compress(const float* x, std::size_t n, int mu, bool quantize) {
  if (n > 0 && x == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "mu_compress: null input with non-zero length");
  }
  if (mu <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "mu_compress: mu must be strictly positive");
  }

  const float mu_f = static_cast<float>(mu);
  const float log1p_mu = std::log1p(mu_f);

  std::vector<float> y(n);
  for (std::size_t i = 0; i < n; ++i) {
    const float v = x[i];
    if (v < -1.0f || v > 1.0f) {
      throw SonareException(ErrorCode::InvalidParameter, "mu_compress: input out of range [-1, 1]");
    }
    const float s = (v > 0.0f) ? 1.0f : (v < 0.0f) ? -1.0f : 0.0f;
    y[i] = s * std::log1p(mu_f * std::abs(v)) / log1p_mu;
  }

  if (!quantize) return y;

  // librosa: np.digitize(x, np.linspace(-1, 1, num=1+mu, endpoint=True), right=True) - (mu+1)//2
  // linspace(-1, 1, num=1+mu) has mu+1 points, step = 2/mu.
  // np.digitize right=True returns the index of the first bin edge strictly
  // greater than x (using inclusive right boundaries). With monotonically
  // increasing edges, this is equivalent to lower_bound on edges with x.
  const int n_edges = 1 + mu;
  std::vector<float> edges(static_cast<std::size_t>(n_edges));
  for (int k = 0; k < n_edges; ++k) {
    edges[static_cast<std::size_t>(k)] =
        -1.0f + 2.0f * static_cast<float>(k) / static_cast<float>(mu);
  }
  const int offset = (mu + 1) / 2;
  for (std::size_t i = 0; i < n; ++i) {
    auto it = std::lower_bound(edges.begin(), edges.end(), y[i]);
    int idx = static_cast<int>(std::distance(edges.begin(), it));
    y[i] = static_cast<float>(idx - offset);
  }
  return y;
}

std::vector<float> mu_compress(const std::vector<float>& x, int mu, bool quantize) {
  return mu_compress(x.data(), x.size(), mu, quantize);
}

std::vector<float> mu_expand(const float* x, std::size_t n, int mu, bool quantize) {
  if (n > 0 && x == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "mu_expand: null input with non-zero length");
  }
  if (mu <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "mu_expand: mu must be strictly positive");
  }

  std::vector<float> y(n);
  const float mu_f = static_cast<float>(mu);
  const float scale = 2.0f / (1.0f + mu_f);
  for (std::size_t i = 0; i < n; ++i) {
    float v = x[i];
    if (quantize) v *= scale;
    if (v < -1.0f || v > 1.0f) {
      throw SonareException(ErrorCode::InvalidParameter, "mu_expand: input out of range [-1, 1]");
    }
    const float s = (v > 0.0f) ? 1.0f : (v < 0.0f) ? -1.0f : 0.0f;
    y[i] = s / mu_f * (std::pow(1.0f + mu_f, std::abs(v)) - 1.0f);
  }
  return y;
}

std::vector<float> mu_expand(const std::vector<float>& x, int mu, bool quantize) {
  return mu_expand(x.data(), x.size(), mu, quantize);
}

std::vector<float> autocorrelate(const float* y, std::size_t n, int max_size) {
  if (n > 0 && y == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "autocorrelate: null input with non-zero length");
  }

  const std::size_t out_size =
      (max_size <= 0) ? n : std::min(static_cast<std::size_t>(max_size), n);
  return unnormalized_autocorrelation(y, n, out_size);
}

std::vector<float> autocorrelate(const std::vector<float>& y, int max_size) {
  return autocorrelate(y.data(), y.size(), max_size);
}

std::vector<float> lpc(const float* y, std::size_t n, int order) {
  if (order < 1) throw SonareException(ErrorCode::InvalidParameter, "lpc: order must be >= 1");
  if (n > 0 && y == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "lpc: null input with non-zero length");
  }
  if (n < static_cast<std::size_t>(order + 1)) {
    throw SonareException(ErrorCode::InvalidParameter, "lpc: input length must exceed order");
  }

  return lpc_burg(y, n, order).ar;
}

std::vector<float> lpc(const std::vector<float>& y, int order) {
  return lpc(y.data(), y.size(), order);
}

}  // namespace sonare
