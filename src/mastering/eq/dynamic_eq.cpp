#include "mastering/eq/dynamic_eq.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "mastering/common/scoped_no_denormals.h"
#include "rt/biquad_design.h"
#include "util/constants.h"
#include "util/db.h"

namespace sonare::mastering::eq {

using sonare::constants::kFloorDb;
using sonare::constants::kTwoPiD;

void DynamicEq::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) throw std::invalid_argument("sample_rate must be positive");
  sample_rate_ = sample_rate;
  eq_.prepare(sample_rate, max_block_size);
  prepared_ = true;
  rebuild();
}

void DynamicEq::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  if (!prepared_) {
    throw std::logic_error("DynamicEq must be prepared before processing");
  }
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw std::invalid_argument("channels must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw std::invalid_argument("channel buffer must not be null");
    }
  }

  validate_sidechain(num_samples);
  const float* const* detector_channels = sidechain_channels_ != nullptr
                                              ? sidechain_channels_
                                              : const_cast<const float* const*>(channels);
  const int detector_num_channels =
      sidechain_channels_ != nullptr ? sidechain_num_channels_ : num_channels;
  last_detector_db_ = detector_db(detector_channels, detector_num_channels, num_samples);
  for (size_t i = 0; i < kMaxBands; ++i) {
    last_band_detector_db_[i] = bands_[i].enabled
                                    ? band_detector_db(detector_channels, detector_num_channels,
                                                       num_samples, sample_rate_, bands_[i])
                                    : kFloorDb;
  }
  rebuild(num_samples);
  eq_.process(channels, num_channels, num_samples);
  clear_sidechain();
}

void DynamicEq::reset() {
  eq_.reset();
  last_detector_db_ = kFloorDb;
  last_band_detector_db_.fill(kFloorDb);
  last_applied_gain_db_.fill(0.0f);
  smoothed_gain_db_.fill(0.0f);
  clear_sidechain();
}

void DynamicEq::set_band(size_t index, const DynamicEqBand& band) {
  validate_index(index);
  validate_band(band);
  bands_[index] = band;
  if (prepared_) {
    rebuild();
  }
}

void DynamicEq::clear_band(size_t index) {
  validate_index(index);
  bands_[index] = {};
  last_applied_gain_db_[index] = 0.0f;
  if (prepared_) {
    eq_.clear_band(index);
  }
}

void DynamicEq::clear() {
  for (size_t i = 0; i < kMaxBands; ++i) {
    clear_band(i);
  }
}

const DynamicEqBand& DynamicEq::band(size_t index) const {
  validate_index(index);
  return bands_[index];
}

float DynamicEq::last_applied_gain_db(size_t index) const {
  validate_index(index);
  return last_applied_gain_db_[index];
}

float DynamicEq::last_band_detector_db(size_t index) const {
  validate_index(index);
  return last_band_detector_db_[index];
}

void DynamicEq::set_sidechain(const float* const* channels, int num_channels, int num_samples) {
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("sidechain dimensions must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    clear_sidechain();
    return;
  }
  if (channels == nullptr) throw std::invalid_argument("sidechain channels must not be null");
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) throw std::invalid_argument("sidechain channel must not be null");
  }
  sidechain_channels_ = channels;
  sidechain_num_channels_ = num_channels;
  sidechain_num_samples_ = num_samples;
}

void DynamicEq::clear_sidechain() {
  sidechain_channels_ = nullptr;
  sidechain_num_channels_ = 0;
  sidechain_num_samples_ = 0;
}

bool DynamicEq::set_parameter(unsigned int param_id, float value) {
  const size_t band_index = param_id / kParamsPerBand;
  if (band_index >= kMaxBands) {
    return false;
  }
  DynamicEqBand& band = bands_[band_index];
  switch (param_id % kParamsPerBand) {
    case 0:
      // Clamp to the open interval (0 Hz, Nyquist) so coefficient design never
      // throws on the audio thread.
      band.frequency_hz =
          std::clamp(value, 1.0e-3f, static_cast<float>(sample_rate_ * 0.5) - 1.0e-3f);
      break;
    case 1:
      band.static_gain_db = value;
      break;
    case 2:
      band.q = std::max(value, 1.0e-6f);
      break;
    case 3:
      band.threshold_db = value;
      break;
    case 4:
      band.ratio = std::max(1.0f, value);
      break;
    case 5:
      band.range_db = value;
      break;
    case 6:
      band.sidechain_q = std::max(value, 1.0e-6f);
      break;
    case 7:
      // -1 (or any non-positive value) is the sentinel meaning "follow the band
      // frequency"; positive values set an explicit sidechain frequency.
      band.sidechain_freq_hz = value > 0.0f ? value : -1.0f;
      break;
    case 8:
      band.attack_ms = std::max(0.0f, value);
      break;
    case 9:
      band.release_ms = std::max(0.0f, value);
      break;
    case 10:
      band.lookahead_ms = std::max(0.0f, value);
      break;
    default:
      return false;
  }
  // Recompute the affected band's gain and biquad coefficients in place. rebuild
  // with num_samples == 0 applies the change immediately (no extra smoothing)
  // and routes through ParametricEq::set_band, which preserves filter state.
  // Disabled bands clear their EQ slot and stay silent until enabled.
  if (prepared_) {
    rebuild();
  }
  return true;
}

