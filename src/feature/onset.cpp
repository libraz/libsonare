#include "feature/onset.h"

#include <Eigen/Core>
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

  /// Map power data to Eigen matrix [n_mels x n_frames] (row-major)
  Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>
      power_map(power, n_mels, n_frames);

  /// Compute log power using Eigen (vectorized)
  constexpr float kAmin = 1e-10f;
  Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> log_power =
      power_map.array().max(kAmin).log();

  /// @details Compute first-order difference and half-wave rectification.
  /// diff = log_power[:, lag:] - log_power[:, :-lag]
  std::vector<float> onset_env(n_frames, 0.0f);

  if (n_frames > config.lag) {
    int diff_frames = n_frames - config.lag;

    // Compute difference matrix
    Eigen::MatrixXf diff =
        log_power.rightCols(diff_frames) - log_power.leftCols(diff_frames);

    // Half-wave rectification and column sum
    Eigen::Map<Eigen::VectorXf> env_map(onset_env.data() + config.lag, diff_frames);
    env_map = diff.array().max(0.0f).colwise().sum();
  }

  /// Detrend: remove mean
  if (config.detrend && n_frames > 0) {
    Eigen::Map<Eigen::ArrayXf> env_array(onset_env.data(), n_frames);
    env_array -= env_array.mean();
  }

  /// Center: normalize by standard deviation
  if (config.center && n_frames > 1) {
    Eigen::Map<Eigen::ArrayXf> env_array(onset_env.data(), n_frames);
    float mean = env_array.mean();
    float var = (env_array - mean).square().sum() / static_cast<float>(n_frames - 1);
    float std_dev = std::sqrt(var);

    if (std_dev > 1e-10f) {
      env_array = (env_array - mean) / std_dev;
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

  // Map power data to Eigen matrix [n_mels x n_frames] (row-major)
  Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>
      power_map(power, n_mels, n_frames);

  // Compute log power using Eigen
  constexpr float kAmin = 1e-10f;
  Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> log_power =
      power_map.array().max(kAmin).log();

  // Divide Mel bands into n_bands groups
  int mels_per_band = n_mels / n_bands;

  // Output: [n_bands x n_frames]
  std::vector<float> onset_multi(n_bands * n_frames, 0.0f);

  if (n_frames > config.lag) {
    int diff_frames = n_frames - config.lag;

    for (int b = 0; b < n_bands; ++b) {
      int mel_start = b * mels_per_band;
      int mel_end = (b == n_bands - 1) ? n_mels : (b + 1) * mels_per_band;
      int band_size = mel_end - mel_start;

      // Extract band and compute difference
      Eigen::MatrixXf band_diff =
          log_power.block(mel_start, config.lag, band_size, diff_frames) -
          log_power.block(mel_start, 0, band_size, diff_frames);

      // Half-wave rectification and column sum
      Eigen::Map<Eigen::VectorXf> out_map(onset_multi.data() + b * n_frames + config.lag,
                                          diff_frames);
      out_map = band_diff.array().max(0.0f).colwise().sum();
    }
  }

  // Apply detrend and center per band using Eigen
  Eigen::Map<Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>
      onset_map(onset_multi.data(), n_bands, n_frames);

  if (config.detrend && n_frames > 0) {
    Eigen::VectorXf row_means = onset_map.rowwise().mean();
    onset_map.colwise() -= row_means;
  }

  if (config.center && n_frames > 1) {
    for (int b = 0; b < n_bands; ++b) {
      Eigen::Map<Eigen::ArrayXf> band(onset_multi.data() + b * n_frames, n_frames);
      float mean = band.mean();
      float var = (band - mean).square().sum() / static_cast<float>(n_frames - 1);
      float std_dev = std::sqrt(var);

      if (std_dev > 1e-10f) {
        band = (band - mean) / std_dev;
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

  if (n_frames > lag) {
    int diff_frames = n_frames - lag;

    // Map magnitude to Eigen matrix [n_bins x n_frames] (row-major)
    Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>
        mag_map(magnitude.data(), n_bins, n_frames);

    // Compute difference and absolute sum
    Eigen::MatrixXf diff = mag_map.rightCols(diff_frames) - mag_map.leftCols(diff_frames);

    Eigen::Map<Eigen::VectorXf> flux_map(flux.data() + lag, diff_frames);
    flux_map = diff.array().abs().colwise().sum();
  }

  return flux;
}

}  // namespace sonare
