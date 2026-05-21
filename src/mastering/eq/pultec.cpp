#include "mastering/eq/pultec.h"

#include <algorithm>
#include <stdexcept>

namespace sonare::mastering::eq {

void PultecEq::prepare(double sample_rate, int max_block_size) {
  eq_.prepare(sample_rate, max_block_size);
  rebuild();
}

void PultecEq::process(float* const* channels, int num_channels, int num_samples) {
  eq_.process(channels, num_channels, num_samples);
}

void PultecEq::reset() { eq_.reset(); }

void PultecEq::set_low_frequency(float frequency_hz) {
  low_frequency_hz_ = validate_frequency(frequency_hz);
  rebuild();
}

void PultecEq::set_low_boost(float amount) {
  low_boost_ = clamp_amount(amount);
  rebuild();
}

void PultecEq::set_low_attenuation(float amount) {
  low_attenuation_ = clamp_amount(amount);
  rebuild();
}

void PultecEq::set_high_boost(float frequency_hz, float amount, float bandwidth) {
  high_boost_frequency_hz_ = validate_frequency(frequency_hz);
  high_boost_ = clamp_amount(amount);
  high_bandwidth_ = std::clamp(bandwidth, 0.0f, 1.0f);
  rebuild();
}

void PultecEq::set_high_attenuation(float frequency_hz, float amount) {
  high_attenuation_frequency_hz_ = validate_frequency(frequency_hz);
  high_attenuation_ = clamp_amount(amount);
  rebuild();
}

void PultecEq::clear() {
  low_boost_ = 0.0f;
  low_attenuation_ = 0.0f;
  high_boost_ = 0.0f;
  high_attenuation_ = 0.0f;
  rebuild();
}

void PultecEq::rebuild() {
  eq_.clear();

  const bool low_boost_enabled = low_boost_ > 0.0f;
  const bool low_atten_enabled = low_attenuation_ > 0.0f;
  const bool high_boost_enabled = high_boost_ > 0.0f;
  const bool high_atten_enabled = high_attenuation_ > 0.0f;

  eq_.set_band(
      0, {EqBandType::LowShelf, low_frequency_hz_, low_boost_ * 1.7f, 0.65f, low_boost_enabled});
  eq_.set_band(1, {EqBandType::Peak, low_frequency_hz_ * 1.45f, -low_attenuation_ * 1.2f, 0.75f,
                   low_atten_enabled});

  const float high_q = 0.6f + high_bandwidth_ * 3.0f;
  eq_.set_band(2, {EqBandType::Peak, high_boost_frequency_hz_, high_boost_ * 1.5f, high_q,
                   high_boost_enabled});
  eq_.set_band(3, {EqBandType::HighShelf, high_attenuation_frequency_hz_, -high_attenuation_ * 1.2f,
                   0.7f, high_atten_enabled});
}

float PultecEq::clamp_amount(float amount) { return std::clamp(amount, 0.0f, 10.0f); }

float PultecEq::validate_frequency(float frequency_hz) {
  if (!(frequency_hz > 0.0f)) {
    throw std::invalid_argument("frequency_hz must be positive");
  }
  return frequency_hz;
}

}  // namespace sonare::mastering::eq
