#include "mastering/repair/denoise_classical.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <utility>
#include <vector>

#include "core/spectrum.h"
#include "mastering/common/noise_profile.h"
#include "mastering/common/noise_tracker.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/validated.h"

namespace sonare::mastering::repair {

namespace {

using sonare::constants::kPiD;

/// @brief Gain below which an attenuation is reported as the mask's floor.
constexpr double kReportedGainEpsilon = 1e-12;

StftConfig analysis_config(const DenoiseClassicalConfig& config) {
  StftConfig stft_config;
  stft_config.n_fft = config.n_fft;
  stft_config.hop_length = config.hop_length;
  stft_config.window = WindowType::Hann;
  stft_config.center = true;
  return stft_config;
}

/// @brief |z|^2 per cell, squared from the magnitude.
/// @details Deliberately not Spectrogram::power(), which is re^2 + im^2: the two
///   part company in the last bits and this module's gain recursion has always
///   been defined by this one while its noise estimator reads the other.
std::vector<double> magnitude_power(const Spectrogram& spec) {
  const size_t cells = static_cast<size_t>(spec.n_bins()) * static_cast<size_t>(spec.n_frames());
  const auto* data = spec.complex_data();
  std::vector<double> out(cells, 0.0);
  for (size_t i = 0; i < cells; ++i) {
    const double magnitude = std::abs(data[i]);
    out[i] = magnitude * magnitude;
  }
  return out;
}

std::vector<double> estimate_quantile_noise_psd(const float* power, int bins, int frames,
                                                float quantile) {
  std::vector<double> noise(static_cast<size_t>(bins), 0.0);
  if (frames == 0) return noise;

  const auto row_of = [power, frames](int b) {
    return power + static_cast<size_t>(b) * static_cast<size_t>(frames);
  };

  // `power` is row-major [bins x frames], so a bin is one contiguous row. Accumulating
  // bin-major still feeds every frame's energy its bins in increasing b, as the column walk did.
  std::vector<std::pair<double, int>> frame_energies(static_cast<size_t>(frames));
  for (int t = 0; t < frames; ++t) {
    frame_energies[static_cast<size_t>(t)] = {0.0, t};
  }
  for (int b = 0; b < bins; ++b) {
    const float* row = row_of(b);
    for (int t = 0; t < frames; ++t) {
      frame_energies[static_cast<size_t>(t)].first += row[t];
    }
  }
  std::sort(frame_energies.begin(), frame_energies.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  const int noise_frames =
      std::max(1, static_cast<int>(std::round(static_cast<float>(frames) * quantile)));
  std::vector<int> selected(static_cast<size_t>(noise_frames));
  for (int i = 0; i < noise_frames; ++i) {
    selected[static_cast<size_t>(i)] = frame_energies[static_cast<size_t>(i)].second;
  }
  // The selected frames are scattered, so bin-major buys a working set of one row rather than a
  // contiguous read. Each bin still sums the frames in the order they were selected.
  for (int b = 0; b < bins; ++b) {
    const float* row = row_of(b);
    double accumulated = 0.0;
    for (int i = 0; i < noise_frames; ++i) {
      accumulated += row[selected[static_cast<size_t>(i)]];
    }
    noise[static_cast<size_t>(b)] = accumulated;
  }
  const double scale = 1.0 / static_cast<double>(noise_frames);
  for (auto& v : noise) v *= scale;
  return noise;
}

std::vector<double> estimate_noise_psd_frames(const float* power, int bins, int frames,
                                              int sample_rate,
                                              const DenoiseClassicalConfig& config) {
  std::vector<double> noise(static_cast<size_t>(bins * frames), 0.0);
  if (frames == 0) return noise;

  if (config.noise_estimator == DenoiseNoiseEstimator::Quantile) {
    const auto stationary =
        estimate_quantile_noise_psd(power, bins, frames, config.noise_estimation_quantile);
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
  common::NoiseTracker tracker(bins, sample_rate, mode, config.hop_length);

  // The tracker is recursive in t, so this is a tiling rather than an exchange: a bounded block
  // lets the read and the write-back run along bin rows while the tracker still sees whole
  // columns in order.
  constexpr int kFrameTile = 64;
  std::vector<float> power_tile(static_cast<size_t>(bins) * kFrameTile, 0.0f);
  std::vector<float> noise_tile(static_cast<size_t>(bins) * kFrameTile, 0.0f);

  for (int tile_start = 0; tile_start < frames; tile_start += kFrameTile) {
    const int tile_frames = std::min(kFrameTile, frames - tile_start);

    for (int b = 0; b < bins; ++b) {
      const float* row = power + static_cast<size_t>(b) * static_cast<size_t>(frames) +
                         static_cast<size_t>(tile_start);
      for (int t = 0; t < tile_frames; ++t) {
        power_tile[static_cast<size_t>(t) * static_cast<size_t>(bins) + static_cast<size_t>(b)] =
            std::max(row[t], 0.0f);
      }
    }

    for (int t = 0; t < tile_frames; ++t) {
      const size_t frame_offset = static_cast<size_t>(t) * static_cast<size_t>(bins);
      tracker.update(power_tile.data() + frame_offset);
      const float* tracked = tracker.noise_psd();
      std::copy(tracked, tracked + bins, noise_tile.data() + frame_offset);
    }

    for (int b = 0; b < bins; ++b) {
      double* out = noise.data() + static_cast<size_t>(b) * static_cast<size_t>(frames) +
                    static_cast<size_t>(tile_start);
      for (int t = 0; t < tile_frames; ++t) {
        out[t] =
            noise_tile[static_cast<size_t>(t) * static_cast<size_t>(bins) + static_cast<size_t>(b)];
      }
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

std::vector<double> gains_ephraim_malah(const double* power_cells, const double* noise_psd_frames,
                                        int bins, int frames,
                                        const DenoiseClassicalConfig& config) {
  std::vector<double> gains(static_cast<size_t>(bins * frames), 1.0);
  // Decision-directed a priori SNR uses the previous frame's clean estimate.
  std::vector<double> prev_clean_power(static_cast<size_t>(bins), 0.0);
  const double alpha = config.dd_alpha;
  const double floor_gain = static_cast<double>(config.gain_floor);

  for (int t = 0; t < frames; ++t) {
    for (int b = 0; b < bins; ++b) {
      const size_t idx = static_cast<size_t>(b * frames + t);
      const double power = power_cells[idx];
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
  // Rounded to float here rather than at the multiply: the mask is one real
  // number per cell and every channel has to be scaled by the same one.
  for (double& gain : gains) {
    gain = static_cast<double>(static_cast<float>(std::clamp(gain, floor_gain, 1.0)));
  }
  return gains;
}

std::vector<double> gains_berouti(const double* power_cells, const double* noise_psd_frames,
                                  int bins, int frames, const DenoiseClassicalConfig& config) {
  std::vector<double> gains(static_cast<size_t>(bins * frames), 1.0);
  const double alpha = static_cast<double>(config.over_subtraction);
  const double beta = static_cast<double>(config.spectral_floor);

  for (int b = 0; b < bins; ++b) {
    for (int t = 0; t < frames; ++t) {
      const size_t idx = static_cast<size_t>(b * frames + t);
      const double power = power_cells[idx];
      const double mag = std::sqrt(power);
      const double noise_pow = std::max(noise_psd_frames[idx], 1e-12);
      const double floor_pow = beta * noise_pow;
      const double clean_power = std::max(power - alpha * noise_pow, floor_pow);
      gains[idx] = mag > 1e-12 ? std::sqrt(clean_power) / mag : 0.0;
    }
  }
  return gains;
}

std::vector<double> compute_gains(const double* power_cells, const double* noise_psd_frames,
                                  int bins, int frames, const DenoiseClassicalConfig& config) {
  if (config.mode == DenoiseMode::SpectralSubtraction) {
    return gains_berouti(power_cells, noise_psd_frames, bins, frames, config);
  }
  return gains_ephraim_malah(power_cells, noise_psd_frames, bins, frames, config);
}

double mean_square(const float* samples, size_t size) {
  double sum = 0.0;
  for (size_t i = 0; i < size; ++i) {
    sum += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
  }
  return size == 0 ? 0.0 : sum / static_cast<double>(size);
}

NoiseDetection to_detection(const common::NoiseFloorDbfs& levels) {
  NoiseDetection detection;
  detection.floor_dbfs = levels.broadband;
  for (size_t k = 0; k < kRepairNoiseBandCount; ++k) detection.band_floor_dbfs[k] = levels.bands[k];
  return detection;
}

void summarize_mask(const std::vector<double>& gains, const DenoiseClassicalConfig& config,
                    DenoiseReport* report) {
  if (gains.empty()) return;
  const double floor_gain = static_cast<double>(config.gain_floor);
  const bool has_gain_floor = config.mode != DenoiseMode::SpectralSubtraction;
  double sum_db = 0.0;
  double max_db = 0.0;
  size_t at_floor = 0;
  for (const double gain : gains) {
    const double reduction = -20.0 * std::log10(std::max(gain, kReportedGainEpsilon));
    sum_db += reduction;
    max_db = std::max(max_db, reduction);
    if (has_gain_floor && gain <= floor_gain) ++at_floor;
  }
  report->mean_reduction_db = static_cast<float>(sum_db / static_cast<double>(gains.size()));
  report->max_reduction_db = static_cast<float>(max_db);
  report->floor_limited_fraction =
      static_cast<float>(static_cast<double>(at_floor) / static_cast<double>(gains.size()));
}

}  // namespace

void validate_config(const DenoiseClassicalConfig& config) {
  if (config.mode != DenoiseMode::LogMmse && config.mode != DenoiseMode::MmseStsa &&
      config.mode != DenoiseMode::SpectralSubtraction) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid denoise mode");
  }
  if (config.noise_estimator != DenoiseNoiseEstimator::Quantile &&
      config.noise_estimator != DenoiseNoiseEstimator::Mcra &&
      config.noise_estimator != DenoiseNoiseEstimator::Imcra) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid denoise noise estimator");
  }
  if (config.n_fft <= 0 || (config.n_fft & (config.n_fft - 1)) != 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "denoise n_fft must be a positive power of two");
  }
  if (config.hop_length <= 0 || config.hop_length > config.n_fft) {
    throw SonareException(ErrorCode::InvalidParameter, "denoise hop_length must be in (0, n_fft]");
  }
  if (!std::isfinite(config.dd_alpha) || config.dd_alpha < 0.0f || config.dd_alpha >= 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "denoise dd_alpha must be finite and in [0, 1)");
  }
  if (!std::isfinite(config.gain_floor) || config.gain_floor < 0.0f || config.gain_floor > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "denoise gain_floor must be finite and in [0, 1]");
  }
  if (!std::isfinite(config.over_subtraction) || config.over_subtraction < 0.0f ||
      config.over_subtraction > 16.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "denoise over_subtraction must be finite and in [0, 16]");
  }
  if (!std::isfinite(config.spectral_floor) || config.spectral_floor < 0.0f ||
      config.spectral_floor > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "denoise spectral_floor must be finite and in [0, 1]");
  }
  if (!std::isfinite(config.noise_estimation_quantile) ||
      config.noise_estimation_quantile <= 0.0f || config.noise_estimation_quantile > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "denoise noise_estimation_quantile must be finite and in (0, 1]");
  }
}

