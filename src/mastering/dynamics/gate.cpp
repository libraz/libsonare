#include "mastering/dynamics/gate.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "util/constants.h"
#include "util/db.h"
#include "util/dsp_primitives.h"

namespace sonare::mastering::dynamics {

namespace {
using sonare::constants::kTwoPi;
}  // namespace

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
  const float cutoff = config_.key_hpf_hz;
  if (cutoff > 0.0f) {
    const float rc =
        1.0f / (kTwoPi * std::clamp(cutoff, 1.0f, static_cast<float>(sample_rate_ * 0.49)));
    const float dt = 1.0f / static_cast<float>(sample_rate_);
    hpf_coeff_ = rc / (rc + dt);
  }
  prepared_ = true;
  reset();
}

void Gate::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) {
    throw std::logic_error("Gate must be prepared before processing");
  }
  if (num_channels < 0 || num_samples < 0) throw std::invalid_argument("invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr) throw std::invalid_argument("channels must not be null");
  if (hpf_x1_.size() != static_cast<size_t>(num_channels)) {
    hpf_x1_.assign(static_cast<size_t>(num_channels), 0.0f);
    hpf_y1_.assign(static_cast<size_t>(num_channels), 0.0f);
  }
  const float attack = coeff(sample_rate_, config_.attack_ms);
  const float release = coeff(sample_rate_, config_.release_ms);
  const int hold_samples =
      static_cast<int>(sample_rate_ * static_cast<double>(config_.hold_ms) * 0.001);
  last_gain_reduction_db_ = 0.0f;
  for (int i = 0; i < num_samples; ++i) {
    float detector = 0.0f;
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] == nullptr) throw std::invalid_argument("channel buffer must not be null");
      float s = channels[ch][i];
      if (config_.key_hpf_hz > 0.0f) {
        const auto idx = static_cast<size_t>(ch);
        const float y = hpf_coeff_ * (hpf_y1_[idx] + s - hpf_x1_[idx]);
        hpf_x1_[idx] = s;
        hpf_y1_[idx] = y;
        s = y;
      }
      detector = std::max(detector, std::abs(s));
    }
    const float level_db = linear_to_db(detector);
    if (level_db >= config_.threshold_db) {
      hold_samples_remaining_ = hold_samples;
    } else if (hold_samples_remaining_ > 0) {
      --hold_samples_remaining_;
    }
    if (level_db >= config_.threshold_db) {
      gate_open_ = true;
    } else if (hold_samples_remaining_ == 0 && level_db < config_.close_threshold_db) {
      gate_open_ = false;
    }
    const bool open = gate_open_ || hold_samples_remaining_ > 0;
    const float target_db = open ? 0.0f : config_.range_db;
    const float c = target_db > gain_db_ ? attack : release;
    gain_db_ = c * gain_db_ + (1.0f - c) * target_db;
    const float gain = db_to_linear(gain_db_);
    for (int ch = 0; ch < num_channels; ++ch) channels[ch][i] *= gain;
    last_gain_reduction_db_ = std::min(last_gain_reduction_db_, gain_db_);
  }
}

void Gate::reset() {
  gain_db_ = 0.0f;
  last_gain_reduction_db_ = 0.0f;
  hold_samples_remaining_ = 0;
  gate_open_ = false;
  std::fill(hpf_x1_.begin(), hpf_x1_.end(), 0.0f);
  std::fill(hpf_y1_.begin(), hpf_y1_.end(), 0.0f);
}

void Gate::set_config(const GateConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    prepare(sample_rate_, max_block_size_);
  }
}

void Gate::validate_config(const GateConfig& config) {
  if (config.attack_ms < 0.0f || config.release_ms < 0.0f || config.range_db > 0.0f ||
      config.hold_ms < 0.0f || config.key_hpf_hz < 0.0f ||
      config.close_threshold_db > config.threshold_db) {
    throw std::invalid_argument("invalid gate configuration");
  }
}

float Gate::coeff(double sample_rate, float ms) { return time_to_coefficient(sample_rate, ms); }

}  // namespace sonare::mastering::dynamics
