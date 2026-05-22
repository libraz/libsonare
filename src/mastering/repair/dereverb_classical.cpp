#include "mastering/repair/dereverb_classical.h"

#include <cmath>
#include <complex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/spectrum.h"

namespace sonare::mastering::repair {
namespace {

bool is_power_of_two(int value) { return value > 0 && (value & (value - 1)) == 0; }


std::vector<std::complex<float>> solve_linear_system(
    std::vector<std::vector<std::complex<double>>> matrix,
    std::vector<std::complex<double>> rhs) {
  const size_t n = rhs.size();
  for (size_t col = 0; col < n; ++col) {
    size_t pivot = col;
    double best = std::abs(matrix[col][col]);
    for (size_t row = col + 1; row < n; ++row) {
      const double candidate = std::abs(matrix[row][col]);
      if (candidate > best) {
        best = candidate;
        pivot = row;
      }
    }
    if (best < 1.0e-18) continue;
    if (pivot != col) {
      std::swap(matrix[pivot], matrix[col]);
      std::swap(rhs[pivot], rhs[col]);
    }
    const auto diagonal = matrix[col][col];
    for (size_t k = col; k < n; ++k) matrix[col][k] /= diagonal;
    rhs[col] /= diagonal;
    for (size_t row = 0; row < n; ++row) {
      if (row == col) continue;
      const auto factor = matrix[row][col];
      if (std::abs(factor) < 1.0e-18) continue;
      for (size_t k = col; k < n; ++k) matrix[row][k] -= factor * matrix[col][k];
      rhs[row] -= factor * rhs[col];
    }
  }

  std::vector<std::complex<float>> solution(n);
  for (size_t i = 0; i < n; ++i) solution[i] = static_cast<std::complex<float>>(rhs[i]);
  return solution;
}

Audio legacy_tail_attenuate(const Audio& audio, const DereverbClassicalConfig& config) {
  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  bool in_tail = false;
  for (auto& sample : samples) {
    if (std::abs(sample) >= config.threshold) {
      in_tail = false;
    } else if (in_tail || std::abs(sample) > 0.0f) {
      in_tail = true;
      sample *= config.attenuation;
    }
  }
  return Audio::from_vector(std::move(samples), audio.sample_rate());
}

}  // namespace

Audio dereverb_classical(const Audio& audio, const DereverbClassicalConfig& config) {
  if (audio.empty()) throw std::invalid_argument("audio must not be empty");
  if (!(config.threshold >= 0.0f) || !(config.attenuation >= 0.0f && config.attenuation <= 1.0f) ||
      !is_power_of_two(config.n_fft) || config.hop_length <= 0 ||
      config.hop_length > config.n_fft || !(config.t60_sec > 0.0f) || config.late_delay_ms < 0.0f ||
      config.over_subtraction < 0.0f || config.spectral_floor < 0.0f ||
      config.spectral_floor > 1.0f || config.wpe_iterations < 1 || config.wpe_taps < 1 ||
      config.wpe_strength < 0.0f ||
      config.wpe_strength > 1.0f) {
    throw std::invalid_argument("invalid dereverb configuration");
  }

  if (static_cast<int>(audio.size()) < config.n_fft) {
    return legacy_tail_attenuate(audio, config);
  }

  StftConfig stft_config;
  stft_config.n_fft = config.n_fft;
  stft_config.hop_length = config.hop_length;
  stft_config.window = WindowType::Hann;
  stft_config.center = true;
  const Spectrogram spec = Spectrogram::compute(audio, stft_config);
  if (spec.empty()) return audio;

  const int bins = spec.n_bins();
  const int frames = spec.n_frames();
  const auto* complex_data = spec.complex_data();
  const auto& power = spec.power();
  std::vector<std::complex<float>> dereverbed(static_cast<size_t>(bins * frames));
  std::vector<std::complex<float>> working(complex_data, complex_data + bins * frames);

  const int delay_frames =
      std::max(1, static_cast<int>(std::round(config.late_delay_ms * 0.001f *
                                              static_cast<float>(audio.sample_rate()) /
                                              static_cast<float>(config.hop_length))));
  const float delay_sec = static_cast<float>(delay_frames * config.hop_length) /
                          static_cast<float>(audio.sample_rate());
  const double decay = std::exp(-2.0 * static_cast<double>(delay_sec) * 6.0 * std::log(10.0) /
                                static_cast<double>(config.t60_sec));

  for (int b = 0; b < bins; ++b) {
    for (int t = 0; t < frames; ++t) {
      const size_t idx = static_cast<size_t>(b * frames + t);
      const std::complex<float>& bin = working[idx];
      const double current_power = std::max(static_cast<double>(power[idx]), 1e-18);
      const int late_frame = t - delay_frames;
      const double late_psd =
          late_frame >= 0
              ? static_cast<double>(power[static_cast<size_t>(b * frames + late_frame)]) * decay
              : 0.0;
      const double clean_power =
          std::max(current_power - static_cast<double>(config.over_subtraction) * late_psd,
                   static_cast<double>(config.spectral_floor) * current_power);
      const double gain = std::sqrt(clean_power / current_power);
      dereverbed[idx] = {static_cast<float>(bin.real() * gain),
                         static_cast<float>(bin.imag() * gain)};
    }
  }

  if (config.wpe_enabled) {
    std::vector<std::complex<float>> next = dereverbed;
    const int taps = std::max(1, config.wpe_taps);
    const int first_predictable = delay_frames + taps - 1;
    for (int iteration = 0; iteration < config.wpe_iterations; ++iteration) {
      for (int b = 0; b < bins; ++b) {
        std::vector<std::vector<std::complex<double>>> covariance(
            static_cast<size_t>(taps),
            std::vector<std::complex<double>>(static_cast<size_t>(taps), {0.0, 0.0}));
        std::vector<std::complex<double>> cross(static_cast<size_t>(taps), {0.0, 0.0});
        for (int t = first_predictable; t < frames; ++t) {
          const auto current =
              static_cast<std::complex<double>>(dereverbed[static_cast<size_t>(b * frames + t)]);
          for (int i = 0; i < taps; ++i) {
            const auto xi = static_cast<std::complex<double>>(
                dereverbed[static_cast<size_t>(b * frames + t - delay_frames - i)]);
            cross[static_cast<size_t>(i)] += current * std::conj(xi);
            for (int j = 0; j < taps; ++j) {
              const auto xj = static_cast<std::complex<double>>(
                  dereverbed[static_cast<size_t>(b * frames + t - delay_frames - j)]);
              covariance[static_cast<size_t>(i)][static_cast<size_t>(j)] += xi * std::conj(xj);
            }
          }
        }
        for (int i = 0; i < taps; ++i) {
          covariance[static_cast<size_t>(i)][static_cast<size_t>(i)] += std::complex<double>{1.0e-8, 0.0};
        }
        auto predictors = solve_linear_system(std::move(covariance), std::move(cross));
        double predictor_norm = 0.0;
        for (const auto& predictor : predictors) predictor_norm += std::abs(predictor);
        if (predictor_norm > 0.98) {
          const float scale = static_cast<float>(0.98 / predictor_norm);
          for (auto& predictor : predictors) predictor *= scale;
        }
        for (int t = 0; t < frames; ++t) {
          const size_t idx = static_cast<size_t>(b * frames + t);
          if (t < first_predictable) {
            next[idx] = dereverbed[idx];
            continue;
          }
          std::complex<float> predicted{0.0f, 0.0f};
          for (int tap = 0; tap < taps; ++tap) {
            predicted += predictors[static_cast<size_t>(tap)] *
                         dereverbed[static_cast<size_t>(b * frames + t - delay_frames - tap)];
          }
          next[idx] = dereverbed[idx] - config.wpe_strength * predicted;
        }
      }
      dereverbed.swap(next);
    }
  }

  const Spectrogram clean =
      Spectrogram::from_complex(dereverbed.data(), bins, frames, spec.n_fft(), spec.hop_length(),
                                spec.sample_rate(), spec.center(), spec.win_length());
  return clean.to_audio(static_cast<int>(audio.size()));
}

}  // namespace sonare::mastering::repair
