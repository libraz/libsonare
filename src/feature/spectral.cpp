#include "feature/spectral.h"

#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <numeric>

#include "util/exception.h"

namespace sonare {

namespace {

constexpr float kLog10Scale = 4.342944819032518f;

/// @brief Computes frequency for each FFT bin using Eigen.
Eigen::VectorXf bin_frequencies_eigen(int n_bins, int sr, int n_fft) {
  float bin_width = static_cast<float>(sr) / static_cast<float>(n_fft);
  return Eigen::VectorXf::LinSpaced(n_bins, 0.0f, (n_bins - 1) * bin_width);
}

/// @brief Computes frequency for each FFT bin.
std::vector<float> bin_frequencies(int n_bins, int sr, int n_fft) {
  std::vector<float> freqs(n_bins);
  float bin_width = static_cast<float>(sr) / static_cast<float>(n_fft);
  for (int i = 0; i < n_bins; ++i) {
    freqs[i] = static_cast<float>(i) * bin_width;
  }
  return freqs;
}

std::vector<float> pad_for_centered_frames(const float* samples, size_t n_samples, int frame_length) {
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

}  // namespace

std::vector<float> spectral_centroid(const Spectrogram& spec, int sr) {
  return spectral_centroid(spec.magnitude().data(), spec.n_bins(), spec.n_frames(), sr,
                           spec.n_fft());
}

std::vector<float> spectral_centroid(const float* magnitude, int n_bins, int n_frames, int sr,
                                     int n_fft) {
  SONARE_CHECK(magnitude != nullptr, ErrorCode::InvalidParameter);

  /// Create frequency vector
  Eigen::VectorXf freqs = bin_frequencies_eigen(n_bins, sr, n_fft);

  /// Map magnitude to Eigen matrix [n_bins x n_frames] (row-major)
  Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> mag_map(
      magnitude, n_bins, n_frames);

  /// Compute weighted sum: freqs^T * mag (produces row vector of length n_frames)
  Eigen::RowVectorXf sum_weighted = freqs.transpose() * mag_map;

  /// Compute magnitude sum per frame
  Eigen::RowVectorXf sum_mag = mag_map.colwise().sum();

  /// Compute centroid with division safety
  std::vector<float> centroid(n_frames);
  Eigen::Map<Eigen::RowVectorXf> result_map(centroid.data(), n_frames);
  result_map = sum_weighted.array() / (sum_mag.array() + 1e-10f);

  return centroid;
}

std::vector<float> spectral_bandwidth(const Spectrogram& spec, int sr, float p) {
  return spectral_bandwidth(spec.magnitude().data(), spec.n_bins(), spec.n_frames(), sr,
                            spec.n_fft(), p);
}

std::vector<float> spectral_bandwidth(const float* magnitude, int n_bins, int n_frames, int sr,
                                      int n_fft, float p) {
  SONARE_CHECK(magnitude != nullptr, ErrorCode::InvalidParameter);

  std::vector<float> freqs = bin_frequencies(n_bins, sr, n_fft);
  std::vector<float> centroids = spectral_centroid(magnitude, n_bins, n_frames, sr, n_fft);
  std::vector<float> bandwidth(n_frames);

  for (int t = 0; t < n_frames; ++t) {
    float centroid = centroids[t];
    float sum_weighted = 0.0f;
    float sum_magnitude = 0.0f;

    for (int k = 0; k < n_bins; ++k) {
      float mag = magnitude[k * n_frames + t];
      float diff = std::abs(freqs[k] - centroid);
      sum_weighted += std::pow(diff, p) * mag;
      sum_magnitude += mag;
    }

    if (sum_magnitude > 1e-10f) {
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
  SONARE_CHECK(roll_percent > 0.0f && roll_percent <= 1.0f, ErrorCode::InvalidParameter);

  std::vector<float> freqs = bin_frequencies(n_bins, sr, n_fft);
  std::vector<float> rolloff(n_frames);

  for (int t = 0; t < n_frames; ++t) {
    // Compute total energy
    float total_energy = 0.0f;
    for (int k = 0; k < n_bins; ++k) {
      float mag = magnitude[k * n_frames + t];
      total_energy += mag * mag;
    }

    float threshold = roll_percent * total_energy;
    float cumulative_energy = 0.0f;

    // Find rolloff frequency
    int rolloff_bin = n_bins - 1;
    for (int k = 0; k < n_bins; ++k) {
      float mag = magnitude[k * n_frames + t];
      cumulative_energy += mag * mag;
      if (cumulative_energy >= threshold) {
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

  constexpr float kAmin = 1e-10f;

  // Map magnitude to Eigen matrix [n_bins x n_frames] (row-major)
  Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> mag_map(
      magnitude, n_bins, n_frames);

  // Compute log sum per frame (for geometric mean)
  Eigen::RowVectorXf sum_log = mag_map.array().max(kAmin).log().colwise().sum();

  // Compute linear sum per frame (for arithmetic mean)
  Eigen::RowVectorXf sum_linear = mag_map.colwise().sum();

  // Compute flatness: geometric_mean / arithmetic_mean
  // geometric_mean = exp(sum_log / n_bins)
  // arithmetic_mean = sum_linear / n_bins
  // flatness = exp(sum_log / n_bins) / (sum_linear / n_bins + eps)
  //          = exp(sum_log / n_bins) * n_bins / (sum_linear + eps * n_bins)
  std::vector<float> flatness(n_frames);
  Eigen::Map<Eigen::RowVectorXf> result_map(flatness.data(), n_frames);

  float n_bins_f = static_cast<float>(n_bins);
  Eigen::ArrayXf geometric_mean = (sum_log.array() / n_bins_f).exp();
  Eigen::ArrayXf arithmetic_mean = sum_linear.array() / n_bins_f;

  result_map = geometric_mean / (arithmetic_mean + kAmin);

  return flatness;
}

std::vector<float> spectral_contrast(const Spectrogram& spec, int sr, int n_bands, float fmin,
                                     float quantile) {
  SONARE_CHECK(n_bands > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(quantile > 0.0f && quantile < 0.5f, ErrorCode::InvalidParameter);

  const std::vector<float>& magnitude = spec.magnitude();
  int n_bins = spec.n_bins();
  int n_frames = spec.n_frames();
  int n_fft = spec.n_fft();

  // Compute octave-band edges, including the low band from 0..fmin.
  std::vector<float> band_edges(n_bands + 2);
  float fmax = static_cast<float>(sr) / 2.0f;
  band_edges[0] = 0.0f;
  band_edges[1] = std::min(fmin, fmax);
  for (int i = 2; i <= n_bands + 1; ++i) {
    band_edges[i] = std::min(fmin * std::pow(2.0f, static_cast<float>(i - 1)), fmax);
  }

  // Convert frequency edges to bin indices
  float bin_width = static_cast<float>(sr) / static_cast<float>(n_fft);
  std::vector<int> band_bins(n_bands + 2);
  for (int i = 0; i <= n_bands + 1; ++i) {
    int edge_bin = static_cast<int>(std::floor(band_edges[i] / bin_width));
    band_bins[i] = std::max(0, std::min(edge_bin, n_bins));
  }

  // Output: [n_bands + 1 x n_frames]
  std::vector<float> contrast((n_bands + 1) * n_frames, 0.0f);

  for (int t = 0; t < n_frames; ++t) {
    for (int b = 0; b <= n_bands; ++b) {
      int start = band_bins[b];
      int end = band_bins[b + 1];

      if (start >= end) {
        continue;
      }

      // Collect magnitudes in this band
      std::vector<float> band_mags;
      band_mags.reserve(end - start);
      for (int k = start; k < end; ++k) {
        band_mags.push_back(magnitude[k * n_frames + t]);
      }

      if (band_mags.empty()) {
        continue;
      }

      // Sort to find quantiles
      std::sort(band_mags.begin(), band_mags.end());

      int q_index = static_cast<int>(quantile * static_cast<float>(band_mags.size()));
      q_index = std::max(1, std::min(q_index, static_cast<int>(band_mags.size()) - 1));

      float valley = band_mags[q_index - 1];
      float peak = band_mags[band_mags.size() - q_index];

      float amin = 1e-10f;
      contrast[b * n_frames + t] =
          kLog10Scale * std::log(std::max(peak, amin) / std::max(valley, amin));
    }
  }

  return contrast;
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

  int n_frames = 1 + static_cast<int>((padded_length - static_cast<size_t>(frame_length)) / hop_length);
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

  int n_frames = 1 + static_cast<int>((padded_length - static_cast<size_t>(frame_length)) / hop_length);
  if (n_frames <= 0) {
    n_frames = 1;
  }
  std::vector<float> rms(n_frames);

  for (int t = 0; t < n_frames; ++t) {
    size_t start = static_cast<size_t>(t) * hop_length;
    float sum_sq = 0.0f;

    for (int i = 0; i < frame_length; ++i) {
      float sample = padded_samples[start + i];
      sum_sq += sample * sample;
    }

    rms[t] = std::sqrt(sum_sq / static_cast<float>(frame_length));
  }

  return rms;
}

}  // namespace sonare
