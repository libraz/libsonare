#include "mastering/eq/dynamic_eq.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace sonare::mastering::eq {
namespace {

float linear_to_db(float value) {
  if (value <= 0.0f) {
    return -120.0f;
  }
  return 20.0f * std::log10(value);
}

}  // namespace

void DynamicEq::prepare(double sample_rate, int max_block_size) {
  eq_.prepare(sample_rate, max_block_size);
  prepared_ = true;
  rebuild(last_detector_db_);
}

void DynamicEq::process(float* const* channels, int num_channels, int num_samples) {
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

  last_detector_db_ = detector_db(channels, num_channels, num_samples);
  rebuild(last_detector_db_);
  eq_.process(channels, num_channels, num_samples);
}

void DynamicEq::reset() {
  eq_.reset();
  last_detector_db_ = -120.0f;
  last_applied_gain_db_.fill(0.0f);
}

void DynamicEq::set_band(size_t index, const DynamicEqBand& band) {
  validate_index(index);
  validate_band(band);
  bands_[index] = band;
  if (prepared_) {
    rebuild(last_detector_db_);
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
}

float DynamicEq::detector_db(float* const* channels, int num_channels, int num_samples) {
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

void DynamicEq::rebuild(float detector_db) {
  for (size_t i = 0; i < kMaxBands; ++i) {
    const auto& dynamic_band = bands_[i];
    if (!dynamic_band.enabled) {
      eq_.clear_band(i);
      last_applied_gain_db_[i] = 0.0f;
      continue;
    }

    const float applied_gain =
        dynamic_band.static_gain_db + dynamic_gain_delta(dynamic_band, detector_db);
    last_applied_gain_db_[i] = applied_gain;
    eq_.set_band(
        i, {dynamic_band.type, dynamic_band.frequency_hz, applied_gain, dynamic_band.q, true});
  }
}

}  // namespace sonare::mastering::eq
