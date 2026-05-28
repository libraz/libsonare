#include "feature/spectral.h"

#include <Eigen/Core>
#include <Eigen/SVD>
#include <algorithm>
#include <cmath>
#include <numeric>

#include "util/dsp_primitives.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare {

using sonare::constants::kEpsilon;

namespace {

/// @brief Computes frequency for each FFT bin.
std::vector<float> bin_frequencies(int n_bins, int sr, int n_fft) {
  std::vector<float> freqs(n_bins);
  float bin_width = static_cast<float>(sr) / static_cast<float>(n_fft);
  for (int i = 0; i < n_bins; ++i) {
    freqs[i] = static_cast<float>(i) * bin_width;
  }
  return freqs;
}

std::vector<float> pad_for_centered_frames(const float* samples, size_t n_samples,
                                           int frame_length) {
  int pad_length = frame_length / 2;
  std::vector<float> padded(n_samples + static_cast<size_t>(pad_length * 2), 0.0f);
  if (n_samples > 0) {
    std::copy(samples, samples + n_samples, padded.begin() + pad_length);
  }
  return padded;
}

std::vector<float> pad_for_centered_zcr(const float* samples, size_t n_samples, int frame_length) {
  int pad_length = frame_length / 2;
  std::vector<float> padded(n_samples + static_cast<size_t>(pad_length * 2), 0.0f);
  if (n_samples > 0) {
    std::fill(padded.begin(), padded.begin() + pad_length, samples[0]);
    std::copy(samples, samples + n_samples, padded.begin() + pad_length);
    std::fill(padded.begin() + pad_length + static_cast<int>(n_samples), padded.end(),
              samples[n_samples - 1]);
  }
  return padded;
}

float sanitized_magnitude(float magnitude) noexcept {
  return std::isfinite(magnitude) ? std::max(magnitude, 0.0f) : 0.0f;
}

}  // namespace

std::vector<float> spectral_centroid(const Spectrogram& spec, int sr) {
  return spectral_centroid(spec.magnitude().data(), spec.n_bins(), spec.n_frames(), sr,
                           spec.n_fft());
}

