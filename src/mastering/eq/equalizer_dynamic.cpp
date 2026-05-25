#include <algorithm>
#include <cmath>

#include "mastering/eq/equalizer.h"
#include "util/constants.h"
#include "util/db.h"

namespace sonare::mastering::eq {

using sonare::constants::kFloorDb;
using sonare::constants::kTwoPiD;

float EqualizerProcessor::detector_db(const float* const* channels, int num_channels,
                                      int num_samples) {
  double sum = 0.0;
  for (int ch = 0; ch < num_channels; ++ch) {
    for (int i = 0; i < num_samples; ++i) {
      const double sample = channels[ch][i];
      sum += sample * sample;
    }
  }
  const double count = static_cast<double>(num_channels) * static_cast<double>(num_samples);
  return linear_to_db(static_cast<float>(std::sqrt(sum / std::max(count, 1.0))));
}

float EqualizerProcessor::band_detector_db(const float* const* channels, int num_channels,
                                           int num_samples, double sample_rate,
                                           const EqBand& band) {
  if (num_samples <= 0 || num_channels <= 0) return kFloorDb;
  struct Biquad {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
    double z1 = 0.0;
    double z2 = 0.0;

    double process(double x) {
      const double y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }
  };

  const double detector_frequency =
      band.dyn.sidechain_freq_hz > 0.0f ? band.dyn.sidechain_freq_hz : band.frequency_hz;
  const double frequency =
      std::clamp(static_cast<double>(detector_frequency), 1.0, sample_rate * 0.5 - 1.0);
  const double omega = kTwoPiD * frequency / sample_rate;
  const double sin_w0 = std::sin(omega);
  const double cos_w0 = std::cos(omega);
  const double alpha = sin_w0 / (2.0 * std::max(static_cast<double>(band.dyn.sidechain_q), 1.0e-6));
  const double a0 = 1.0 + alpha;
  Biquad prototype;
  prototype.b0 = alpha / a0;
  prototype.b1 = 0.0;
  prototype.b2 = -alpha / a0;
  prototype.a1 = -2.0 * cos_w0 / a0;
  prototype.a2 = (1.0 - alpha) / a0;

  const double attack =
      band.dyn.attack_ms <= 0.0f
          ? 1.0
          : 1.0 - std::exp(-1.0 / std::max(sample_rate * band.dyn.attack_ms * 0.001, 1.0));
  const double release =
      band.dyn.release_ms <= 0.0f
          ? 1.0
          : 1.0 - std::exp(-1.0 / std::max(sample_rate * band.dyn.release_ms * 0.001, 1.0));
  const int lookahead_samples =
      static_cast<int>(std::round(sample_rate * band.dyn.lookahead_ms * 0.001));

  double sum = 0.0;
  for (int ch = 0; ch < num_channels; ++ch) {
    Biquad filter_a = prototype;
    Biquad filter_b = prototype;
    double envelope = 0.0;
    for (int i = 0; i < num_samples; ++i) {
      const int src = std::min(num_samples - 1, i + std::max(lookahead_samples, 0));
      const double rectified = std::abs(filter_b.process(filter_a.process(channels[ch][src])));
      const double coeff = rectified > envelope ? attack : release;
      envelope += coeff * (rectified - envelope);
      sum += envelope * envelope;
    }
  }
  const double count = static_cast<double>(num_channels) * static_cast<double>(num_samples);
  return linear_to_db(static_cast<float>(std::sqrt(sum / std::max(count, 1.0))));
}

float EqualizerProcessor::rms_db(const float* const* channels, int num_channels,
                                 int num_samples) noexcept {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return kFloorDb;
  }
  double sum = 0.0;
  size_t count = 0;
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      continue;
    }
    for (int i = 0; i < num_samples; ++i) {
      const double sample = channels[ch][i];
      sum += sample * sample;
      ++count;
    }
  }
  return count == 0 ? kFloorDb
                    : linear_to_db(static_cast<float>(std::sqrt(sum / static_cast<double>(count))));
}

float EqualizerProcessor::dynamic_gain_delta(const EqBand& band, float detector_db,
                                             float threshold_db) {
  if (!band.enabled || !band.dyn.enabled || detector_db <= threshold_db ||
      band.dyn.range_db == 0.0f) {
    return 0.0f;
  }

  const float over_db = detector_db - threshold_db;
  const float compressed_db = over_db * (1.0f - 1.0f / band.dyn.ratio);
  const float range = std::abs(band.dyn.range_db);
  const float amount = std::min(range, compressed_db);
  return band.dyn.range_db < 0.0f ? -amount : amount;
}

void EqualizerProcessor::update_dynamic_state(const float* const* channels, int num_channels,
                                              int num_samples) {
  last_detector_db_ = detector_db(channels, num_channels, num_samples);
  for (size_t i = 0; i < kMaxBands; ++i) {
    const auto& band = bands_[i];
    if (band.enabled && band.dyn.enabled) {
      last_band_detector_db_[i] =
          band_detector_db(channels, num_channels, num_samples, sample_rate_, band);
      if (band.dyn.auto_threshold) {
        const float target = last_band_detector_db_[i] - 6.0f;
        if (auto_threshold_db_[i] <= kFloorDb + 1.0f) {
          auto_threshold_db_[i] = target;
        } else {
          const double smoothing_samples = std::max(sample_rate_ * 0.250, 1.0);
          const float coeff = static_cast<float>(
              1.0 - std::exp(-static_cast<double>(num_samples) / smoothing_samples));
          auto_threshold_db_[i] += coeff * (target - auto_threshold_db_[i]);
        }
      }
    } else {
      last_band_detector_db_[i] = kFloorDb;
      last_applied_gain_db_[i] = 0.0f;
    }
  }
}

void EqualizerProcessor::apply_auto_gain(float* const* channels, int num_channels, int num_samples,
                                         float input_db) noexcept {
  if (!auto_gain_enabled_ || channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    last_auto_gain_db_ = 0.0f;
    return;
  }
  const float output_db =
      rms_db(const_cast<const float* const*>(channels), num_channels, num_samples);
  if (input_db <= kFloorDb + 1.0f || output_db <= kFloorDb + 1.0f) {
    last_auto_gain_db_ = 0.0f;
    return;
  }
  const float target_db = std::clamp(input_db - output_db, -24.0f, 24.0f);
  const double smoothing_samples = std::max(sample_rate_ * 0.050, 1.0);
  const float coeff =
      static_cast<float>(1.0 - std::exp(-static_cast<double>(num_samples) / smoothing_samples));
  smoothed_auto_gain_db_ += coeff * (target_db - smoothed_auto_gain_db_);
  last_auto_gain_db_ = smoothed_auto_gain_db_;
  const float gain = db_to_linear(last_auto_gain_db_);
  for (int ch = 0; ch < num_channels; ++ch) {
    for (int i = 0; i < num_samples; ++i) {
      channels[ch][i] *= gain;
    }
  }
}
}  // namespace sonare::mastering::eq
