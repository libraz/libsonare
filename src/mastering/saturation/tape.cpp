#include "mastering/saturation/tape.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::saturation {

Tape::Tape(TapeConfig config) : config_(config) { validate_config(config_); }

void Tape::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) throw std::invalid_argument("sample_rate must be positive");
  if (max_block_size < 0) throw std::invalid_argument("max_block_size must be non-negative");
  prepared_ = true;
  reset();
}

void Tape::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) throw std::logic_error("Tape must be prepared before processing");
  if (num_channels < 0 || num_samples < 0) throw std::invalid_argument("invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr) throw std::invalid_argument("channels must not be null");
  ensure_state(num_channels);
  const float drive = db_to_linear(config_.drive_db);
  const float output = db_to_linear(config_.output_gain_db);
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) throw std::invalid_argument("channel buffer must not be null");
    auto& state = hysteresis_state_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      state += (channels[ch][i] - state) * config_.hysteresis;
      const float driven = (channels[ch][i] + state * 0.15f) * drive;
      const float wet = std::tanh(driven * (1.0f + config_.saturation * 2.0f)) * output;
      channels[ch][i] = channels[ch][i] * (1.0f - config_.saturation) + wet * config_.saturation;
    }
  }
}

void Tape::reset() { std::fill(hysteresis_state_.begin(), hysteresis_state_.end(), 0.0f); }

void Tape::set_config(const TapeConfig& config) {
  validate_config(config);
  config_ = config;
}

void Tape::validate_config(const TapeConfig& config) {
  if (config.saturation < 0.0f || config.saturation > 1.0f || config.hysteresis < 0.0f ||
      config.hysteresis > 1.0f) {
    throw std::invalid_argument("invalid tape configuration");
  }
}

float Tape::db_to_linear(float db) { return std::pow(10.0f, db / 20.0f); }

void Tape::ensure_state(int num_channels) {
  if (hysteresis_state_.size() != static_cast<size_t>(num_channels)) {
    hysteresis_state_.assign(static_cast<size_t>(num_channels), 0.0f);
  }
}

}  // namespace sonare::mastering::saturation
