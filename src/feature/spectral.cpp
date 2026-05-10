#include "feature/spectral.h"

#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <numeric>

#include "util/exception.h"
#include "util/math_utils.h"

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
  SONARE_CHECK(n_bins > 0 && n_frames > 0 && sr > 0 && n_fft > 0, ErrorCode::InvalidParameter);

  std::vector<float> freqs = bin_frequencies(n_bins, sr, n_fft);
  std::vector<float> centroid(n_frames);

  for (int t = 0; t < n_frames; ++t) {
    float weighted_sum = 0.0f;
    float magnitude_sum = 0.0f;
    for (int k = 0; k < n_bins; ++k) {
      float mag = magnitude[k * n_frames + t];
      SONARE_CHECK(mag >= 0.0f, ErrorCode::InvalidParameter);
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
      float mag = magnitude[k * n_frames + t];
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
      float mag = magnitude[k * n_frames + t];
      SONARE_CHECK(mag >= 0.0f, ErrorCode::InvalidParameter);
      total += mag;
    }

    float threshold = roll_percent * total;
    float cumulative = 0.0f;

    int rolloff_bin = n_bins - 1;
    for (int k = 0; k < n_bins; ++k) {
      float mag = magnitude[k * n_frames + t];
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

  constexpr float kAmin = 1e-10f;

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

    int q_count =
        std::max(1, static_cast<int>(std::round(quantile * band_indices.size())));

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
  power_to_db(peak.data(), peak.size(), 1.0f, 1e-10f, 80.0f, peak_db.data());
  power_to_db(valley.data(), valley.size(), 1.0f, 1e-10f, 80.0f, valley_db.data());

  for (size_t i = 0; i < contrast.size(); ++i) {
    contrast[i] = peak_db[i] - valley_db[i];
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