NoiseDetection detect_noise_floor(const float* samples, std::size_t size, int sample_rate,
                                  const DenoiseClassicalConfig& config) {
  const auto validated = Validated<DenoiseClassicalConfig>::make(config);
  if (samples == nullptr || size == 0) {
    throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  }
  if (sample_rate <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (size < static_cast<size_t>(validated->n_fft)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "denoise input must contain at least n_fft samples");
  }

  const Audio audio = Audio::from_buffer(samples, size, sample_rate);
  const Spectrogram spec = Spectrogram::compute(audio, analysis_config(validated.get()));
  if (spec.empty()) return NoiseDetection{};

  const auto noise_psd = estimate_noise_psd_frames(spec.power().data(), spec.n_bins(),
                                                   spec.n_frames(), sample_rate, validated.get());
  return to_detection(common::noise_floor_dbfs(noise_psd.data(), spec.power().data(), spec.n_bins(),
                                               spec.n_frames(), mean_square(samples, size),
                                               sample_rate));
}

Audio denoise_classical(const Audio& audio, const DenoiseClassicalConfig& config) {
  return denoise_classical(audio, config, nullptr);
}

Audio denoise_classical(const Audio& audio, const DenoiseClassicalConfig& config,
                        DenoiseReport* report) {
  if (report != nullptr) *report = DenoiseReport{};
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  const auto validated = Validated<DenoiseClassicalConfig>::make(config);

  if (static_cast<int>(audio.size()) < validated->n_fft) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "denoise input must contain at least n_fft samples");
  }

  const Spectrogram spec = Spectrogram::compute(audio, analysis_config(validated.get()));
  if (spec.empty()) return audio;

  const int bins = spec.n_bins();
  const int frames = spec.n_frames();
  const auto noise_psd = estimate_noise_psd_frames(spec.power().data(), bins, frames,
                                                   spec.sample_rate(), validated.get());
  const auto power_cells = magnitude_power(spec);
  const auto gains =
      compute_gains(power_cells.data(), noise_psd.data(), bins, frames, validated.get());

  // Off the sample path entirely, so asking for it cannot move the output.
  if (report != nullptr) {
    report->detected = to_detection(
        common::noise_floor_dbfs(noise_psd.data(), spec.power().data(), bins, frames,
                                 mean_square(audio.data(), audio.size()), spec.sample_rate()));
    summarize_mask(gains, validated.get(), report);
  }

  const auto* complex_data = spec.complex_data();
  std::vector<std::complex<float>> denoised(gains.size());
  for (size_t i = 0; i < gains.size(); ++i) {
    denoised[i] = {static_cast<float>(complex_data[i].real() * gains[i]),
                   static_cast<float>(complex_data[i].imag() * gains[i])};
  }
  const Spectrogram clean = Spectrogram::from_complex(
      denoised.data(), bins, frames, spec.n_fft(), spec.hop_length(), spec.sample_rate(),
      spec.window(), spec.center(), spec.win_length());
  return clean.to_audio(static_cast<int>(audio.size()));
}

