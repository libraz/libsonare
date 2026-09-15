#include "feature/spectral.h"

#include <Eigen/Core>
#include <Eigen/SVD>
#include <algorithm>
#include <cmath>
#include <numeric>

#include "util/dsp_primitives.h"
#include "util/exception.h"
#include "util/math_utils.h"
#include "util/numeric_validation.h"

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

  // Bin-major with per-frame accumulators, as spectral_flatness below: `magnitude` is
  // row-major [n_bins x n_frames], so a bin is one contiguous row. Each frame still sums
  // bins 0..n_bins-1 in order, so the accumulated value is unchanged bit for bit.
  std::vector<float> weighted_sum(static_cast<size_t>(n_frames), 0.0f);
  std::vector<float> magnitude_sum(static_cast<size_t>(n_frames), 0.0f);
  for (int k = 0; k < n_bins; ++k) {
    const float* row = magnitude + static_cast<size_t>(k) * static_cast<size_t>(n_frames);
    const float freq = freqs[static_cast<size_t>(k)];
    for (int t = 0; t < n_frames; ++t) {
      const float mag = sanitized_magnitude(row[t]);
      weighted_sum[static_cast<size_t>(t)] += freq * mag;
      magnitude_sum[static_cast<size_t>(t)] += mag;
    }
  }

  for (int t = 0; t < n_frames; ++t) {
    const size_t index = static_cast<size_t>(t);
    centroid[index] =
        magnitude_sum[index] > 0.0f ? weighted_sum[index] / magnitude_sum[index] : 0.0f;
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
  // An infinite p survives a bare p > 0 and is laundered into a finite lie:
  // pow(sum, 1/p) collapses to pow(x, 0) == 1, so every frame reads 1.0 Hz.
  SONARE_CHECK(numeric::finite_positive(p), ErrorCode::InvalidParameter);

  std::vector<float> freqs = bin_frequencies(n_bins, sr, n_fft);
  std::vector<float> centroids = spectral_centroid(magnitude, n_bins, n_frames, sr, n_fft);
  std::vector<float> bandwidth(n_frames);

  // Bin-major, per-frame accumulators; see spectral_centroid above for why the sums are
  // bit-identical to a frame-major walk.
  std::vector<float> sum_weighted(static_cast<size_t>(n_frames), 0.0f);
  std::vector<float> sum_magnitude(static_cast<size_t>(n_frames), 0.0f);
  for (int k = 0; k < n_bins; ++k) {
    const float* row = magnitude + static_cast<size_t>(k) * static_cast<size_t>(n_frames);
    const float freq = freqs[static_cast<size_t>(k)];
    for (int t = 0; t < n_frames; ++t) {
      const size_t index = static_cast<size_t>(t);
      const float mag = sanitized_magnitude(row[t]);
      const float diff = std::abs(freq - centroids[index]);
      sum_weighted[index] += std::pow(diff, p) * mag;
      sum_magnitude[index] += mag;
    }
  }

  for (int t = 0; t < n_frames; ++t) {
    const size_t index = static_cast<size_t>(t);
    if (sum_magnitude[index] > 0.0f) {
      bandwidth[index] = std::pow(sum_weighted[index] / sum_magnitude[index], 1.0f / p);
    } else {
      bandwidth[index] = 0.0f;
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

  // Two bin-major passes over the row-major [n_bins x n_frames] buffer, as spectral_centroid
  // above: one for the per-frame total, one to find the first bin whose running sum reaches
  // the threshold. Both sums still run bins 0..n_bins-1 per frame, so they are bit-identical
  // to the frame-major walk; the per-frame `break` becomes a latch on rolloff_bin, which
  // freezes on the same bin because sanitized magnitudes are non-negative and the running sum
  // therefore never falls back below the threshold.
  std::vector<float> total(static_cast<size_t>(n_frames), 0.0f);
  for (int k = 0; k < n_bins; ++k) {
    const float* row = magnitude + static_cast<size_t>(k) * static_cast<size_t>(n_frames);
    for (int t = 0; t < n_frames; ++t) {
      total[static_cast<size_t>(t)] += sanitized_magnitude(row[t]);
    }
  }

  std::vector<float> threshold(static_cast<size_t>(n_frames));
  for (int t = 0; t < n_frames; ++t) {
    threshold[static_cast<size_t>(t)] = roll_percent * total[static_cast<size_t>(t)];
  }

  std::vector<float> cumulative(static_cast<size_t>(n_frames), 0.0f);
  std::vector<int> rolloff_bin(static_cast<size_t>(n_frames), -1);
  for (int k = 0; k < n_bins; ++k) {
    const float* row = magnitude + static_cast<size_t>(k) * static_cast<size_t>(n_frames);
    for (int t = 0; t < n_frames; ++t) {
      const size_t index = static_cast<size_t>(t);
      if (rolloff_bin[index] >= 0) {
        continue;
      }
      cumulative[index] += sanitized_magnitude(row[t]);
      if (cumulative[index] >= threshold[index]) {
        rolloff_bin[index] = k;
      }
    }
  }

  for (int t = 0; t < n_frames; ++t) {
    const size_t index = static_cast<size_t>(t);
    // librosa returns the lowest bin frequency (0 Hz) when the frame has no energy. The
    // threshold is 0 for such a frame, so the latch above would fire on bin 0 anyway; the
    // explicit path documents the empty-frame contract rather than relying on that.
    if (total[index] <= 0.0f) {
      rolloff[index] = 0.0f;
      continue;
    }
    const int bin = rolloff_bin[index] < 0 ? n_bins - 1 : rolloff_bin[index];
    rolloff[index] = freqs[static_cast<size_t>(bin)];
  }

  return rolloff;
}

std::vector<float> spectral_flatness(const Spectrogram& spec) {
  return spectral_flatness(spec.magnitude().data(), spec.n_bins(), spec.n_frames());
}

std::vector<float> spectral_flatness(const float* magnitude, int n_bins, int n_frames) {
  SONARE_CHECK(magnitude != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(n_bins > 0 && n_frames > 0, ErrorCode::InvalidParameter);

  constexpr double kAmin = static_cast<double>(constants::kEpsilon);

  // Two per-frame double accumulators, walked bin-major. Two properties are
  // being bought here and neither is free elsewhere:
  //
  //   1. Sanitize before squaring, exactly as the sibling descriptors in this
  //      file do. A single non-finite magnitude otherwise drives both sums to
  //      Inf and their ratio to NaN, which escapes the documented [0, 1] range
  //      and then survives std::clamp downstream, because every comparison
  //      against NaN is false.
  //   2. Accumulate in double, so a finite but very large magnitude stays
  //      representable -- FLT_MAX squares to +Inf in float -- and the ratio is
  //      finite for every finite input.
  //
  // Accumulating per frame rather than materializing an [n_bins x n_frames]
  // double array matters for memory, not for speed: the array form doubled a
  // temporary that reaches ~32 MB for three minutes of audio at n_fft 2048,
  // against an analyze() peak that already sits near the WASM heap ceiling.
  // This form holds 2 * n_frames doubles instead. The loop is bin-major because
  // `magnitude` is row-major [n_bins x n_frames], so each row is contiguous.
  //
  // Floor the power at kAmin (librosa's amin) before the geometric/arithmetic
  // means. A fully-silent frame then floors to kAmin across every bin, so its
  // ratio is 1.0 (maximally flat) — matching librosa.feature.spectral_flatness,
  // which likewise applies the amin floor and does not special-case silence.
  std::vector<double> sum_log(static_cast<size_t>(n_frames), 0.0);
  std::vector<double> sum_linear(static_cast<size_t>(n_frames), 0.0);
  for (int bin = 0; bin < n_bins; ++bin) {
    const float* row = magnitude + static_cast<size_t>(bin) * static_cast<size_t>(n_frames);
    for (int frame = 0; frame < n_frames; ++frame) {
      const double value = static_cast<double>(row[frame]);
      const double sanitized = std::isfinite(value) ? std::max(value, 0.0) : 0.0;
      const double power = std::max(sanitized * sanitized, kAmin);
      sum_log[static_cast<size_t>(frame)] += std::log(power);
      sum_linear[static_cast<size_t>(frame)] += power;
    }
  }

  std::vector<float> flatness(static_cast<size_t>(n_frames));
  const double n_bins_d = static_cast<double>(n_bins);
  for (int frame = 0; frame < n_frames; ++frame) {
    const size_t index = static_cast<size_t>(frame);
    // sum_linear is bounded below by n_bins * kAmin, so the divisor is never 0.
    const double geometric_mean = std::exp(sum_log[index] / n_bins_d);
    const double arithmetic_mean = sum_linear[index] / n_bins_d;
    flatness[index] = static_cast<float>(geometric_mean / arithmetic_mean);
  }

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
  SONARE_CHECK(n_bins > 0 && n_frames > 0 && n_fft > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(!magnitude.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(magnitude.size() >= static_cast<size_t>(n_bins) * static_cast<size_t>(n_frames),
               ErrorCode::InvalidParameter);

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
  // Frame-tile staging buffer, reused across every band and tile; resized per band below.
  std::vector<float> tile;

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

    // librosa computes the quantile count against the FULL band (including the
    // overlap-extension bins) BEFORE trimming the last bin: in librosa
    // `idx = max(1, rint(quantile * np.sum(current_band)))` is evaluated before
    // `sub_band = sub_band[..., :-1, :]`. So q_count is derived from the full
    // band_indices.size() here, then only min-clamped to the trimmed size below.
    //
    // librosa evaluates this product in float64 with quantile = 0.02, so an exact
    // half-integer count (e.g. 0.02 * 75 = 1.5) rounds to the nearest even integer
    // (2) via numpy.rint. Our quantile arrives as float32 (0.02f = 0.0199999996),
    // which drags that product a hair below the tie (1.4999997) and would round
    // the wrong way. Strip the float32 quantization noise off the product before
    // rounding so half-integer counts match librosa; the snap grid (1e-4) is far
    // finer than the per-bin quantile step yet far coarser than the float32 error.
    double q_product = static_cast<double>(quantile) * static_cast<double>(band_indices.size());
    q_product = std::round(q_product * 1.0e4) / 1.0e4;
    int q_count = std::max(1, static_cast<int>(std::rint(q_product)));

    if (b < n_bands && !band_indices.empty()) {
      band_indices.pop_back();
    }
    q_count = std::min(q_count, static_cast<int>(band_indices.size()));

    // Stage the band bin-major in frame tiles instead of gathering a column per frame.
    // `magnitude` is row-major [n_bins x n_frames], so a per-frame gather touches one cache
    // line per bin; a tile reads along each bin row contiguously while bounding the staging
    // buffer to band_size * kFrameTile floats, so the cache win does not cost the
    // [n_bins x n_frames]-sized temporary spectral_flatness deliberately avoids. The sort
    // sees the same multiset per frame, so peak/valley are unchanged bit for bit.
    constexpr int kFrameTile = 256;
    const size_t band_size = band_indices.size();
    tile.resize(band_size * static_cast<size_t>(kFrameTile));

    for (int tile_start = 0; tile_start < n_frames; tile_start += kFrameTile) {
      const int tile_frames = std::min(kFrameTile, n_frames - tile_start);

      for (size_t i = 0; i < band_size; ++i) {
        const float* row = magnitude.data() +
                           static_cast<size_t>(band_indices[i]) * static_cast<size_t>(n_frames) +
                           static_cast<size_t>(tile_start);
        for (int t = 0; t < tile_frames; ++t) {
          // std::sort requires a strict weak ordering. NaN violates that contract, so treat
          // malformed/non-finite magnitudes as zero just as the other spectral descriptors
          // do before they enter an ordering operation.
          tile[static_cast<size_t>(t) * band_size + i] = sanitized_magnitude(row[t]);
        }
      }

      for (int t = 0; t < tile_frames; ++t) {
        const auto frame_begin =
            tile.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(t) * band_size);
        const auto frame_end = frame_begin + static_cast<std::ptrdiff_t>(band_size);
        std::sort(frame_begin, frame_end);

        float valley_sum = 0.0f;
        float peak_sum = 0.0f;
        for (int i = 0; i < q_count; ++i) {
          valley_sum += *(frame_begin + i);
          peak_sum += *(frame_end - q_count + i);
        }
        const size_t out = static_cast<size_t>(b) * static_cast<size_t>(n_frames) +
                           static_cast<size_t>(tile_start + t);
        valley[out] = valley_sum / static_cast<float>(q_count);
        peak[out] = peak_sum / static_cast<float>(q_count);
      }
    }
  }

  std::vector<float> contrast((n_bands + 1) * n_frames, 0.0f);
  std::vector<float> peak_db(peak.size());
  std::vector<float> valley_db(valley.size());
  power_to_db(peak.data(), peak.size(), 1.0f, constants::kEpsilon, constants::kDefaultTopDb,
              peak_db.data());
  power_to_db(valley.data(), valley.size(), 1.0f, constants::kEpsilon, constants::kDefaultTopDb,
              valley_db.data());

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

  // The solve needs a whole column, so this stages the gather bin-major in frame tiles as
  // spectral_contrast above does, rather than exchanging loops. `magnitude` is row-major
  // [n_bins x n_frames], so a per-frame gather touches one cache line per bin; a tile reads
  // along each bin row contiguously and bounds the staging buffer to n_bins * kFrameTile
  // floats. Staging moves data and computes nothing, so `y` holds the same values and the
  // solve is unchanged bit for bit.
  constexpr int kFrameTile = 256;
  std::vector<float> tile(static_cast<size_t>(n_bins) * static_cast<size_t>(kFrameTile));

  for (int tile_start = 0; tile_start < n_frames; tile_start += kFrameTile) {
    const int tile_frames = std::min(kFrameTile, n_frames - tile_start);

    for (int k = 0; k < n_bins; ++k) {
      const float* row =
          magnitude + static_cast<size_t>(k) * static_cast<size_t>(n_frames) + tile_start;
      for (int t = 0; t < tile_frames; ++t) {
        tile[static_cast<size_t>(t) * static_cast<size_t>(n_bins) + static_cast<size_t>(k)] =
            row[t];
      }
    }

    for (int t = 0; t < tile_frames; ++t) {
      const float* column = tile.data() + static_cast<size_t>(t) * static_cast<size_t>(n_bins);
      for (int k = 0; k < n_bins; ++k) {
        y(k) = static_cast<double>(column[k]);
      }
      Eigen::VectorXd c_scaled = svd.solve(y);
      // Unscale: c[p] = c_scaled[p] / scale[p] so out is in the original units.
      for (int p = 0; p <= order; ++p) {
        out[p * n_frames + tile_start + t] = static_cast<float>(c_scaled(p) / scale(p));
      }
    }
  }
  return out;
}

std::vector<float> zero_crossing_rate(const Audio& audio, int frame_length, int hop_length) {
  return zero_crossing_rate(audio.data(), audio.size(), frame_length, hop_length);
}

std::vector<float> zero_crossing_rate(const float* samples, size_t n_samples, int frame_length,
                                      int hop_length) {
  SONARE_CHECK(frame_length > 0 && hop_length > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(n_samples == 0 || samples != nullptr, ErrorCode::InvalidParameter);

  // Empty input: no zero crossings to count. Return a single zero-rate frame so
  // downstream callers always get a defined, non-empty result (and we never
  // index into the empty padded buffer below).
  if (n_samples == 0) {
    return std::vector<float>(1, 0.0f);
  }

  std::vector<float> padded = pad_for_centered_zcr(samples, n_samples, frame_length);
  const float* padded_samples = padded.data();
  size_t padded_length = padded.size();

  // Guard the unsigned subtraction below: if the padded signal is shorter than
  // one frame, fall back to a single frame instead of wrapping to a huge count.
  int n_frames = 1;
  if (padded_length >= static_cast<size_t>(frame_length)) {
    n_frames = 1 + static_cast<int>((padded_length - static_cast<size_t>(frame_length)) /
                                    static_cast<size_t>(hop_length));
  }
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
  SONARE_CHECK(n_samples == 0 || samples != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(frame_length > 0 && hop_length > 0, ErrorCode::InvalidParameter);

  if (n_samples == 0) {
    return std::vector<float>(1, 0.0f);
  }

  std::vector<float> padded = pad_for_centered_frames(samples, n_samples, frame_length);
  const float* padded_samples = padded.data();
  size_t padded_length = padded.size();

  int n_frames = 1;
  if (padded_length >= static_cast<size_t>(frame_length)) {
    n_frames = 1 + static_cast<int>((padded_length - static_cast<size_t>(frame_length)) /
                                    static_cast<size_t>(hop_length));
  }
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
    throw SonareException(ErrorCode::InvalidParameter,
                          "zero_crossings: null input with non-zero length");
  }
  // Finite, not merely non-negative: an infinite threshold zeroes every sample under
  // the sign test, and ref_magnitude scales it to NaN on silence, inverting that.
  if (!numeric::finite_non_negative(threshold)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "zero_crossings: threshold must be finite and non-negative");
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