std::vector<float> spectral_centroid(const float* magnitude, int n_bins, int n_frames, int sr,
                                     int n_fft) {
  SONARE_CHECK(magnitude != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(n_bins > 0 && n_frames > 0 && sr > 0 && n_fft > 0, ErrorCode::InvalidParameter);

  std::vector<float> freqs = bin_frequencies(n_bins, sr, n_fft);
  std::vector<float> centroid(n_frames);

  for (int t = 0; t < n_frames; ++t) {
    float weighted_sum = 0.0f;
    float magnitude_sum = 0.0f;
    for (int k = 0; k < n_bins; ++k) {
      float mag = sanitized_magnitude(magnitude[k * n_frames + t]);
      weighted_sum += freqs[k] * mag;
      magnitude_sum += mag;
    }
    centroid[t] = magnitude_sum > 0.0f ? weighted_sum / magnitude_sum : 0.0f;
  }

  return centroid;
}

std::vector<float> spectral_bandwidth(const Spectrogram& spec, int sr, float p) {
  return spectral_bandwidth(spec.magnitude().data(), spec.n_bins(), spec.n_frames(), sr,
                            spec.n_fft(), p);
}

std::vector<float> spectral_bandwidth(const float* magnitude, int n_bins, int n_frames, int sr,
                                      int n_fft, float p) {
  SONARE_CHECK(magnitude != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(n_bins > 0 && n_frames > 0 && sr > 0 && n_fft > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(p > 0.0f, ErrorCode::InvalidParameter);

  std::vector<float> freqs = bin_frequencies(n_bins, sr, n_fft);
  std::vector<float> centroids = spectral_centroid(magnitude, n_bins, n_frames, sr, n_fft);
  std::vector<float> bandwidth(n_frames);

  for (int t = 0; t < n_frames; ++t) {
    float centroid = centroids[t];
    float sum_weighted = 0.0f;
    float sum_magnitude = 0.0f;

    for (int k = 0; k < n_bins; ++k) {
      float mag = sanitized_magnitude(magnitude[k * n_frames + t]);
      float diff = std::abs(freqs[k] - centroid);
      sum_weighted += std::pow(diff, p) * mag;
      sum_magnitude += mag;
    }

    if (sum_magnitude > 0.0f) {
      bandwidth[t] = std::pow(sum_weighted / sum_magnitude, 1.0f / p);
    } else {
      bandwidth[t] = 0.0f;
    }
  }

  return bandwidth;
}

std::vector<float> spectral_rolloff(const Spectrogram& spec, int sr, float roll_percent) {
  return spectral_rolloff(spec.magnitude().data(), spec.n_bins(), spec.n_frames(), sr, spec.n_fft(),
                          roll_percent);
}

std::vector<float> spectral_rolloff(const float* magnitude, int n_bins, int n_frames, int sr,
                                    int n_fft, float roll_percent) {
  SONARE_CHECK(magnitude != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(n_bins > 0 && n_frames > 0 && sr > 0 && n_fft > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(roll_percent > 0.0f && roll_percent < 1.0f, ErrorCode::InvalidParameter);

  std::vector<float> freqs = bin_frequencies(n_bins, sr, n_fft);
  std::vector<float> rolloff(n_frames);

  for (int t = 0; t < n_frames; ++t) {
    float total = 0.0f;
    for (int k = 0; k < n_bins; ++k) {
      float mag = sanitized_magnitude(magnitude[k * n_frames + t]);
      total += mag;
    }

    // librosa returns the lowest bin frequency (0 Hz) when the frame has no
    // energy. Without this short-circuit, ``threshold = 0`` would still match
    // bin 0 on the first iteration, but the explicit path documents the
    // empty-frame contract and avoids relying on cumulative >= 0.
    if (total <= 0.0f) {
      rolloff[t] = 0.0f;
      continue;
    }

    float threshold = roll_percent * total;
    float cumulative = 0.0f;

    int rolloff_bin = n_bins - 1;
    for (int k = 0; k < n_bins; ++k) {
      float mag = sanitized_magnitude(magnitude[k * n_frames + t]);
      cumulative += mag;
      if (cumulative >= threshold) {
        rolloff_bin = k;
        break;
      }
    }

    rolloff[t] = freqs[rolloff_bin];
  }

  return rolloff;
}

std::vector<float> spectral_flatness(const Spectrogram& spec) {
  return spectral_flatness(spec.magnitude().data(), spec.n_bins(), spec.n_frames());
}

std::vector<float> spectral_flatness(const float* magnitude, int n_bins, int n_frames) {
  SONARE_CHECK(magnitude != nullptr, ErrorCode::InvalidParameter);

  constexpr float kAmin = constants::kEpsilon;

  // Map magnitude to Eigen matrix [n_bins x n_frames] (row-major)
  Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> mag_map(
      magnitude, n_bins, n_frames);

  Eigen::ArrayXXf power = mag_map.array().square().max(kAmin);
  Eigen::RowVectorXf sum_log = power.log().colwise().sum();
  Eigen::RowVectorXf sum_linear = power.colwise().sum();

  std::vector<float> flatness(n_frames);
  Eigen::Map<Eigen::RowVectorXf> result_map(flatness.data(), n_frames);

  float n_bins_f = static_cast<float>(n_bins);
  Eigen::ArrayXf geometric_mean = (sum_log.array() / n_bins_f).exp();
  Eigen::ArrayXf arithmetic_mean = sum_linear.array() / n_bins_f;

  result_map = geometric_mean / arithmetic_mean;

  return flatness;
}

std::vector<float> spectral_contrast(const Spectrogram& spec, int sr, int n_bands, float fmin,
                                     float quantile) {
  SONARE_CHECK(n_bands > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(quantile > 0.0f && quantile < 1.0f, ErrorCode::InvalidParameter);
  SONARE_CHECK(fmin > 0.0f && sr > 0, ErrorCode::InvalidParameter);

  const std::vector<float>& magnitude = spec.magnitude();
  int n_bins = spec.n_bins();
  int n_frames = spec.n_frames();
  int n_fft = spec.n_fft();

  float nyquist = 0.5f * static_cast<float>(sr);
  SONARE_CHECK(fmin * std::pow(2.0f, static_cast<float>(n_bands - 1)) < nyquist,
               ErrorCode::InvalidParameter);

  std::vector<float> band_edges(n_bands + 2);
  band_edges[0] = 0.0f;
  for (int i = 1; i <= n_bands + 1; ++i) {
    band_edges[i] = fmin * std::pow(2.0f, static_cast<float>(i - 1));
  }

  std::vector<float> freqs = bin_frequencies(n_bins, sr, n_fft);
  std::vector<float> peak((n_bands + 1) * n_frames, 0.0f);
  std::vector<float> valley((n_bands + 1) * n_frames, 0.0f);

  for (int b = 0; b <= n_bands; ++b) {
    std::vector<int> band_indices;
    for (int k = 0; k < n_bins; ++k) {
      if (freqs[k] >= band_edges[b] && freqs[k] <= band_edges[b + 1]) {
        band_indices.push_back(k);
      }
    }

    SONARE_CHECK(!band_indices.empty(), ErrorCode::InvalidParameter);

    if (b > 0 && band_indices.front() > 0) {
      band_indices.insert(band_indices.begin(), band_indices.front() - 1);
    }
    if (b == n_bands) {
      int next_bin = band_indices.back() + 1;
      for (int k = next_bin; k < n_bins; ++k) {
        band_indices.push_back(k);
      }
    }

    int q_count = std::max(1, static_cast<int>(std::round(quantile * band_indices.size())));

    if (b < n_bands && !band_indices.empty()) {
      band_indices.pop_back();
    }
    q_count = std::min(q_count, static_cast<int>(band_indices.size()));

    for (int t = 0; t < n_frames; ++t) {
      std::vector<float> band_mags;
      band_mags.reserve(band_indices.size());
      for (int k : band_indices) {
        band_mags.push_back(magnitude[k * n_frames + t]);
      }

      std::sort(band_mags.begin(), band_mags.end());

      float valley_sum = 0.0f;
      float peak_sum = 0.0f;
      for (int i = 0; i < q_count; ++i) {
        valley_sum += band_mags[i];
        peak_sum += band_mags[band_mags.size() - static_cast<size_t>(q_count) + i];
      }
      valley[b * n_frames + t] = valley_sum / static_cast<float>(q_count);
      peak[b * n_frames + t] = peak_sum / static_cast<float>(q_count);
    }
  }

  std::vector<float> contrast((n_bands + 1) * n_frames, 0.0f);
  std::vector<float> peak_db(peak.size());
  std::vector<float> valley_db(valley.size());
  power_to_db(peak.data(), peak.size(), 1.0f, constants::kEpsilon, 80.0f, peak_db.data());
  power_to_db(valley.data(), valley.size(), 1.0f, constants::kEpsilon, 80.0f, valley_db.data());

  for (size_t i = 0; i < contrast.size(); ++i) {
    contrast[i] = peak_db[i] - valley_db[i];
  }

  return contrast;
}

std::vector<float> poly_features(const Spectrogram& spec, int sr, int order) {
  return poly_features(spec.magnitude().data(), spec.n_bins(), spec.n_frames(), sr, spec.n_fft(),
                       order);
}

std::vector<float> poly_features(const float* magnitude, int n_bins, int n_frames, int sr,
                                 int n_fft, int order) {
  SONARE_CHECK(magnitude != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(n_bins > 0 && n_frames > 0 && sr > 0 && n_fft > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(order >= 0, ErrorCode::InvalidParameter);

  // librosa.feature.poly_features computes np.polyfit(freqs, S[:, t], order).
  // Output is [order + 1, n_frames] with coefficients ordered highest-degree first.
  std::vector<float> freqs = bin_frequencies(n_bins, sr, n_fft);

  // Build Vandermonde matrix A [n_bins x (order + 1)] with columns
  // x^order, x^(order-1), ..., 1. With raw frequencies up to ~sr/2 (~11 kHz at
  // sr=22050), the high-degree columns can grow astronomically (11025^5 ~ 1.6e20),
  // wrecking the Jacobi SVD conditioning even in double precision. NumPy's
  // np.polyfit (which librosa wraps) sidesteps this by column-scaling the
  // Vandermonde matrix to unit max-norm before lstsq, then unscaling the
  // resulting coefficients. We mirror that here.
  Eigen::MatrixXd A(n_bins, order + 1);
  for (int k = 0; k < n_bins; ++k) {
    double x = static_cast<double>(freqs[k]);
    double v = 1.0;
    for (int p = order; p >= 0; --p) {
      A(k, p) = v;
      v *= x;
    }
  }

  // Column-norm scaling: divide each column by its max absolute value so each
  // column of the scaled matrix has max-norm 1. Columns that are identically
  // zero (only possible if all freqs[k] == 0, e.g. n_fft is degenerate) keep
  // a scale of 1 to avoid division by zero.
  Eigen::VectorXd scale(order + 1);
  for (int p = 0; p <= order; ++p) {
    const double col_max = A.col(p).cwiseAbs().maxCoeff();
    scale(p) = (col_max > 0.0) ? col_max : 1.0;
    A.col(p) /= scale(p);
  }

  // Solve A_scaled * c_scaled = y in least squares sense. Use SVD for numerical
  // robustness (matches numpy.linalg.lstsq used internally by np.polyfit).
  Eigen::JacobiSVD<Eigen::MatrixXd> svd(A, Eigen::ComputeThinU | Eigen::ComputeThinV);

  std::vector<float> out(static_cast<size_t>(order + 1) * static_cast<size_t>(n_frames), 0.0f);
  Eigen::VectorXd y(n_bins);
  for (int t = 0; t < n_frames; ++t) {
    for (int k = 0; k < n_bins; ++k) {
      y(k) = static_cast<double>(magnitude[k * n_frames + t]);
    }
    Eigen::VectorXd c_scaled = svd.solve(y);
    // Unscale: c[p] = c_scaled[p] / scale[p] so out is in the original units.
    for (int p = 0; p <= order; ++p) {
      out[p * n_frames + t] = static_cast<float>(c_scaled(p) / scale(p));
    }
  }
  return out;
}

std::vector<float> zero_crossing_rate(const Audio& audio, int frame_length, int hop_length) {
  return zero_crossing_rate(audio.data(), audio.size(), frame_length, hop_length);
}

std::vector<float> zero_crossing_rate(const float* samples, size_t n_samples, int frame_length,
                                      int hop_length) {
  SONARE_CHECK(samples != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(frame_length > 0 && hop_length > 0, ErrorCode::InvalidParameter);

  std::vector<float> padded = pad_for_centered_zcr(samples, n_samples, frame_length);
  const float* padded_samples = padded.data();
  size_t padded_length = padded.size();

  int n_frames =
      1 + static_cast<int>((padded_length - static_cast<size_t>(frame_length)) / hop_length);
  if (n_frames <= 0) {
    n_frames = 1;
  }
  std::vector<float> zcr(n_frames);

  for (int t = 0; t < n_frames; ++t) {
    size_t start = static_cast<size_t>(t) * hop_length;
    int crossings = 0;

    for (int i = 1; i < frame_length; ++i) {
      size_t idx = start + i;
      if ((padded_samples[idx] >= 0.0f) != (padded_samples[idx - 1] >= 0.0f)) {
        crossings++;
      }
    }

    zcr[t] = static_cast<float>(crossings) / static_cast<float>(frame_length);
  }

  return zcr;
}

std::vector<float> rms_energy(const Audio& audio, int frame_length, int hop_length) {
  return rms_energy(audio.data(), audio.size(), frame_length, hop_length);
}

std::vector<float> rms_energy(const float* samples, size_t n_samples, int frame_length,
                              int hop_length) {
  SONARE_CHECK(samples != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(frame_length > 0 && hop_length > 0, ErrorCode::InvalidParameter);

  std::vector<float> padded = pad_for_centered_frames(samples, n_samples, frame_length);
  const float* padded_samples = padded.data();
  size_t padded_length = padded.size();

  int n_frames =
      1 + static_cast<int>((padded_length - static_cast<size_t>(frame_length)) / hop_length);
  if (n_frames <= 0) {
    n_frames = 1;
  }
  std::vector<float> rms(n_frames);

  for (int t = 0; t < n_frames; ++t) {
    size_t start = static_cast<size_t>(t) * hop_length;
    rms[t] = sonare::rms(padded_samples + start, static_cast<size_t>(frame_length));
  }

  return rms;
}

std::vector<int> zero_crossings(const float* y, size_t n, float threshold, bool ref_magnitude,
                                bool pad, bool zero_pos) {
  if (n > 0 && y == nullptr) {
    throw std::invalid_argument("zero_crossings: null input with non-zero length");
  }
  if (!(threshold >= 0.0f)) {
    throw std::invalid_argument("zero_crossings: threshold must be non-negative");
  }

  std::vector<int> indices;
  if (n == 0) return indices;

  float effective_threshold = threshold;
  if (ref_magnitude) {
    float max_abs = 0.0f;
    for (size_t i = 0; i < n; ++i) {
      float a = std::abs(y[i]);
      if (a > max_abs) max_abs = a;
    }
    effective_threshold *= max_abs;
  }

  auto sample_sign = [&](float v) -> int {
    // Returns sign with the requested zero handling.
    if (v >= -effective_threshold && v <= effective_threshold) v = 0.0f;
    if (zero_pos) {
      // std::signbit semantics: negative -> 1, others (incl. +0) -> 0
      return std::signbit(v) ? -1 : +1;
    }
    if (v > 0.0f) return +1;
    if (v < 0.0f) return -1;
    return 0;
  };

  if (pad) {
    indices.push_back(0);
  }

  int prev_sign = sample_sign(y[0]);
  for (size_t i = 1; i < n; ++i) {
    int cur_sign = sample_sign(y[i]);
    if (cur_sign != prev_sign) {
      indices.push_back(static_cast<int>(i));
    }
    prev_sign = cur_sign;
  }
  return indices;
}

std::vector<int> zero_crossings(const std::vector<float>& y, float threshold, bool ref_magnitude,
                                bool pad, bool zero_pos) {
  return zero_crossings(y.data(), y.size(), threshold, ref_magnitude, pad, zero_pos);
}

}  // namespace sonare
