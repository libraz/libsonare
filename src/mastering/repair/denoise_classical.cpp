#include "mastering/repair/denoise_classical.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/spectrum.h"

namespace sonare::mastering::repair {

namespace {

void validate(const DenoiseClassicalConfig& config) {
  if (config.n_fft <= 0 || (config.n_fft & (config.n_fft - 1)) != 0) {
    throw std::invalid_argument("denoise n_fft must be a positive power of two");
  }
  if (config.hop_length <= 0 || config.hop_length > config.n_fft) {
    throw std::invalid_argument("denoise hop_length must be in (0, n_fft]");
  }
  if (!(config.over_subtraction >= 0.0f) || config.over_subtraction > 16.0f) {
    throw std::invalid_argument("denoise over_subtraction must be in [0, 16]");
  }
  if (!(config.spectral_floor >= 0.0f) || config.spectral_floor > 1.0f) {
    throw std::invalid_argument("denoise spectral_floor must be in [0, 1]");
  }
  if (!(config.noise_estimation_quantile > 0.0f) || config.noise_estimation_quantile > 1.0f) {
    throw std::invalid_argument("denoise noise_estimation_quantile must be in (0, 1]");
  }
}

std::vector<double> estimate_noise_psd(const Spectrogram& spec, float quantile) {
  const int bins = spec.n_bins();
  const int frames = spec.n_frames();
  std::vector<double> noise(static_cast<size_t>(bins), 0.0);
  if (frames == 0) return noise;

  const auto& power = spec.power();
  std::vector<std::pair<double, int>> frame_energies(static_cast<size_t>(frames));
  for (int t = 0; t < frames; ++t) {
    double energy = 0.0;
    for (int b = 0; b < bins; ++b) {
      energy += power[b * frames + t];
    }
    frame_energies[static_cast<size_t>(t)] = {energy, t};
  }
  std::sort(frame_energies.begin(), frame_energies.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  const int noise_frames =
      std::max(1, static_cast<int>(std::round(static_cast<float>(frames) * quantile)));
  for (int i = 0; i < noise_frames; ++i) {
    const int t = frame_energies[static_cast<size_t>(i)].second;
    for (int b = 0; b < bins; ++b) {
      noise[static_cast<size_t>(b)] += power[b * frames + t];
    }
  }
  const double scale = 1.0 / static_cast<double>(noise_frames);
  for (auto& v : noise) v *= scale;
  return noise;
}

}  // namespace

Audio denoise_classical(const Audio& audio, const DenoiseClassicalConfig& config) {
  if (audio.empty()) throw std::invalid_argument("audio must not be empty");
  validate(config);

  if (static_cast<int>(audio.size()) < config.n_fft) {
    return audio;
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
  const auto noise_psd = estimate_noise_psd(spec, config.noise_estimation_quantile);
  const auto* complex_data = spec.complex_data();

  std::vector<std::complex<float>> denoised_data(static_cast<size_t>(bins * frames));
  const double alpha = static_cast<double>(config.over_subtraction);
  const double beta = static_cast<double>(config.spectral_floor);

  for (int b = 0; b < bins; ++b) {
    const double noise_pow = noise_psd[static_cast<size_t>(b)];
    const double floor_pow = beta * noise_pow;
    for (int t = 0; t < frames; ++t) {
      const size_t idx = static_cast<size_t>(b * frames + t);
      const std::complex<float>& bin = complex_data[idx];
      const double mag = std::abs(bin);
      const double power = mag * mag;
      const double clean_power = std::max(power - alpha * noise_pow, floor_pow);
      const double gain = mag > 1e-12 ? std::sqrt(clean_power) / mag : 0.0;
      denoised_data[idx] = {static_cast<float>(bin.real() * gain),
                            static_cast<float>(bin.imag() * gain)};
    }
  }

  const Spectrogram clean =
      Spectrogram::from_complex(denoised_data.data(), bins, frames, spec.n_fft(), spec.hop_length(),
                                spec.sample_rate(), spec.center(), spec.win_length());
  return clean.to_audio(static_cast<int>(audio.size()));
}

}  // namespace sonare::mastering::repair
