#include "mastering/match/match_eq.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/fft.h"
#include "core/window.h"
#include "mastering/common/partitioned_convolver.h"
#include "util/constants.h"
#include "util/db.h"

namespace sonare::mastering::match {
namespace {

using sonare::constants::kPiD;
using sonare::constants::kTwoPiD;

float interpolate_db(const ReferenceSpectrum& spectrum, float frequency_hz) {
  if (spectrum.frequencies.empty() || spectrum.db.empty()) {
    throw std::invalid_argument("spectrum must not be empty");
  }
  if (spectrum.frequencies.size() != spectrum.db.size()) {
    throw std::invalid_argument("spectrum size mismatch");
  }
  if (frequency_hz <= spectrum.frequencies.front()) {
    return spectrum.db.front();
  }
  if (frequency_hz >= spectrum.frequencies.back()) {
    return spectrum.db.back();
  }

  const auto upper =
      std::upper_bound(spectrum.frequencies.begin(), spectrum.frequencies.end(), frequency_hz);
  const size_t index = static_cast<size_t>(upper - spectrum.frequencies.begin());
  const float f0 = spectrum.frequencies[index - 1];
  const float f1 = spectrum.frequencies[index];
  const float t = (frequency_hz - f0) / std::max(f1 - f0, 1.0f);
  return spectrum.db[index - 1] + (spectrum.db[index] - spectrum.db[index - 1]) * t;
}

bool is_power_of_two(int value) { return value > 0 && (value & (value - 1)) == 0; }

float db_to_gain(float db) { return db_to_linear(db); }

std::vector<float> smooth_log_frequency(const std::vector<float>& frequencies,
                                        const std::vector<float>& gain_db, int smoothing_bins) {
  if (smoothing_bins <= 0 || gain_db.size() <= 2) return gain_db;

  std::vector<float> smoothed(gain_db.size(), 0.0f);
  for (size_t i = 0; i < gain_db.size(); ++i) {
    const size_t begin =
        i > static_cast<size_t>(smoothing_bins) ? i - static_cast<size_t>(smoothing_bins) : 0;
    const size_t end = std::min(gain_db.size(), i + static_cast<size_t>(smoothing_bins) + 1);
    const double center = std::log(std::max(frequencies[i], 1.0f));
    const double radius =
        std::max(std::abs(std::log(std::max(frequencies[end - 1], 1.0f)) - center),
                 std::abs(center - std::log(std::max(frequencies[begin], 1.0f))));
    double weighted_sum = 0.0;
    double weight_sum = 0.0;
    for (size_t j = begin; j < end; ++j) {
      const double distance =
          radius > 0.0 ? std::abs(std::log(std::max(frequencies[j], 1.0f)) - center) / radius : 0.0;
      const double weight = 0.5 + 0.5 * std::cos(std::min(distance, 1.0) * kPiD);
      weighted_sum += static_cast<double>(gain_db[j]) * weight;
      weight_sum += weight;
    }
    smoothed[i] = static_cast<float>(weight_sum > 0.0 ? weighted_sum / weight_sum : gain_db[i]);
  }
  return smoothed;
}

std::vector<std::complex<double>> dft(const std::vector<std::complex<double>>& input,
                                      bool inverse) {
  const size_t n = input.size();
  std::vector<std::complex<double>> output(n);
  const double sign = inverse ? 1.0 : -1.0;
  for (size_t k = 0; k < n; ++k) {
    std::complex<double> sum{0.0, 0.0};
    for (size_t t = 0; t < n; ++t) {
      const double angle = sign * kTwoPiD * static_cast<double>(k * t) / static_cast<double>(n);
      sum += input[t] * std::complex<double>{std::cos(angle), std::sin(angle)};
    }
    output[k] = inverse ? sum / static_cast<double>(n) : sum;
  }
  return output;
}

std::vector<float> minimum_phase_kernel(const std::vector<float>& magnitude_bins, int kernel_size) {
  const size_t n_bins = magnitude_bins.size();
  const size_t n_fft = (n_bins - 1) * 2;
  std::vector<std::complex<double>> log_magnitude(n_fft, {0.0, 0.0});
  for (size_t k = 0; k < n_bins; ++k) {
    log_magnitude[k] = {std::log(std::max(static_cast<double>(magnitude_bins[k]), 1e-9)), 0.0};
  }
  for (size_t k = 1; k + 1 < n_bins; ++k) {
    log_magnitude[n_fft - k] = log_magnitude[k];
  }

  auto cepstrum = dft(log_magnitude, true);
  for (size_t n = 1; n < n_fft / 2; ++n) cepstrum[n] *= 2.0;
  for (size_t n = n_fft / 2 + 1; n < n_fft; ++n) cepstrum[n] = {0.0, 0.0};

  auto minimum_log_spectrum = dft(cepstrum, false);
  std::vector<std::complex<double>> minimum_spectrum(n_fft);
  for (size_t k = 0; k < n_fft; ++k) {
    minimum_spectrum[k] = std::exp(minimum_log_spectrum[k]);
  }
  auto impulse = dft(minimum_spectrum, true);

  std::vector<float> kernel(static_cast<size_t>(kernel_size), 0.0f);
  for (int i = 0; i < kernel_size; ++i) {
    kernel[static_cast<size_t>(i)] = static_cast<float>(impulse[static_cast<size_t>(i)].real());
  }
  return kernel;
}

std::vector<float> apply_fir_partitioned(const Audio& audio, const std::vector<float>& kernel,
                                         int partition_size, int latency_compensation) {
  const int block_size = partition_size > 0 ? partition_size : 256;
  common::PartitionedConvolver convolver({block_size});
  convolver.set_impulse_response(kernel);

  const size_t padded_size = ((audio.size() + kernel.size() + static_cast<size_t>(block_size) - 1) /
                              static_cast<size_t>(block_size)) *
                             static_cast<size_t>(block_size);
  std::vector<float> input(padded_size, 0.0f);
  std::copy(audio.begin(), audio.end(), input.begin());
  std::vector<float> convolved(padded_size, 0.0f);
  for (size_t offset = 0; offset < padded_size; offset += static_cast<size_t>(block_size)) {
    convolver.process_block(input.data() + offset, convolved.data() + offset);
  }

  std::vector<float> output(audio.size(), 0.0f);
  for (size_t i = 0; i < output.size(); ++i) {
    const size_t source = i + static_cast<size_t>(std::max(latency_compensation, 0));
    output[i] = source < convolved.size() ? convolved[source] : 0.0f;
  }
  return output;
}

}  // namespace

MatchEqCurve match_eq_curve(const ReferenceSpectrum& source, const ReferenceSpectrum& reference,
                            const MatchEqConfig& config) {
  if (!(config.max_gain_db >= 0.0f) || !(config.min_frequency_hz > 0.0f) ||
      !(config.max_frequency_hz > config.min_frequency_hz) || !(config.q > 0.0f) ||
      config.smoothing_bins < 0) {
    throw std::invalid_argument("invalid match EQ configuration");
  }
  if (source.sample_rate != reference.sample_rate) {
    throw std::invalid_argument("sample rates must match");
  }
  if (source.frequencies.empty() || source.frequencies.size() != source.db.size()) {
    throw std::invalid_argument("invalid source spectrum");
  }

  MatchEqCurve curve;
  curve.frequencies.reserve(source.frequencies.size());
  curve.gain_db.reserve(source.frequencies.size());
  for (float frequency : source.frequencies) {
    if (frequency < config.min_frequency_hz || frequency > config.max_frequency_hz) continue;
    curve.frequencies.push_back(frequency);
    curve.gain_db.push_back(
        std::clamp(interpolate_db(reference, frequency) - interpolate_db(source, frequency),
                   -config.max_gain_db, config.max_gain_db));
  }

  curve.gain_db = smooth_log_frequency(curve.frequencies, curve.gain_db, config.smoothing_bins);
  return curve;
}

std::vector<float> match_eq_fir_kernel(const MatchEqCurve& curve, int sample_rate,
                                       const MatchEqFirConfig& config) {
  if (curve.frequencies.empty() || curve.frequencies.size() != curve.gain_db.size()) {
    throw std::invalid_argument("invalid match EQ curve");
  }
  if (sample_rate <= 0 || !is_power_of_two(config.fft_size) || config.kernel_size <= 0 ||
      config.kernel_size > config.fft_size || (config.kernel_size % 2) == 0 ||
      config.partition_size < 0) {
    throw std::invalid_argument("invalid match EQ FIR configuration");
  }

  FFT fft(config.fft_size);
  std::vector<std::complex<float>> spectrum(static_cast<size_t>(fft.n_bins()));
  std::vector<float> magnitude(static_cast<size_t>(fft.n_bins()), 1.0f);
  const ReferenceSpectrum curve_spectrum{curve.frequencies, curve.gain_db, sample_rate};
  for (int bin = 0; bin < fft.n_bins(); ++bin) {
    const float frequency = static_cast<float>(bin) * static_cast<float>(sample_rate) /
                            static_cast<float>(config.fft_size);
    magnitude[static_cast<size_t>(bin)] = db_to_gain(interpolate_db(curve_spectrum, frequency));
    spectrum[static_cast<size_t>(bin)] = {magnitude[static_cast<size_t>(bin)], 0.0f};
  }

  if (config.phase == MatchEqFirPhase::MinimumPhase) {
    return minimum_phase_kernel(magnitude, config.kernel_size);
  }

  std::vector<float> zero_phase(static_cast<size_t>(config.fft_size), 0.0f);
  fft.inverse(spectrum.data(), zero_phase.data());

  const size_t kernel_size = static_cast<size_t>(config.kernel_size);
  std::vector<float> kernel(kernel_size, 0.0f);
  const int half = config.kernel_size / 2;
  for (int i = 0; i < config.kernel_size; ++i) {
    const int source = (i - half + config.fft_size) % config.fft_size;
    kernel[static_cast<size_t>(i)] =
        zero_phase[static_cast<size_t>(source)] * hann_value(i, config.kernel_size);
  }

  return kernel;
}

Audio apply_match_eq(const Audio& audio, const ReferenceSpectrum& source,
                     const ReferenceSpectrum& reference, const MatchEqConfig& match_config,
                     const MatchEqFirConfig& fir_config) {
  if (audio.empty()) throw std::invalid_argument("audio must not be empty");
  if (audio.sample_rate() != source.sample_rate || source.sample_rate != reference.sample_rate) {
    throw std::invalid_argument("sample rates must match");
  }

  const MatchEqCurve curve = match_eq_curve(source, reference, match_config);
  const std::vector<float> kernel = match_eq_fir_kernel(curve, audio.sample_rate(), fir_config);
  const int latency_compensation =
      fir_config.phase == MatchEqFirPhase::LinearPhase ? static_cast<int>(kernel.size() / 2) : 0;
  std::vector<float> output =
      apply_fir_partitioned(audio, kernel, fir_config.partition_size, latency_compensation);
  return Audio::from_vector(std::move(output), audio.sample_rate());
}

float estimate_reference_delay_samples(const Audio& source, const Audio& reference,
                                       int max_abs_delay) {
  if (source.empty() || reference.empty()) {
    throw std::invalid_argument("audio must not be empty");
  }
  if (source.sample_rate() != reference.sample_rate()) {
    throw std::invalid_argument("sample rates must match");
  }
  if (max_abs_delay < 0) {
    throw std::invalid_argument("max_abs_delay must be non-negative");
  }

  const size_t length = std::min(source.size(), reference.size());
  const auto score_lag = [&](int lag) {
    double cross = 0.0;
    double source_energy = 0.0;
    double reference_energy = 0.0;
    const size_t source_start = lag < 0 ? static_cast<size_t>(-lag) : 0;
    const size_t reference_start = lag > 0 ? static_cast<size_t>(lag) : 0;
    const size_t count =
        length - static_cast<size_t>(std::min<int>(std::abs(lag), static_cast<int>(length)));
    for (size_t i = 0; i < count; ++i) {
      const float s = source[source_start + i];
      const float r = reference[reference_start + i];
      cross += static_cast<double>(s) * r;
      source_energy += static_cast<double>(s) * s;
      reference_energy += static_cast<double>(r) * r;
    }
    if (source_energy <= 0.0 || reference_energy <= 0.0) {
      return -1.0;
    }
    return cross / std::sqrt(source_energy * reference_energy);
  };

  int best_lag = 0;
  double best_score = -1.0;
  for (int lag = -max_abs_delay; lag <= max_abs_delay; ++lag) {
    if (static_cast<size_t>(std::abs(lag)) >= length) {
      continue;
    }
    const double score = score_lag(lag);
    if (score > best_score) {
      best_score = score;
      best_lag = lag;
    }
  }
  return static_cast<float>(best_lag);
}

Audio align_reference_to_source(const Audio& source, const Audio& reference, int max_abs_delay) {
  const int delay = static_cast<int>(
      std::round(estimate_reference_delay_samples(source, reference, max_abs_delay)));
  std::vector<float> aligned(source.size(), 0.0f);
  for (size_t i = 0; i < aligned.size(); ++i) {
    const int reference_index = static_cast<int>(i) + delay;
    if (reference_index >= 0 && reference_index < static_cast<int>(reference.size())) {
      aligned[i] = reference[static_cast<size_t>(reference_index)];
    }
  }
  return Audio::from_vector(std::move(aligned), source.sample_rate());
}

std::vector<eq::EqBand> match_eq_bands(const ReferenceSpectrum& source,
                                       const ReferenceSpectrum& reference,
                                       const MatchEqConfig& config) {
  if (config.max_bands == 0 || config.max_bands > eq::ParametricEq::kMaxBands) {
    throw std::invalid_argument("invalid match EQ band count");
  }
  if (!(config.max_gain_db >= 0.0f) || !(config.min_frequency_hz > 0.0f) ||
      !(config.max_frequency_hz > config.min_frequency_hz) || !(config.q > 0.0f)) {
    throw std::invalid_argument("invalid match EQ configuration");
  }
  if (source.sample_rate != reference.sample_rate) {
    throw std::invalid_argument("sample rates must match");
  }

  const MatchEqCurve curve = match_eq_curve(source, reference, config);
  if (curve.frequencies.empty()) {
    return {};
  }

  std::vector<eq::EqBand> bands;
  bands.reserve(config.max_bands);
  const float ratio = std::pow(config.max_frequency_hz / config.min_frequency_hz,
                               1.0f / static_cast<float>(config.max_bands));
  float frequency = config.min_frequency_hz;
  for (size_t i = 0; i < config.max_bands; ++i) {
    const float center = frequency * std::sqrt(ratio);
    ReferenceSpectrum curve_spectrum{curve.frequencies, curve.gain_db, source.sample_rate};
    const float diff = interpolate_db(curve_spectrum, center);
    bands.push_back({eq::EqBandType::Peak, center,
                     std::clamp(diff, -config.max_gain_db, config.max_gain_db), config.q, true});
    frequency *= ratio;
  }
  return bands;
}

}  // namespace sonare::mastering::match
