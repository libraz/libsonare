#include "mastering/match/match_eq.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/fft.h"

namespace sonare::mastering::match {
namespace {

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

float db_to_gain(float db) { return std::pow(10.0f, db / 20.0f); }

float hann(size_t index, size_t length) {
  if (length <= 1) return 1.0f;
  return static_cast<float>(
      0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * static_cast<double>(index) /
                           static_cast<double>(length - 1)));
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

  if (config.smoothing_bins > 0 && curve.gain_db.size() > 2) {
    std::vector<float> smoothed(curve.gain_db.size(), 0.0f);
    for (size_t i = 0; i < curve.gain_db.size(); ++i) {
      const size_t begin = i > static_cast<size_t>(config.smoothing_bins)
                               ? i - static_cast<size_t>(config.smoothing_bins)
                               : 0;
      const size_t end =
          std::min(curve.gain_db.size(), i + static_cast<size_t>(config.smoothing_bins) + 1);
      float sum = 0.0f;
      for (size_t j = begin; j < end; ++j) sum += curve.gain_db[j];
      smoothed[i] = sum / static_cast<float>(end - begin);
    }
    curve.gain_db = std::move(smoothed);
  }
  return curve;
}

std::vector<float> match_eq_fir_kernel(const MatchEqCurve& curve, int sample_rate,
                                       const MatchEqFirConfig& config) {
  if (curve.frequencies.empty() || curve.frequencies.size() != curve.gain_db.size()) {
    throw std::invalid_argument("invalid match EQ curve");
  }
  if (sample_rate <= 0 || !is_power_of_two(config.fft_size) || config.kernel_size <= 0 ||
      config.kernel_size > config.fft_size || (config.kernel_size % 2) == 0) {
    throw std::invalid_argument("invalid match EQ FIR configuration");
  }

  FFT fft(config.fft_size);
  std::vector<std::complex<float>> spectrum(static_cast<size_t>(fft.n_bins()));
  const ReferenceSpectrum curve_spectrum{curve.frequencies, curve.gain_db, sample_rate};
  for (int bin = 0; bin < fft.n_bins(); ++bin) {
    const float frequency = static_cast<float>(bin) * static_cast<float>(sample_rate) /
                            static_cast<float>(config.fft_size);
    spectrum[static_cast<size_t>(bin)] = {db_to_gain(interpolate_db(curve_spectrum, frequency)),
                                          0.0f};
  }

  std::vector<float> zero_phase(static_cast<size_t>(config.fft_size), 0.0f);
  fft.inverse(spectrum.data(), zero_phase.data());

  const size_t kernel_size = static_cast<size_t>(config.kernel_size);
  std::vector<float> kernel(kernel_size, 0.0f);
  const int half = config.kernel_size / 2;
  for (int i = 0; i < config.kernel_size; ++i) {
    const int source = (i - half + config.fft_size) % config.fft_size;
    kernel[static_cast<size_t>(i)] =
        zero_phase[static_cast<size_t>(source)] * hann(static_cast<size_t>(i), kernel_size);
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
  const int half = static_cast<int>(kernel.size() / 2);
  std::vector<float> output(audio.size(), 0.0f);
  for (size_t i = 0; i < audio.size(); ++i) {
    double sum = 0.0;
    for (size_t k = 0; k < kernel.size(); ++k) {
      const int source_index = static_cast<int>(i) + static_cast<int>(k) - half;
      if (source_index < 0 || source_index >= static_cast<int>(audio.size())) continue;
      sum += static_cast<double>(kernel[k]) * audio[static_cast<size_t>(source_index)];
    }
    output[i] = static_cast<float>(sum);
  }
  return Audio::from_vector(std::move(output), audio.sample_rate());
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