DenoiseReport denoise_classical_linked(const Audio* const* channels, std::size_t channel_count,
                                       std::vector<Audio>* out,
                                       const DenoiseClassicalConfig& config) {
  const auto validated = Validated<DenoiseClassicalConfig>::make(config);
  if (out == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "denoise output must not be null");
  }
  if (channels == nullptr || channel_count == 0 || channels[0] == nullptr || channels[0]->empty()) {
    throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  }
  const size_t length = channels[0]->size();
  if (static_cast<int>(length) < validated->n_fft) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "denoise input must contain at least n_fft samples");
  }

  const common::LinkedSpectra linked =
      common::LinkedSpectra::compute(channels, channel_count, analysis_config(validated.get()));
  DenoiseReport report;
  if (linked.empty()) {
    out->clear();
    for (size_t c = 0; c < channel_count; ++c) out->push_back(*channels[c]);
    return report;
  }

  const int bins = linked.n_bins();
  const int frames = linked.n_frames();
  const int sample_rate = channels[0]->sample_rate();
  const auto noise_psd = estimate_noise_psd_frames(linked.summed_power().data(), bins, frames,
                                                   sample_rate, validated.get());
  const auto power_cells = linked.magnitude_power_sum();
  const auto gains =
      compute_gains(power_cells.data(), noise_psd.data(), bins, frames, validated.get());

  *out = linked.resynthesize(linked.masked(gains.data()), static_cast<int>(length));

  double summed_mean_square = 0.0;
  for (size_t c = 0; c < channel_count; ++c) {
    summed_mean_square += mean_square(channels[c]->data(), channels[c]->size());
  }
  report.detected =
      to_detection(common::noise_floor_dbfs(noise_psd.data(), linked.summed_power().data(), bins,
                                            frames, summed_mean_square, sample_rate));
  summarize_mask(gains, validated.get(), &report);
  return report;
}

DenoiseStereoResult denoise_classical_stereo(const Audio& left, const Audio& right,
                                             const DenoiseClassicalConfig& config) {
  const Audio* channels[2] = {&left, &right};
  std::vector<Audio> out;
  DenoiseStereoResult result;
  result.report = denoise_classical_linked(channels, 2, &out, config);
  result.left = std::move(out[0]);
  result.right = std::move(out[1]);
  return result;
}

}  // namespace sonare::mastering::repair
