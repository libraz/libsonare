#include "mastering/saturation/exciter.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "util/constants.h"
#include "util/db.h"

namespace sonare::mastering::saturation {

namespace {
using sonare::constants::kPiD;
}

float Exciter::Biquad::process(float x) {
  const float y = b0 * x + z1;
  z1 = b1 * x - a1 * y + z2;
  z2 = b2 * x - a2 * y;
  return y;
}

void Exciter::Biquad::reset() {
  z1 = 0.0f;
  z2 = 0.0f;
}

Exciter::Exciter(ExciterConfig config) : config_(config) { validate_config(config_); }

void Exciter::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) throw std::invalid_argument("sample_rate must be positive");
  if (max_block_size < 0) throw std::invalid_argument("max_block_size must be non-negative");
  sample_rate_ = sample_rate;
  prepared_ = true;
  update_coeff();
  reset();
}

void Exciter::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) throw std::logic_error("Exciter must be prepared before processing");
  if (num_channels < 0 || num_samples < 0) throw std::invalid_argument("invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr) throw std::invalid_argument("channels must not be null");
  ensure_state(num_channels);
  const float drive = db_to_linear(config_.drive_db);
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) throw std::invalid_argument("channel buffer must not be null");
    auto& bandpass = bandpass_[static_cast<size_t>(ch)];
    auto& allpass = allpass_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      const float band = bandpass.process(channels[ch][i]);
      const float aligned = allpass.process(band);
      const float even = (band * band) * (band < 0.0f ? -1.0f : 1.0f);
      const float odd = std::tanh(band * drive);
      const float harmonic =
          (1.0f - config_.even_odd_mix) * even * drive + config_.even_odd_mix * odd;
      channels[ch][i] += aligned * 0.05f * config_.amount + harmonic * config_.amount;
    }
  }
}

void Exciter::reset() {
  for (auto& filter : bandpass_) filter.reset();
  for (auto& filter : allpass_) filter.reset();
}

void Exciter::set_config(const ExciterConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    update_coeff();
  }
}

void Exciter::validate_config(const ExciterConfig& config) {
  if (!(config.frequency_hz > 0.0f) || config.amount < 0.0f || !(config.q > 0.0f) ||
      config.even_odd_mix < 0.0f || config.even_odd_mix > 1.0f) {
    throw std::invalid_argument("invalid exciter configuration");
  }
}

void Exciter::update_coeff() {
  const float cutoff =
      std::clamp(config_.frequency_hz, 10.0f, static_cast<float>(sample_rate_ * 0.49));
  const float w0 = static_cast<float>(2.0 * kPiD * cutoff / sample_rate_);
  const float alpha = std::sin(w0) / (2.0f * config_.q);
  const float cosw = std::cos(w0);
  const float a0 = 1.0f + alpha;
  bandpass_coeffs_.b0 = alpha / a0;
  bandpass_coeffs_.b1 = 0.0f;
  bandpass_coeffs_.b2 = -alpha / a0;
  bandpass_coeffs_.a1 = -2.0f * cosw / a0;
  bandpass_coeffs_.a2 = (1.0f - alpha) / a0;
  allpass_coeffs_.b0 = (1.0f - alpha) / a0;
  allpass_coeffs_.b1 = -2.0f * cosw / a0;
  allpass_coeffs_.b2 = 1.0f;
  allpass_coeffs_.a1 = -2.0f * cosw / a0;
  allpass_coeffs_.a2 = (1.0f - alpha) / a0;
  for (auto& filter : bandpass_) filter = bandpass_coeffs_;
  for (auto& filter : allpass_) filter = allpass_coeffs_;
}

void Exciter::ensure_state(int num_channels) {
  if (bandpass_.size() != static_cast<size_t>(num_channels)) {
    bandpass_.assign(static_cast<size_t>(num_channels), bandpass_coeffs_);
    allpass_.assign(static_cast<size_t>(num_channels), allpass_coeffs_);
  }
}

}  // namespace sonare::mastering::saturation
