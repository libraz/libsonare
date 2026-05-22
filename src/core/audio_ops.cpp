/// @file audio_ops.cpp
/// @brief Implementation of time-domain audio operations.

#include "core/audio_ops.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <stdexcept>
#include <vector>

#include "core/fft.h"

namespace sonare {

std::vector<float> mu_compress(const float* x, std::size_t n, int mu, bool quantize) {
  if (n > 0 && x == nullptr) {
    throw std::invalid_argument("mu_compress: null input with non-zero length");
  }
  if (mu <= 0) {
    throw std::invalid_argument("mu_compress: mu must be strictly positive");
  }

  const float mu_f = static_cast<float>(mu);
  const float log1p_mu = std::log1p(mu_f);

  std::vector<float> y(n);
  for (std::size_t i = 0; i < n; ++i) {
    const float v = x[i];
    if (v < -1.0f || v > 1.0f) {
      throw std::invalid_argument("mu_compress: input out of range [-1, 1]");
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
    throw std::invalid_argument("mu_expand: null input with non-zero length");
  }
  if (mu <= 0) {
    throw std::invalid_argument("mu_expand: mu must be strictly positive");
  }

  std::vector<float> y(n);
  const float mu_f = static_cast<float>(mu);
  const float scale = 2.0f / (1.0f + mu_f);
  for (std::size_t i = 0; i < n; ++i) {
    float v = x[i];
    if (quantize) v *= scale;
    if (v < -1.0f || v > 1.0f) {
      throw std::invalid_argument("mu_expand: input out of range [-1, 1]");
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
    throw std::invalid_argument("autocorrelate: null input with non-zero length");
  }

  const std::size_t out_size =
      (max_size <= 0) ? n : std::min(static_cast<std::size_t>(max_size), n);
  std::vector<float> out(out_size, 0.0f);
  if (n == 0) return out;

  // For small inputs the direct O(n^2) path is faster than FFT setup overhead
  // and avoids power-of-two padding overhead.
  constexpr std::size_t kFftThreshold = 64;
  if (n < kFftThreshold) {
    for (std::size_t k = 0; k < out_size; ++k) {
      double acc = 0.0;
      for (std::size_t i = 0; i + k < n; ++i) {
        acc += static_cast<double>(y[i]) * static_cast<double>(y[i + k]);
      }
      out[k] = static_cast<float>(acc);
    }
    return out;
  }

  // FFT path. Zero-pad to next power of two of (2n - 1) so that the circular
  // autocorrelation of the padded signal equals the linear autocorrelation
  // restricted to non-negative lags.
  std::size_t n_pad = 1;
  while (n_pad < 2 * n - 1) n_pad *= 2;

  std::vector<float> y_padded(n_pad, 0.0f);
  std::copy(y, y + n, y_padded.begin());

  FFT fft(static_cast<int>(n_pad));
  const std::size_t n_bins = n_pad / 2 + 1;
  std::vector<std::complex<float>> Y(n_bins);
  fft.forward(y_padded.data(), Y.data());

  // Power spectrum |Y|^2 (real, kept as complex with zero imaginary).
  std::vector<std::complex<float>> psd(n_bins);
  for (std::size_t k = 0; k < n_bins; ++k) {
    const float re = Y[k].real();
    const float im = Y[k].imag();
    psd[k] = std::complex<float>(re * re + im * im, 0.0f);
  }

  // FFT::inverse applies a 1/n_pad scaling, which is exactly the normalisation
  // needed so that out[0] == sum_i y[i]^2 (Wiener-Khinchin / Parseval).
  std::vector<float> r(n_pad);
  fft.inverse(psd.data(), r.data());

  std::copy(r.begin(), r.begin() + out_size, out.begin());
  return out;
}

std::vector<float> autocorrelate(const std::vector<float>& y, int max_size) {
  return autocorrelate(y.data(), y.size(), max_size);
}

std::vector<float> lpc(const float* y, std::size_t n, int order) {
  if (order < 1) throw std::invalid_argument("lpc: order must be >= 1");
  if (n > 0 && y == nullptr) {
    throw std::invalid_argument("lpc: null input with non-zero length");
  }
  if (n < static_cast<std::size_t>(order + 1)) {
    throw std::invalid_argument("lpc: input length must exceed order");
  }

  // Burg's method - direct translation of librosa's __lpc.
  // Use double precision internally for stability.
  std::vector<double> y_d(n);
  for (std::size_t i = 0; i < n; ++i) y_d[i] = static_cast<double>(y[i]);

  std::vector<double> ar_coeffs(order + 1, 0.0);
  std::vector<double> ar_coeffs_prev(order + 1, 0.0);
  ar_coeffs[0] = 1.0;
  ar_coeffs_prev[0] = 1.0;

  // fwd_pred_error = y[1:], bwd_pred_error = y[:-1]
  std::vector<double> fwd(y_d.begin() + 1, y_d.end());
  std::vector<double> bwd(y_d.begin(), y_d.end() - 1);

  // den = sum(fwd^2 + bwd^2)
  double den = 0.0;
  for (std::size_t i = 0; i < fwd.size(); ++i) {
    den += fwd[i] * fwd[i] + bwd[i] * bwd[i];
  }

  // tiny / epsilon for stability (librosa uses util.tiny - the smallest normal).
  const double epsilon = 1e-300;

  for (int i = 0; i < order; ++i) {
    // reflect_coeff = -2 * sum(bwd * fwd) / (den + epsilon)
    double dot = 0.0;
    for (std::size_t k = 0; k < fwd.size(); ++k) {
      dot += bwd[k] * fwd[k];
    }
    double reflect_coeff = -2.0 * dot / (den + epsilon);

    // Swap ar_coeffs and ar_coeffs_prev
    std::swap(ar_coeffs, ar_coeffs_prev);
    ar_coeffs[0] = ar_coeffs_prev[0];

    // Update ar_coeffs: a_M[j] = a_{M-1}[j] + reflect_coeff * a_{M-1}[i - j + 1]
    for (int j = 1; j <= i + 1; ++j) {
      ar_coeffs[j] = ar_coeffs_prev[j] + reflect_coeff * ar_coeffs_prev[i - j + 1];
    }

    // Update forward and backward prediction errors.
    std::vector<double> fwd_tmp(fwd);
    for (std::size_t k = 0; k < fwd.size(); ++k) {
      fwd[k] = fwd_tmp[k] + reflect_coeff * bwd[k];
      bwd[k] = bwd[k] + reflect_coeff * fwd_tmp[k];
    }

    // Compute new den via recursion (eqn 17):
    //   q = 1 - reflect_coeff^2
    //   den = q * den - bwd[-1]^2 - fwd[0]^2
    if (!fwd.empty()) {
      const double q = 1.0 - reflect_coeff * reflect_coeff;
      den = q * den - bwd.back() * bwd.back() - fwd.front() * fwd.front();
    }

    // Shift up forward error / down backward error.
    if (!fwd.empty()) {
      fwd.erase(fwd.begin());
      bwd.pop_back();
    }
  }

  std::vector<float> result(order + 1);
  for (int i = 0; i <= order; ++i) {
    result[static_cast<std::size_t>(i)] = static_cast<float>(ar_coeffs[i]);
  }
  return result;
}

std::vector<float> lpc(const std::vector<float>& y, int order) {
  return lpc(y.data(), y.size(), order);
}

}  // namespace sonare
