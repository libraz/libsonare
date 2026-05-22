#include "mastering/eq/api_style.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sonare::mastering::eq {
namespace {

constexpr std::array<float, 7> kLowFrequencies = {30.0f,  40.0f,  50.0f, 100.0f,
                                                  200.0f, 300.0f, 400.0f};
constexpr std::array<float, 7> kLowMidFrequencies = {75.0f,  150.0f, 180.0f, 240.0f,
                                                     500.0f, 700.0f, 1000.0f};
constexpr std::array<float, 7> kHighMidFrequencies = {800.0f,  1500.0f,  3000.0f, 5000.0f,
                                                      8000.0f, 10000.0f, 12500.0f};
constexpr std::array<float, 7> kHighFrequencies = {2500.0f,  5000.0f,  7000.0f, 10000.0f,
                                                   12500.0f, 15000.0f, 20000.0f};
constexpr std::array<float, 13> kGainSteps = {-12.0f, -10.0f, -8.0f, -6.0f, -4.0f, -2.0f, 0.0f,
                                              2.0f,   4.0f,   6.0f,  8.0f,  10.0f, 12.0f};

}  // namespace

void ApiStyleEq::prepare(double sample_rate, int max_block_size) {
  eq_.prepare(sample_rate, max_block_size);
  bands_[index(Band::Low)].frequency_hz = 100.0f;
  bands_[index(Band::LowMid)].frequency_hz = 500.0f;
  bands_[index(Band::HighMid)].frequency_hz = 3000.0f;
  bands_[index(Band::High)].frequency_hz = 10000.0f;
  clear();
}

void ApiStyleEq::process(float* const* channels, int num_channels, int num_samples) {
  eq_.process(channels, num_channels, num_samples);
}

void ApiStyleEq::reset() { eq_.reset(); }

void ApiStyleEq::set_band(Band band, float frequency_hz, float gain_db) {
  const size_t i = index(band);
  bands_[i].frequency_hz = snapped_frequency(band, frequency_hz);
  bands_[i].gain_db = snapped_gain(gain_db);
  bands_[i].enabled = bands_[i].gain_db != 0.0f;
  rebuild_band(band);
}

void ApiStyleEq::clear_band(Band band) {
  const size_t i = index(band);
  bands_[i].gain_db = 0.0f;
  bands_[i].enabled = false;
  rebuild_band(band);
}

void ApiStyleEq::clear() {
  for (size_t i = 0; i < bands_.size(); ++i) {
    bands_[i].gain_db = 0.0f;
    bands_[i].enabled = false;
    eq_.clear_band(i);
  }
}

float ApiStyleEq::frequency(Band band) const { return bands_[index(band)].frequency_hz; }

float ApiStyleEq::gain_db(Band band) const { return bands_[index(band)].gain_db; }

float ApiStyleEq::snapped_frequency(Band band, float requested_frequency_hz) const {
  switch (band) {
    case Band::Low:
      return nearest(requested_frequency_hz, kLowFrequencies.data(), kLowFrequencies.size());
    case Band::LowMid:
      return nearest(requested_frequency_hz, kLowMidFrequencies.data(), kLowMidFrequencies.size());
    case Band::HighMid:
      return nearest(requested_frequency_hz, kHighMidFrequencies.data(),
                     kHighMidFrequencies.size());
    case Band::High:
      return nearest(requested_frequency_hz, kHighFrequencies.data(), kHighFrequencies.size());
  }
  return requested_frequency_hz;
}

float ApiStyleEq::snapped_gain(float requested_gain_db) const {
  return nearest(requested_gain_db, kGainSteps.data(), kGainSteps.size());
}

void ApiStyleEq::rebuild_band(Band band) {
  const size_t i = index(band);
  const auto& state = bands_[i];
  EqBandType type = EqBandType::Peak;
  if (band == Band::Low && state.frequency_hz <= 50.0f) {
    type = EqBandType::LowShelf;
  } else if (band == Band::High && state.frequency_hz >= 10000.0f) {
    type = EqBandType::HighShelf;
  }

  eq_.set_band(
      i, {type, state.frequency_hz, state.gain_db, proportional_q(state.gain_db), state.enabled});
}

size_t ApiStyleEq::index(Band band) { return static_cast<size_t>(band); }

float ApiStyleEq::nearest(float value, const float* values, size_t count) {
  size_t best_index = 0;
  float best_distance = std::abs(value - values[0]);
  for (size_t i = 1; i < count; ++i) {
    const float distance = std::abs(value - values[i]);
    if (distance < best_distance) {
      best_index = i;
      best_distance = distance;
    }
  }
  return values[best_index];
}

float ApiStyleEq::proportional_q(float gain_db) {
  return 0.55f + std::min(std::abs(gain_db), 12.0f) / 12.0f * 2.2f;
}

}  // namespace sonare::mastering::eq
