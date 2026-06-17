#include "mastering/dynamics/ducking_processor.h"

#include <algorithm>

#include "rt/scoped_no_denormals.h"
#include "util/exception.h"

namespace sonare::mastering::dynamics {

DuckingProcessor::DuckingProcessor(DuckingConfig config)
    : config_(config), router_(to_router_config(config)) {}

void DuckingProcessor::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  sample_rate_ = sample_rate;
  router_.prepare(sample_rate_, max_block_size);
}

void DuckingProcessor::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  router_.process(channels, num_channels, num_samples);
}

void DuckingProcessor::reset() { router_.reset(); }

int DuckingProcessor::latency_samples() const noexcept { return router_.latency_samples(); }

void DuckingProcessor::set_key_input(const float* const* channels, int num_channels,
                                     int num_samples) {
  router_.set_sidechain(channels, num_channels, num_samples);
}

void DuckingProcessor::clear_key_input() { router_.clear_sidechain(); }

void DuckingProcessor::set_config(const DuckingConfig& config) {
  // Apply to the router first: if to_router_config / set_config rejects the
  // values, config_ is left untouched rather than diverging from the router.
  router_.set_config(to_router_config(config));
  config_ = config;
}

bool DuckingProcessor::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.threshold_db = value;
      break;
    case 1:
      config_.ratio = std::max(1.0f, value);
      value = config_.ratio;
      break;
    case 2:
      config_.attack_ms = std::max(0.0f, value);
      value = config_.attack_ms;
      break;
    case 3:
      config_.release_ms = std::max(0.0f, value);
      value = config_.release_ms;
      break;
    case 4:
      config_.range_db = std::max(0.0f, value);
      value = config_.range_db;
      break;
    default:
      return false;
  }
  // Ducking parameters map 1:1 to the inner router; forward the clamped value so
  // the router recomputes only the affected coefficients without resetting state.
  return router_.set_parameter(param_id, value);
}

std::vector<rt::ParamDescriptor> DuckingProcessor::parameter_descriptors() const {
  return {{"thresholdDb", 0}, {"ratio", 1}, {"attackMs", 2}, {"releaseMs", 3}, {"rangeDb", 4}};
}

SidechainRouterConfig DuckingProcessor::to_router_config(const DuckingConfig& config) {
  if (config.lookahead_ms < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "ducking lookahead must be non-negative");
  }
  SidechainRouterConfig router_config;
  router_config.threshold_db = config.threshold_db;
  router_config.ratio = config.ratio;
  router_config.attack_ms = config.attack_ms;
  router_config.release_ms = config.release_ms;
  router_config.range_db = config.range_db;
  router_config.lookahead_ms = config.lookahead_ms;
  return router_config;
}

}  // namespace sonare::mastering::dynamics
