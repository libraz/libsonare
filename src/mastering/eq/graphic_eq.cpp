#include "mastering/eq/graphic_eq.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::eq {
namespace {

constexpr std::array<float, GraphicEq::kNumBands> kCenterFrequencies = {
    20.0f,   25.0f,   31.5f,   40.0f,    50.0f,    63.0f,    80.0f,   100.0f,
    125.0f,  160.0f,  200.0f,  250.0f,   315.0f,   400.0f,   500.0f,  630.0f,
    800.0f,  1000.0f, 1250.0f, 1600.0f,  2000.0f,  2500.0f,  3150.0f, 4000.0f,
    5000.0f, 6300.0f, 8000.0f, 10000.0f, 12500.0f, 16000.0f, 20000.0f};

}  // namespace

void GraphicEq::prepare(double sample_rate, int max_block_size) {
  low_eq_.prepare(sample_rate, max_block_size);
  high_eq_.prepare(sample_rate, max_block_size);
  rebuild_bands();
}

void GraphicEq::process(float* const* channels, int num_channels, int num_samples) {
  low_eq_.process(channels, num_channels, num_samples);
  high_eq_.process(channels, num_channels, num_samples);
}

void GraphicEq::reset() {
  low_eq_.reset();
  high_eq_.reset();
}

void GraphicEq::set_gain_db(size_t index, float gain_db) {
  validate_index(index);
  gains_db_[index] = gain_db;
  rebuild_bands();
}

void GraphicEq::set_gain_for_frequency(float frequency_hz, float gain_db) {
  set_gain_db(nearest_band(frequency_hz), gain_db);
}

void GraphicEq::clear() {
  gains_db_.fill(0.0f);
  rebuild_bands();
}

float GraphicEq::gain_db(size_t index) const {
  validate_index(index);
  return gains_db_[index];
}

float GraphicEq::center_frequency(size_t index) const {
  validate_index(index);
  return kCenterFrequencies[index];
}

size_t GraphicEq::nearest_band(float frequency_hz) const {
  if (!(frequency_hz > 0.0f)) {
    throw std::invalid_argument("frequency_hz must be positive");
  }

  size_t best_index = 0;
  float best_distance = std::abs(std::log2(frequency_hz / kCenterFrequencies[0]));
  for (size_t i = 1; i < kCenterFrequencies.size(); ++i) {
    const float distance = std::abs(std::log2(frequency_hz / kCenterFrequencies[i]));
    if (distance < best_distance) {
      best_index = i;
      best_distance = distance;
    }
  }
  return best_index;
}

void GraphicEq::rebuild_bands() {
  low_eq_.clear();
  high_eq_.clear();

  for (size_t i = 0; i < ParametricEq::kMaxBands; ++i) {
    const bool enabled = gains_db_[i] != 0.0f;
    low_eq_.set_band(i, {EqBandType::Peak, kCenterFrequencies[i], gains_db_[i], 4.318f, enabled});
  }

  for (size_t i = ParametricEq::kMaxBands; i < kCenterFrequencies.size(); ++i) {
    const size_t high_index = i - ParametricEq::kMaxBands;
    const bool enabled = gains_db_[i] != 0.0f;
    high_eq_.set_band(high_index,
                      {EqBandType::Peak, kCenterFrequencies[i], gains_db_[i], 4.318f, enabled});
  }
}

void GraphicEq::validate_index(size_t index) {
  if (index >= kNumBands) {
    throw std::out_of_range("graphic EQ band index out of range");
  }
}

}  // namespace sonare::mastering::eq
