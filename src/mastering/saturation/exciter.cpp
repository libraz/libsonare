#include "mastering/saturation/exciter.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::saturation {

namespace {
constexpr double kPi = 3.14159265358979323846;
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
    auto& low = lowpass_state_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      low = lowpass_coeff_ * low + (1.0f - lowpass_coeff_) * channels[ch][i];
      const float high = channels[ch][i] - low;
      channels[ch][i] += std::tanh(high * drive) * config_.amount;
    }
  }
}

void Exciter::reset() { std::fill(lowpass_state_.begin(), lowpass_state_.end(), 0.0f); }

void Exciter::set_config(const ExciterConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    update_coeff();
    reset();
  }
}

void Exciter::validate_config(const ExciterConfig& config) {
  if (!(config.frequency_hz > 0.0f) || config.amount < 0.0f) {
    throw std::invalid_argument("invalid exciter configuration");
  }
}

float Exciter::db_to_linear(float db) { return std::pow(10.0f, db / 20.0f); }

void Exciter::update_coeff() {
  const float cutoff =
      std::clamp(config_.frequency_hz, 10.0f, static_cast<float>(sample_rate_ * 0.49));
  lowpass_coeff_ = static_cast<float>(std::exp(-2.0 * kPi * cutoff / sample_rate_));
}

void Exciter::ensure_state(int num_channels) {
  if (lowpass_state_.size() != static_cast<size_t>(num_channels)) {
    lowpass_state_.assign(static_cast<size_t>(num_channels), 0.0f);
  }
}

}  // namespace sonare::mastering::saturation
