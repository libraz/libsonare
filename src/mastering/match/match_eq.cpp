#include "mastering/match/match_eq.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

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
