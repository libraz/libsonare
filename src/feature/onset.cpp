#include "feature/onset.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "util/exception.h"

namespace sonare {

std::vector<float> compute_onset_strength(const MelSpectrogram& mel_spec,
                                          const OnsetConfig& config) {
  SONARE_CHECK(!mel_spec.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(config.lag >= 1, ErrorCode::InvalidParameter);

  int n_mels = mel_spec.n_mels();
  int n_frames = mel_spec.n_frames();
  const float* power = mel_spec.power_data();

  // Compute log power
  std::vector<float> log_power(n_mels * n_frames);
  float amin = 1e-10f;
  for (size_t i = 0; i < log_power.size(); ++i) {
    log_power[i] = std::log(std::max(power[i], amin));
  }

  // Compute first-order difference along time axis
  std::vector<float> onset_env(n_frames, 0.0f);

  for (int t = config.lag; t < n_frames; ++t) {
    float sum = 0.0f;
    for (int m = 0; m < n_mels; ++m) {
      float diff = log_power[m * n_frames + t] - log_power[m * n_frames + (t - config.lag)];
      // Half-wave rectification: only keep positive differences
      if (diff > 0.0f) {
        sum += diff;
      }
    }
    onset_env[t] = sum;
  }

  // Detrend: remove mean
  if (config.detrend && n_frames > 0) {
    float mean =
        std::accumulate(onset_env.begin(), onset_env.end(), 0.0f) / static_cast<float>(n_frames);
    for (auto& val : onset_env) {
      val -= mean;
    }
  }

  // Center: normalize by standard deviation
  if (config.center && n_frames > 1) {
    float mean =
        std::accumulate(onset_env.begin(), onset_env.end(), 0.0f) / static_cast<float>(n_frames);
    float var = 0.0f;
    for (const auto& val : onset_env) {
      float diff = val - mean;
      var += diff * diff;
    }
    var /= static_cast<float>(n_frames - 1);
    float std_dev = std::sqrt(var);

    if (std_dev > 1e-10f) {
      for (auto& val : onset_env) {
        val = (val - mean) / std_dev;
      }
    }
  }

  return onset_env;
}

std::vector<float> compute_onset_strength(const Audio& audio, const MelConfig& mel_config,
                                          const OnsetConfig& onset_config) {
  MelSpectrogram mel_spec = MelSpectrogram::compute(audio, mel_config);
  return compute_onset_strength(mel_spec, onset_config);
}

std::vector<float> onset_strength_multi(const MelSpectrogram& mel_spec, int n_bands,
                                        const OnsetConfig& config) {
  SONARE_CHECK(!mel_spec.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(n_bands > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.lag >= 1, ErrorCode::InvalidParameter);

  int n_mels = mel_spec.n_mels();
  int n_frames = mel_spec.n_frames();
  const float* power = mel_spec.power_data();

  // Divide Mel bands into n_bands groups
  int mels_per_band = n_mels / n_bands;

  // Compute log power
  std::vector<float> log_power(n_mels * n_frames);
  float amin = 1e-10f;
  for (size_t i = 0; i < log_power.size(); ++i) {
    log_power[i] = std::log(std::max(power[i], amin));
  }

  // Output: [n_bands x n_frames]
  std::vector<float> onset_multi(n_bands * n_frames, 0.0f);

  for (int b = 0; b < n_bands; ++b) {
    int mel_start = b * mels_per_band;
    int mel_end = (b == n_bands - 1) ? n_mels : (b + 1) * mels_per_band;

    for (int t = config.lag; t < n_frames; ++t) {
      float sum = 0.0f;
      for (int m = mel_start; m < mel_end; ++m) {
        float diff = log_power[m * n_frames + t] - log_power[m * n_frames + (t - config.lag)];
        // Half-wave rectification
        if (diff > 0.0f) {
          sum += diff;
        }
      }
      onset_multi[b * n_frames + t] = sum;
    }
  }

  // Apply detrend and center per band
  for (int b = 0; b < n_bands; ++b) {
    float* band = onset_multi.data() + b * n_frames;

    if (config.detrend && n_frames > 0) {
      float mean = 0.0f;
      for (int t = 0; t < n_frames; ++t) {
        mean += band[t];
      }
      mean /= static_cast<float>(n_frames);
      for (int t = 0; t < n_frames; ++t) {
        band[t] -= mean;
      }
    }

    if (config.center && n_frames > 1) {
      float mean = 0.0f;
      for (int t = 0; t < n_frames; ++t) {
        mean += band[t];
      }
      mean /= static_cast<float>(n_frames);

      float var = 0.0f;
      for (int t = 0; t < n_frames; ++t) {
        float diff = band[t] - mean;
        var += diff * diff;
      }
      var /= static_cast<float>(n_frames - 1);
      float std_dev = std::sqrt(var);

      if (std_dev > 1e-10f) {
        for (int t = 0; t < n_frames; ++t) {
          band[t] = (band[t] - mean) / std_dev;
        }
      }
    }
  }

  return onset_multi;
}

std::vector<float> spectral_flux(const Spectrogram& spec, int lag) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(lag >= 1, ErrorCode::InvalidParameter);

  int n_bins = spec.n_bins();
  int n_frames = spec.n_frames();
  const std::vector<float>& magnitude = spec.magnitude();

  std::vector<float> flux(n_frames, 0.0f);

  for (int t = lag; t < n_frames; ++t) {
    float sum = 0.0f;
    for (int k = 0; k < n_bins; ++k) {
      float diff = magnitude[k * n_frames + t] - magnitude[k * n_frames + (t - lag)];
      sum += std::abs(diff);
    }
    flux[t] = sum;
  }

  return flux;
}

}  // namespace sonare
