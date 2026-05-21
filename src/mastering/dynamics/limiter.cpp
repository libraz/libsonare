#include "mastering/dynamics/limiter.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::dynamics {

Limiter::Limiter(LimiterConfig config) : config_(config) { validate_config(config_); }

void Limiter::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }

  sample_rate_ = sample_rate;
  lookahead_samples_ = static_cast<int>(std::round(sample_rate_ * config_.lookahead_ms * 0.001));
  const double release_samples = sample_rate_ * std::max(config_.release_ms, 0.0f) * 0.001;
  release_coeff_ =
      release_samples <= 0.0 ? 0.0f : static_cast<float>(std::exp(-1.0 / release_samples));
  prepared_ = true;
  lookahead_.clear();
  gains_.clear();
  reset();
}

void Limiter::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) {
    throw std::logic_error("Limiter must be prepared before processing");
  }
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw std::invalid_argument("channels must not be null");
  }

  prepare_buffers(num_channels);
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw std::invalid_argument("channel buffer must not be null");
    }
  }

  const float ceiling = db_to_linear(config_.threshold_db);
  float min_gain = 1.0f;
  std::vector<float> delayed(static_cast<size_t>(num_channels), 0.0f);
  for (int i = 0; i < num_samples; ++i) {
    float peak = 0.0f;
    for (int ch = 0; ch < num_channels; ++ch) {
      delayed[static_cast<size_t>(ch)] =
          lookahead_[static_cast<size_t>(ch)].process(channels[ch][i]);
      peak = std::max(peak, lookahead_[static_cast<size_t>(ch)].peak());
    }

    const float target_gain = peak > ceiling && peak > 0.0f ? ceiling / peak : 1.0f;
    for (int ch = 0; ch < num_channels; ++ch) {
      auto& gain = gains_[static_cast<size_t>(ch)];
      if (target_gain < gain) {
        gain = target_gain;
      } else {
        gain = release_coeff_ * gain + (1.0f - release_coeff_) * target_gain;
      }
      channels[ch][i] = delayed[static_cast<size_t>(ch)] * gain;
      min_gain = std::min(min_gain, gain);
    }
  }

  last_gain_reduction_db_ = std::min(0.0f, linear_to_db(min_gain));
}

void Limiter::reset() {
  for (auto& buffer : lookahead_) {
    buffer.reset();
  }
  std::fill(gains_.begin(), gains_.end(), 1.0f);
  last_gain_reduction_db_ = 0.0f;
}

void Limiter::set_config(const LimiterConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    prepare(sample_rate_, 0);
  }
}

void Limiter::validate_config(const LimiterConfig& config) {
  if (config.lookahead_ms < 0.0f || config.release_ms < 0.0f) {
    throw std::invalid_argument("limiter timing values must be non-negative");
  }
}

float Limiter::db_to_linear(float db) { return std::pow(10.0f, db / 20.0f); }

float Limiter::linear_to_db(float value) {
  if (value <= 0.0f) {
    return -120.0f;
  }
  return 20.0f * std::log10(value);
}

void Limiter::prepare_buffers(int num_channels) {
  if (lookahead_.size() == static_cast<size_t>(num_channels)) {
    return;
  }

  lookahead_.assign(static_cast<size_t>(num_channels), {});
  gains_.assign(static_cast<size_t>(num_channels), 1.0f);
  for (auto& buffer : lookahead_) {
    buffer.prepare(static_cast<size_t>(std::max(lookahead_samples_, 0)));
  }
}

}  // namespace sonare::mastering::dynamics
