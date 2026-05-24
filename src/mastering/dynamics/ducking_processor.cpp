#include "mastering/dynamics/ducking_processor.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::dynamics {

DuckingProcessor::DuckingProcessor(DuckingConfig config)
    : config_(config), router_(to_router_config(config)) {}

void DuckingProcessor::prepare(double sample_rate, int max_block_size) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  router_.prepare(sample_rate_, max_block_size);
}

void DuckingProcessor::process(float* const* channels, int num_channels, int num_samples) {
  router_.process(channels, num_channels, num_samples);
}

void DuckingProcessor::reset() { router_.reset(); }

int DuckingProcessor::latency_samples() const noexcept {
  return static_cast<int>(
      std::round(std::clamp(config_.lookahead_ms, 0.0f, 1000.0f) * 0.001f * sample_rate_));
}

void DuckingProcessor::set_key_input(const float* const* channels, int num_channels,
                                     int num_samples) {
  router_.set_sidechain(channels, num_channels, num_samples);
}

void DuckingProcessor::clear_key_input() { router_.clear_sidechain(); }

void DuckingProcessor::set_config(const DuckingConfig& config) {
  config_ = config;
  router_.set_config(to_router_config(config_));
}

SidechainRouterConfig DuckingProcessor::to_router_config(const DuckingConfig& config) {
  if (config.lookahead_ms < 0.0f) {
    throw std::invalid_argument("ducking lookahead must be non-negative");
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
