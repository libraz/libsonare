#include "mastering/maximizer/soft_knee_max.h"

#include <cmath>
#include <stdexcept>

namespace sonare::mastering::maximizer {

SoftKneeMax::SoftKneeMax(SoftKneeMaxConfig config) : config_(config) { validate_config(config_); }

void SoftKneeMax::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) throw std::invalid_argument("sample_rate must be positive");
  if (max_block_size < 0) throw std::invalid_argument("max_block_size must be non-negative");
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  maximizer_.set_config({0.0f, config_.ceiling_db, 1.0f, config_.release_ms});
  maximizer_.prepare(sample_rate_, max_block_size_);
  prepared_ = true;
}

void SoftKneeMax::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) throw std::logic_error("SoftKneeMax must be prepared before processing");
  if (num_channels < 0 || num_samples < 0) throw std::invalid_argument("invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr) throw std::invalid_argument("channels must not be null");
  const float drive = db_to_linear(config_.input_gain_db);
  const float knee = db_to_linear(config_.ceiling_db - config_.knee_db);
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) throw std::invalid_argument("channel buffer must not be null");
    for (int i = 0; i < num_samples; ++i) {
      float x = channels[ch][i] * drive;
      const float ax = std::abs(x);
      if (ax > knee && knee > 0.0f) {
        const float sign = x < 0.0f ? -1.0f : 1.0f;
        x = sign * (knee + std::tanh((ax - knee) / knee) * knee);
      }
      channels[ch][i] = x;
    }
  }
  maximizer_.process(channels, num_channels, num_samples);
}

void SoftKneeMax::reset() { maximizer_.reset(); }

void SoftKneeMax::set_config(const SoftKneeMaxConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) prepare(sample_rate_, max_block_size_);
}

void SoftKneeMax::validate_config(const SoftKneeMaxConfig& config) {
  if (config.knee_db < 0.0f || config.release_ms < 0.0f) {
    throw std::invalid_argument("invalid soft knee maximizer configuration");
  }
}

float SoftKneeMax::db_to_linear(float db) { return std::pow(10.0f, db / 20.0f); }

}  // namespace sonare::mastering::maximizer
