#include "mastering/saturation/soft_clipper.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "mastering/common/scoped_no_denormals.h"

namespace sonare::mastering::saturation {

SoftClipper::SoftClipper(SoftClipperConfig config) : config_(config) { validate_config(config_); }

void SoftClipper::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) throw std::invalid_argument("sample_rate must be positive");
  if (max_block_size < 0) throw std::invalid_argument("max_block_size must be non-negative");
  prepared_ = true;
}

void SoftClipper::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  if (!prepared_) throw std::logic_error("SoftClipper must be prepared before processing");
  if (num_channels < 0 || num_samples < 0) throw std::invalid_argument("invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr) throw std::invalid_argument("channels must not be null");
  ensure_state(num_channels);
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) throw std::invalid_argument("channel buffer must not be null");
    for (int i = 0; i < num_samples; ++i) {
      channels[ch][i] = process_sample(channels[ch][i], ch);
    }
  }
}

void SoftClipper::reset() {
  for (auto& state : tanh_adaa_) state.reset();
}

void SoftClipper::set_config(const SoftClipperConfig& config) {
  validate_config(config);
  const bool reset_state = config_.ceiling != config.ceiling || config_.aliasing != config.aliasing;
  config_ = config;
  if (reset_state) reset();
}

bool SoftClipper::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.drive_db = value;
      return true;
    case 1:
      config_.mix = std::clamp(value, 0.0f, 1.0f);
      return true;
    default:
      return false;
  }
}

void SoftClipper::validate_config(const SoftClipperConfig& config) {
  if (!(config.ceiling > 0.0f) || config.mix < 0.0f || config.mix > 1.0f) {
    throw std::invalid_argument("invalid soft clipper configuration");
  }
}

void SoftClipper::ensure_state(int num_channels) {
  if (tanh_adaa_.size() != static_cast<size_t>(num_channels)) {
    tanh_adaa_.clear();
    tanh_adaa_.resize(static_cast<size_t>(num_channels));
  }
}

float SoftClipper::process_sample(float sample, int channel) {
  const float drive = Waveshaper::db_to_linear(config_.drive_db);
  const float normalized = sample * drive / config_.ceiling;
  const float wet =
      config_.ceiling * (config_.aliasing == common::AliasingControl::Adaa1
                             ? tanh_adaa_[static_cast<size_t>(channel)].process(normalized)
                             : std::tanh(normalized));
  return sample * (1.0f - config_.mix) + wet * config_.mix;
}

}  // namespace sonare::mastering::saturation
