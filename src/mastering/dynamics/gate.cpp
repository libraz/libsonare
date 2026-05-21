#include "mastering/dynamics/gate.h"

#include <stdexcept>

namespace sonare::mastering::dynamics {

Gate::Gate(GateConfig config) : config_(config) { validate_config(config_); }

void Gate::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }

  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  expander_.set_config(
      {config_.threshold_db, 8.0f, config_.attack_ms, config_.release_ms, config_.range_db});
  expander_.prepare(sample_rate_, max_block_size_);
  prepared_ = true;
}

void Gate::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) {
    throw std::logic_error("Gate must be prepared before processing");
  }
  expander_.process(channels, num_channels, num_samples);
}

void Gate::reset() { expander_.reset(); }

void Gate::set_config(const GateConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    prepare(sample_rate_, max_block_size_);
  }
}

void Gate::validate_config(const GateConfig& config) {
  if (config.attack_ms < 0.0f || config.release_ms < 0.0f || config.range_db > 0.0f) {
    throw std::invalid_argument("invalid gate configuration");
  }
}

}  // namespace sonare::mastering::dynamics
