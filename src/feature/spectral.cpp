#include "feature/spectral.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "util/exception.h"

namespace sonare {

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

}  // namespace

std::vector<float> spectral_centroid(const Spectrogram& spec, int sr) {
  return spectral_centroid(spec.magnitude().data(), spec.n_bins(), spec.n_frames(), sr,
                           spec.n_fft());
}

std::vector<float> spectral_centroid(const float* magnitude, int n_bins, int n_frames, int sr,
                                     int n_fft) {
  SONARE_CHECK(magnitude != nullptr, ErrorCode::InvalidParameter);

  std::vector<float> freqs = bin_frequencies(n_bins, sr, n_fft);
  std::vector<float> centroid(n_frames);

  for (int t = 0; t < n_frames; ++t) {
    float sum_weighted = 0.0f;
    float sum_magnitude = 0.0f;

    for (int k = 0; k < n_bins; ++k) {
      float mag = magnitude[k * n_frames + t];
      sum_weighted += freqs[k] * mag;
      sum_magnitude += mag;
    }

    if (sum_magnitude > 1e-10f) {
      centroid[t] = sum_weighted / sum_magnitude;
    } else {
      centroid[t] = 0.0f;
    }
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

  std::vector<float> flatness(n_frames);
  float amin = 1e-10f;

  for (int t = 0; t < n_frames; ++t) {
    float sum_log = 0.0f;
    float sum_linear = 0.0f;

    for (int k = 0; k < n_bins; ++k) {
      float mag = std::max(magnitude[k * n_frames + t], amin);
      sum_log += std::log(mag);
      sum_linear += mag;
    }

    // Geometric mean = exp(mean(log(x)))
    float geometric_mean = std::exp(sum_log / static_cast<float>(n_bins));
    // Arithmetic mean
    float arithmetic_mean = sum_linear / static_cast<float>(n_bins);

    if (arithmetic_mean > amin) {
      flatness[t] = geometric_mean / arithmetic_mean;
    } else {
      flatness[t] = 0.0f;
    }
  }

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

  // Compute frequency bounds for each band (octave bands)
  std::vector<float> band_edges(n_bands + 2);
  float fmax = static_cast<float>(sr) / 2.0f;
  band_edges[0] = fmin;
  band_edges[n_bands + 1] = fmax;

  for (int i = 1; i <= n_bands; ++i) {
    band_edges[i] = fmin * std::pow(2.0f, static_cast<float>(i));
    if (band_edges[i] > fmax) {
      band_edges[i] = fmax;
    }
  }

  // Convert frequency edges to bin indices
  float bin_width = static_cast<float>(sr) / static_cast<float>(n_fft);
  std::vector<int> band_bins(n_bands + 2);
  for (int i = 0; i <= n_bands + 1; ++i) {
    band_bins[i] = static_cast<int>(band_edges[i] / bin_width);
    band_bins[i] = std::min(band_bins[i], n_bins - 1);
  }

  // Output: [n_bands + 1 x n_frames] (includes residual)
  std::vector<float> contrast((n_bands + 1) * n_frames, 0.0f);

  for (int t = 0; t < n_frames; ++t) {
    for (int b = 0; b < n_bands; ++b) {
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

      int q_idx = static_cast<int>(quantile * static_cast<float>(band_mags.size()));
      q_idx = std::max(0, std::min(q_idx, static_cast<int>(band_mags.size()) - 1));

      float valley = band_mags[q_idx];
      float peak = band_mags[band_mags.size() - 1 - q_idx];

      // Contrast is log ratio of peak to valley
      float amin = 1e-10f;
      contrast[b * n_frames + t] =
          std::log(std::max(peak, amin)) - std::log(std::max(valley, amin));
    }

    // Residual: overall spectral mean
    float mean_mag = 0.0f;
    for (int k = 0; k < n_bins; ++k) {
      mean_mag += magnitude[k * n_frames + t];
    }
    mean_mag /= static_cast<float>(n_bins);
    contrast[n_bands * n_frames + t] = std::log(std::max(mean_mag, 1e-10f));
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

  if (n_samples < static_cast<size_t>(frame_length)) {
    return std::vector<float>();
  }

  int n_frames = static_cast<int>((n_samples - frame_length) / hop_length) + 1;
  std::vector<float> zcr(n_frames);

  for (int t = 0; t < n_frames; ++t) {
    size_t start = static_cast<size_t>(t) * hop_length;
    int crossings = 0;

    for (int i = 1; i < frame_length; ++i) {
      size_t idx = start + i;
      if (idx >= n_samples) break;

      // Check if sign changed
      if ((samples[idx] >= 0.0f) != (samples[idx - 1] >= 0.0f)) {
        crossings++;
      }
    }

    zcr[t] = static_cast<float>(crossings) / static_cast<float>(frame_length - 1);
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

  if (n_samples < static_cast<size_t>(frame_length)) {
    return std::vector<float>();
  }

  int n_frames = static_cast<int>((n_samples - frame_length) / hop_length) + 1;
  std::vector<float> rms(n_frames);

  for (int t = 0; t < n_frames; ++t) {
    size_t start = static_cast<size_t>(t) * hop_length;
    float sum_sq = 0.0f;
    int count = 0;

    for (int i = 0; i < frame_length; ++i) {
      size_t idx = start + i;
      if (idx >= n_samples) break;

      sum_sq += samples[idx] * samples[idx];
      count++;
    }

    if (count > 0) {
      rms[t] = std::sqrt(sum_sq / static_cast<float>(count));
    } else {
      rms[t] = 0.0f;
    }
  }

  return rms;
}

}  // namespace sonare
