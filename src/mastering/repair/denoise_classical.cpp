#include "mastering/repair/denoise_classical.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <utility>
#include <vector>

#include "core/spectrum.h"
#include "mastering/common/noise_tracker.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare::mastering::repair {

namespace {

using sonare::constants::kPiD;

void validate(const DenoiseClassicalConfig& config) {
  if (config.n_fft <= 0 || (config.n_fft & (config.n_fft - 1)) != 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "denoise n_fft must be a positive power of two");
  }
  if (config.hop_length <= 0 || config.hop_length > config.n_fft) {
    throw SonareException(ErrorCode::InvalidParameter, "denoise hop_length must be in (0, n_fft]");
  }
  if (!(config.dd_alpha >= 0.0f) || config.dd_alpha >= 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "denoise dd_alpha must be in [0, 1)");
  }
  if (!(config.gain_floor >= 0.0f) || config.gain_floor > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "denoise gain_floor must be in [0, 1]");
  }
  if (!(config.over_subtraction >= 0.0f) || config.over_subtraction > 16.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "denoise over_subtraction must be in [0, 16]");
  }
  if (!(config.spectral_floor >= 0.0f) || config.spectral_floor > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "denoise spectral_floor must be in [0, 1]");
  }
  if (!(config.noise_estimation_quantile > 0.0f) || config.noise_estimation_quantile > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "denoise noise_estimation_quantile must be in (0, 1]");
  }
}

