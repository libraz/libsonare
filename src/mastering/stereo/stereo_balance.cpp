#include "mastering/stereo/stereo_balance.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "mastering/common/scoped_no_denormals.h"
#include "util/constants.h"

namespace sonare::mastering::stereo {

namespace {

using sonare::constants::kHalfPi;
using sonare::constants::kSqrt2;

}  // namespace

StereoBalance::StereoBalance(StereoBalanceConfig config) : config_(config) {
  validate_config(config_);
}

void StereoBalance::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }
  prepared_ = true;
}

void StereoBalance::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "StereoBalance");
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw std::invalid_argument("channels must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw std::invalid_argument("channel buffer must not be null");
    }
  }
  if (num_channels < 2) {
    return;
  }

  float left_gain = 1.0f;
  float right_gain = 1.0f;
  gains(config_, left_gain, right_gain);
  for (int i = 0; i < num_samples; ++i) {
    channels[0][i] *= left_gain;
    channels[1][i] *= right_gain;
  }
}

void StereoBalance::reset() {}

void StereoBalance::set_config(const StereoBalanceConfig& config) {
  validate_config(config);
  config_ = config;
}

bool StereoBalance::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.balance = std::clamp(value, -1.0f, 1.0f);
      return true;
    default:
      return false;
  }
}

void StereoBalance::validate_config(const StereoBalanceConfig& config) {
  if (config.balance < -1.0f || config.balance > 1.0f) {
    throw std::invalid_argument("stereo balance must be in [-1, 1]");
  }
}

void StereoBalance::gains(const StereoBalanceConfig& config, float& left, float& right) {
  const float balance = std::clamp(config.balance, -1.0f, 1.0f);
  if (config.constant_power) {
    const float angle = (balance + 1.0f) * 0.5f * kHalfPi;
    left = std::cos(angle) * kSqrt2;
    right = std::sin(angle) * kSqrt2;
  } else {
    left = balance > 0.0f ? 1.0f - balance : 1.0f;
    right = balance < 0.0f ? 1.0f + balance : 1.0f;
  }
}

}  // namespace sonare::mastering::stereo
