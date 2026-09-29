#include "mastering/stereo/stereo_balance.h"

#include <algorithm>
#include <cmath>

#include "rt/pan_law.h"
#include "rt/scoped_no_denormals.h"
#include "util/exception.h"

namespace sonare::mastering::stereo {

namespace {

using sonare::rt::compute_pan_gains;
using sonare::rt::PanGains;
using sonare::rt::PanLaw;
using sonare::rt::PanNormalization;

// Settling time of a balance change, in seconds.
constexpr double kRampSeconds = 0.005;

}  // namespace

StereoBalance::StereoBalance(StereoBalanceConfig config) : config_(config) {
  validate_config(config_);
}

void StereoBalance::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }
  ramp_samples_ = std::max(1, static_cast<int>(std::lround(kRampSeconds * sample_rate)));
  prepared_ = true;
  reset();
}

void StereoBalance::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "StereoBalance");
  if (!validate_process_buffers(channels, num_channels, num_samples)) {
    return;
  }
  if (num_channels < 2) {
    return;
  }

  float target_left = 1.0f;
  float target_right = 1.0f;
  gains(config_, target_left, target_right);
  if (!primed_) {
    left_gain_ = target_left_ = target_left;
    right_gain_ = target_right_ = target_right;
    ramp_remaining_ = 0;
    primed_ = true;
  } else if (target_left != target_left_ || target_right != target_right_) {
    // Retarget from wherever the gains are now, so a change mid-glide does not jump.
    target_left_ = target_left;
    target_right_ = target_right;
    ramp_remaining_ = ramp_samples_;
    step_left_ = (target_left_ - left_gain_) / static_cast<float>(ramp_samples_);
    step_right_ = (target_right_ - right_gain_) / static_cast<float>(ramp_samples_);
  }
  for (int i = 0; i < num_samples; ++i) {
    if (ramp_remaining_ > 0) {
      if (--ramp_remaining_ == 0) {
        left_gain_ = target_left_;
        right_gain_ = target_right_;
      } else {
        left_gain_ += step_left_;
        right_gain_ += step_right_;
      }
    }
    channels[0][i] *= left_gain_;
    channels[1][i] *= right_gain_;
  }
}

void StereoBalance::reset() {
  primed_ = false;
  ramp_remaining_ = 0;
}

void StereoBalance::set_config(const StereoBalanceConfig& config) {
  validate_config(config);
  config_ = config;
}

bool StereoBalance::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.balance = std::clamp(value, -1.0f, 1.0f);
      return true;
    case 1:
      // An unnamed value is refused rather than rounded onto a neighbour.
      if (value < 0.0f || value != std::floor(value) ||
          value >= static_cast<float>(kStereoBalanceLawCount)) {
        return false;
      }
      config_.law = static_cast<StereoBalanceLaw>(static_cast<int>(value));
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> StereoBalance::parameter_descriptors() const {
  return {{"balance", 0}, {"law", 1}};
}

void StereoBalance::validate_config(const StereoBalanceConfig& config) {
  if (config.balance < -1.0f || config.balance > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "stereo balance must be in [-1, 1]");
  }
}

void StereoBalance::gains(const StereoBalanceConfig& config, float& left, float& right) {
  // A balance control is unity at centre either way; the two settings differ in
  // what happens off centre. Constant power raises the near channel so the pair
  // keeps the input's stereo energy, while the linear balance leaves the near
  // channel alone and only pulls the away channel down.
  const PanGains g =
      config.law == StereoBalanceLaw::kRawConstantPower
          ? compute_pan_gains(config.balance, PanLaw::Const3dB, PanNormalization::Raw)
      : config.constant_power
          ? compute_pan_gains(config.balance, PanLaw::Const3dB, PanNormalization::CenterUnity)
          : compute_pan_gains(config.balance, PanLaw::Linear0dB, PanNormalization::NearUnity);
  left = g.left;
  right = g.right;
}

}  // namespace sonare::mastering::stereo