void DynamicEq::validate_index(size_t index) {
  if (index >= kMaxBands) {
    throw std::out_of_range("dynamic EQ band index out of range");
  }
}

void DynamicEq::validate_band(const DynamicEqBand& band) {
  if (!band.enabled) {
    return;
  }
  if (!(band.frequency_hz > 0.0f)) {
    throw std::invalid_argument("frequency_hz must be positive");
  }
  if (!(band.q > 0.0f)) {
    throw std::invalid_argument("Q must be positive");
  }
  if (!(band.ratio >= 1.0f)) {
    throw std::invalid_argument("ratio must be at least 1");
  }
  if (!(band.sidechain_q > 0.0f) || band.attack_ms < 0.0f || band.release_ms < 0.0f ||
      band.lookahead_ms < 0.0f ||
      (band.sidechain_freq_hz != -1.0f && band.sidechain_freq_hz <= 0.0f)) {
    throw std::invalid_argument("invalid dynamic EQ sidechain configuration");
  }
}

float DynamicEq::detector_db(const float* const* channels, int num_channels, int num_samples) {
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

float DynamicEq::band_detector_db(const float* const* channels, int num_channels, int num_samples,
                                  double sample_rate, const DynamicEqBand& band) {
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
      band.sidechain_freq_hz > 0.0f ? band.sidechain_freq_hz : band.frequency_hz;
  const double frequency =
      std::clamp(static_cast<double>(detector_frequency), 1.0, sample_rate * 0.5 - 1.0);
  const auto coeffs =
      rt::rbj_bandpass(static_cast<float>(kTwoPiD * frequency / sample_rate),
                       static_cast<float>(std::max(static_cast<double>(band.sidechain_q), 1.0e-6)));
  Biquad prototype;
  prototype.b0 = coeffs.b0;
  prototype.b1 = coeffs.b1;
  prototype.b2 = coeffs.b2;
  prototype.a1 = coeffs.a1;
  prototype.a2 = coeffs.a2;

  const double attack =
      band.attack_ms <= 0.0f
          ? 1.0
          : 1.0 - std::exp(-1.0 / std::max(sample_rate * band.attack_ms * 0.001, 1.0));
  const double release =
      band.release_ms <= 0.0f
          ? 1.0
          : 1.0 - std::exp(-1.0 / std::max(sample_rate * band.release_ms * 0.001, 1.0));
  const int lookahead_samples =
      static_cast<int>(std::round(sample_rate * band.lookahead_ms * 0.001));
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
  const double rms = std::sqrt(sum / std::max(count, 1.0));
  return linear_to_db(static_cast<float>(rms));
}

float DynamicEq::dynamic_gain_delta(const DynamicEqBand& band, float detector_db) {
  if (!band.enabled || detector_db <= band.threshold_db || band.range_db == 0.0f) {
    return 0.0f;
  }

  const float over_db = detector_db - band.threshold_db;
  const float compressed_db = over_db * (1.0f - 1.0f / band.ratio);
  const float range = std::abs(band.range_db);
  const float amount = std::min(range, compressed_db);
  return band.range_db < 0.0f ? -amount : amount;
}

void DynamicEq::rebuild(int num_samples) {
  for (size_t i = 0; i < kMaxBands; ++i) {
    const auto& dynamic_band = bands_[i];
    if (!dynamic_band.enabled) {
      eq_.clear_band(i);
      last_applied_gain_db_[i] = 0.0f;
      continue;
    }

    const float detector = last_band_detector_db_[i];
    const float target_gain =
        dynamic_band.static_gain_db + dynamic_gain_delta(dynamic_band, detector);
    float applied_gain = target_gain;
    if (prepared_ && num_samples > 0) {
      const double smoothing_samples = std::max(sample_rate_ * 0.005, 1.0);
      const float coeff =
          static_cast<float>(1.0 - std::exp(-static_cast<double>(num_samples) / smoothing_samples));
      smoothed_gain_db_[i] += coeff * (target_gain - smoothed_gain_db_[i]);
      applied_gain = smoothed_gain_db_[i];
    } else {
      smoothed_gain_db_[i] = target_gain;
    }
    last_applied_gain_db_[i] = applied_gain;
    eq_.set_band(
        i, {dynamic_band.type, dynamic_band.frequency_hz, applied_gain, dynamic_band.q, true});
  }
}

void DynamicEq::validate_sidechain(int expected_samples) const {
  if (sidechain_channels_ == nullptr) return;
  if (sidechain_num_samples_ != expected_samples) {
    throw std::invalid_argument("sidechain length must match process block length");
  }
}

}  // namespace sonare::mastering::eq
