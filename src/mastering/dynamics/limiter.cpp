#include "mastering/dynamics/limiter.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "mastering/common/scoped_no_denormals.h"
#include "util/db.h"
#include "util/dsp_primitives.h"

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
  update_release_coeff();
  prepared_ = true;
  lookahead_.clear();
  gain_smoothers_.clear();
  reset();
}

void Limiter::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
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
      auto& gain_smoother = gain_smoothers_[static_cast<size_t>(ch)];
      const float gain = gain_smoother.smooth_bidirectional(target_gain, release_coeff_, true);
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
  for (auto& smoother : gain_smoothers_) {
    smoother.reset(1.0f);
  }
  last_gain_reduction_db_ = 0.0f;
}

void Limiter::set_config(const LimiterConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    prepare(sample_rate_, 0);
  }
}

void Limiter::set_release_ms(float release_ms) {
  if (release_ms < 0.0f) {
    throw std::invalid_argument("limiter release must be non-negative");
  }
  config_.release_ms = release_ms;
  if (prepared_) {
    update_release_coeff();
  }
}

bool Limiter::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.threshold_db = value;
      return true;
    case 1:
      config_.release_ms = std::max(0.0f, value);
      // Recompute the release coefficient in place; preserves gain-smoother state.
      if (prepared_) {
        update_release_coeff();
      }
      return true;
    default:
      return false;
  }
}

void Limiter::validate_config(const LimiterConfig& config) {
  if (config.lookahead_ms < 0.0f || config.release_ms < 0.0f) {
    throw std::invalid_argument("limiter timing values must be non-negative");
  }
}

void Limiter::prepare_buffers(int num_channels) {
  if (lookahead_.size() == static_cast<size_t>(num_channels)) {
    return;
  }

  lookahead_.assign(static_cast<size_t>(num_channels), {});
  gain_smoothers_.assign(static_cast<size_t>(num_channels), {});
  for (auto& buffer : lookahead_) {
    buffer.prepare(static_cast<size_t>(std::max(lookahead_samples_, 0)));
  }
  for (auto& smoother : gain_smoothers_) {
    smoother.prepare(sample_rate_, 0.0f, config_.release_ms);
    smoother.reset(1.0f);
  }
}

void Limiter::update_release_coeff() {
  release_coeff_ = time_to_coefficient(sample_rate_, config_.release_ms);
}

}  // namespace sonare::mastering::dynamics