std::vector<double> estimate_quantile_noise_psd(const Spectrogram& spec, float quantile) {
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

std::vector<double> estimate_noise_psd_frames(const Spectrogram& spec,
                                              const DenoiseClassicalConfig& config) {
  const int bins = spec.n_bins();
  const int frames = spec.n_frames();
  std::vector<double> noise(static_cast<size_t>(bins * frames), 0.0);
  if (frames == 0) return noise;

  if (config.noise_estimator == DenoiseNoiseEstimator::Quantile) {
    const auto stationary = estimate_quantile_noise_psd(spec, config.noise_estimation_quantile);
    for (int b = 0; b < bins; ++b) {
      for (int t = 0; t < frames; ++t) {
        noise[static_cast<size_t>(b * frames + t)] = stationary[static_cast<size_t>(b)];
      }
    }
    return noise;
  }

  common::NoiseTracker::Mode mode = common::NoiseTracker::Mode::Imcra;
  if (config.noise_estimator == DenoiseNoiseEstimator::Mcra) {
    mode = common::NoiseTracker::Mode::Mcra;
  }
  common::NoiseTracker tracker(bins, spec.sample_rate(), mode, config.hop_length);
  std::vector<float> frame_power(static_cast<size_t>(bins), 0.0f);
  const auto& power = spec.power();

  for (int t = 0; t < frames; ++t) {
    for (int b = 0; b < bins; ++b) {
      frame_power[static_cast<size_t>(b)] =
          static_cast<float>(std::max(power[b * frames + t], 0.0f));
    }
    tracker.update(frame_power.data());
    const auto& tracked = tracker.noise_psd();
    for (int b = 0; b < bins; ++b) {
      noise[static_cast<size_t>(b * frames + t)] = tracked[static_cast<size_t>(b)];
    }
  }
  return noise;
}

// Exponential integral E1(x) = integral from x to infinity of (e^-t / t) dt.
// Approximated via Abramowitz & Stegun 5.1.53 (series) for x <= 1, and
// 5.1.56 (rational asymptotic) for x > 1. Accuracy ~1e-7 over (0, inf).
double exponential_integral_e1(double x) {
  if (x <= 0.0) return 0.0;
  if (x <= 1.0) {
    // A&S 5.1.53: E1(x) = -gamma - ln(x) + sum_{n=1..inf} (-1)^(n+1) x^n / (n * n!)
    constexpr double kGamma = 0.5772156649015329;
    double sum = -kGamma - std::log(x);
    double term = 1.0;
    for (int n = 1; n < 20; ++n) {
      term *= -x / static_cast<double>(n);
      sum -= term / static_cast<double>(n);
    }
    return sum;
  }
  // A&S 5.1.56: rational approximation for x > 1.
  const double a0 = 8.5733287401, a1 = 18.0590169730, a2 = 8.6347608925, a3 = 0.2677737343;
  const double b0 = 9.5733223454, b1 = 25.6329561486, b2 = 21.0996530827, b3 = 3.9584969228;
  const double num = x * x * x * x + a3 * x * x * x + a2 * x * x + a1 * x + a0;
  const double den = x * x * x * x + b3 * x * x * x + b2 * x * x + b1 * x + b0;
  return std::exp(-x) / x * (num / den);
}

float gain_logmmse(double ksi, double gamma_post) {
  const double nu = ksi * gamma_post / (1.0 + ksi);
  const double e1 = exponential_integral_e1(nu);
  return static_cast<float>((ksi / (1.0 + ksi)) * std::exp(0.5 * e1));
}

float gain_mmse_stsa(double ksi, double gamma_post) {
  // Closed-form using modified Bessel functions of orders 0 and 1 (Ephraim-Malah 1984).
  // G_STSA = sqrt(pi)/2 * sqrt(nu)/gamma * exp(-nu/2) * ((1+nu)*I0(nu/2) + nu*I1(nu/2))
  const double nu = ksi * gamma_post / (1.0 + ksi);
  if (nu < 1e-12) return 0.0f;
  const double half_nu = 0.5 * nu;
  if (half_nu > 30.0) {
    const double envelope = (1.0 + 4.0 * half_nu) / std::sqrt(2.0 * kPiD * half_nu);
    const double prefactor = std::sqrt(kPiD) * 0.5 * std::sqrt(nu) / gamma_post;
    const double gain = prefactor * envelope;
    return static_cast<float>(std::isfinite(gain) ? gain : 1.0);
  }
  // Series for I0(z) and I1(z) at z = nu/2 = half_nu.
  // I0(z) = sum_k (z/2)^(2k) / (k!)^2, I1(z) = sum_k (z/2)^(2k+1) / (k! (k+1)!).
  // The successive-term ratio therefore carries (z/2)^2 = (half_nu/2)^2.
  const double z_half_sq = 0.25 * half_nu * half_nu;
  double i0 = 1.0;
  double i1 = 0.0;
  double term0 = 1.0;
  double term1 = 0.5 * half_nu;
  i1 = term1;
  for (int k = 1; k < 25; ++k) {
    term0 *= z_half_sq / static_cast<double>(k * k);
    i0 += term0;
    if (k >= 1) {
      term1 *= z_half_sq / static_cast<double>(k * (k + 1));
      i1 += term1;
    }
  }
  const double envelope = std::exp(-half_nu) * ((1.0 + nu) * i0 + nu * i1);
  const double prefactor = std::sqrt(kPiD) * 0.5 * std::sqrt(nu) / gamma_post;
  const double gain = prefactor * envelope;
  return static_cast<float>(std::isfinite(gain) ? gain : 1.0);
}

double speech_presence_probability(double ksi, double gamma_post) {
  constexpr double q = 0.05;  // Cohen OM-LSA default speech-absence prior.
  const double v = ksi * gamma_post / (1.0 + ksi);
  const double odds = (q / (1.0 - q)) * (1.0 + ksi) * std::exp(-v);
  const double p = 1.0 / (1.0 + odds);
  return std::clamp(p, 0.05, 1.0);
}

std::vector<double> smooth_gain_3x3(const std::vector<double>& gains, int bins, int frames) {
  std::vector<double> smoothed = gains;
  std::vector<double> window;
  window.reserve(9);
  for (int b = 0; b < bins; ++b) {
    for (int t = 0; t < frames; ++t) {
      window.clear();
      for (int db = -1; db <= 1; ++db) {
        const int bb = b + db;
        if (bb < 0 || bb >= bins) continue;
        for (int dt = -1; dt <= 1; ++dt) {
          const int tt = t + dt;
          if (tt < 0 || tt >= frames) continue;
          window.push_back(gains[static_cast<size_t>(bb * frames + tt)]);
        }
      }
      std::nth_element(window.begin(), window.begin() + window.size() / 2, window.end());
      smoothed[static_cast<size_t>(b * frames + t)] = window[window.size() / 2];
    }
  }
  return smoothed;
}

Audio denoise_ephraim_malah(const Audio& audio, const Spectrogram& spec,
                            const std::vector<double>& noise_psd_frames,
                            const DenoiseClassicalConfig& config) {
  const int bins = spec.n_bins();
  const int frames = spec.n_frames();
  const auto* complex_data = spec.complex_data();

  std::vector<std::complex<float>> denoised_data(static_cast<size_t>(bins * frames));
  std::vector<double> gains(static_cast<size_t>(bins * frames), 1.0);
  // Decision-directed a priori SNR uses the previous frame's clean estimate.
  std::vector<double> prev_clean_power(static_cast<size_t>(bins), 0.0);
  const double alpha = config.dd_alpha;
  const double floor_gain = static_cast<double>(config.gain_floor);

  for (int t = 0; t < frames; ++t) {
    for (int b = 0; b < bins; ++b) {
      const size_t idx = static_cast<size_t>(b * frames + t);
      const std::complex<float>& bin = complex_data[idx];
      const double mag = std::abs(bin);
      const double power = mag * mag;
      const double noise = std::max(noise_psd_frames[idx], 1e-12);

      // a posteriori SNR.
      const double gamma_post = std::max(power / noise, 1e-6);
      // Decision-directed a priori SNR (Ephraim-Malah recursion).
      const double ml_estimate = std::max(gamma_post - 1.0, 0.0);
      const double ksi = std::max(
          alpha * prev_clean_power[static_cast<size_t>(b)] / noise + (1.0 - alpha) * ml_estimate,
          1e-6);

      double gain;
      if (config.mode == DenoiseMode::LogMmse) {
        gain = gain_logmmse(ksi, gamma_post);
      } else {
        gain = gain_mmse_stsa(ksi, gamma_post);
      }
      if (config.speech_presence_gain) {
        const double presence = speech_presence_probability(ksi, gamma_post);
        gain = std::pow(std::max(gain, floor_gain), presence) *
               std::pow(std::max(floor_gain, 1.0e-6), 1.0 - presence);
      }
      gain = std::max(gain, floor_gain);
      gain = std::min(gain, 1.0);

      gains[idx] = gain;
      prev_clean_power[static_cast<size_t>(b)] = power * gain * gain;
    }
  }

  if (config.gain_smoothing) {
    gains = smooth_gain_3x3(gains, bins, frames);
  }

  for (int b = 0; b < bins; ++b) {
    for (int t = 0; t < frames; ++t) {
      const size_t idx = static_cast<size_t>(b * frames + t);
      const std::complex<float>& bin = complex_data[idx];
      const float gf = static_cast<float>(std::clamp(gains[idx], floor_gain, 1.0));
      denoised_data[idx] = {bin.real() * gf, bin.imag() * gf};
    }
  }

  const Spectrogram clean =
      Spectrogram::from_complex(denoised_data.data(), bins, frames, spec.n_fft(), spec.hop_length(),
                                spec.sample_rate(), spec.center(), spec.win_length());
  return clean.to_audio(static_cast<int>(audio.size()));
}

Audio denoise_berouti(const Audio& audio, const Spectrogram& spec,
                      const std::vector<double>& noise_psd_frames,
                      const DenoiseClassicalConfig& config) {
  const int bins = spec.n_bins();
  const int frames = spec.n_frames();
  const auto* complex_data = spec.complex_data();

  std::vector<std::complex<float>> denoised_data(static_cast<size_t>(bins * frames));
  const double alpha = static_cast<double>(config.over_subtraction);
  const double beta = static_cast<double>(config.spectral_floor);

  for (int b = 0; b < bins; ++b) {
    for (int t = 0; t < frames; ++t) {
      const size_t idx = static_cast<size_t>(b * frames + t);
      const std::complex<float>& bin = complex_data[idx];
      const double mag = std::abs(bin);
      const double power = mag * mag;
      const double noise_pow = std::max(noise_psd_frames[idx], 1e-12);
      const double floor_pow = beta * noise_pow;
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

}  // namespace

Audio denoise_classical(const Audio& audio, const DenoiseClassicalConfig& config) {
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
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

  const auto noise_psd = estimate_noise_psd_frames(spec, config);

  switch (config.mode) {
    case DenoiseMode::LogMmse:
    case DenoiseMode::MmseStsa:
      return denoise_ephraim_malah(audio, spec, noise_psd, config);
    case DenoiseMode::SpectralSubtraction:
      return denoise_berouti(audio, spec, noise_psd, config);
  }
  return audio;
}

}  // namespace sonare::mastering::repair
