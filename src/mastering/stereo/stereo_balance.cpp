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

  const bool law_changed =
      config_.law != to_law_.law || config_.constant_power != to_law_.constant_power;
  if (!primed_) {
    position_ = target_position_ = config_.balance;
    from_law_ = to_law_ = config_;
    law_blend_ = 1.0f;
    ramp_remaining_ = 0;
    gains(config_, left_gain_, right_gain_);
    primed_ = true;
  } else if (config_.balance != target_position_ || law_changed) {
    // Retarget from wherever the glide is now, so a change mid-glide does not jump.
    const float ramp = static_cast<float>(ramp_samples_);
    target_position_ = config_.balance;
    step_position_ = (target_position_ - position_) / ramp;
    if (law_changed) {
      from_law_ = law_blend_ < 0.5f ? from_law_ : to_law_;
      to_law_ = config_;
      law_blend_ = 0.0f;
      step_law_blend_ = 1.0f / ramp;
    }
    ramp_remaining_ = ramp_samples_;
  }
  for (int i = 0; i < num_samples; ++i) {
    if (ramp_remaining_ > 0) {
      if (--ramp_remaining_ == 0) {
        position_ = target_position_;
        law_blend_ = 1.0f;
      } else {
        position_ += step_position_;
        law_blend_ = std::min(1.0f, law_blend_ + step_law_blend_);
      }
      glide_gains(left_gain_, right_gain_);
    }
    channels[0][i] *= left_gain_;
    channels[1][i] *= right_gain_;
  }
}

void StereoBalance::glide_gains(float& left, float& right) const {
  StereoBalanceConfig at = to_law_;
  at.balance = position_;
  gains(at, left, right);
  if (law_blend_ >= 1.0f) return;
  StereoBalanceConfig leaving = from_law_;
  leaving.balance = position_;
  float from_left = 1.0f;
  float from_right = 1.0f;
  gains(leaving, from_left, from_right);
  left = from_left + law_blend_ * (left - from_left);
  right = from_right + law_blend_ * (right - from_right);
}

void StereoBalance::reset() {
  primed_ = false;
  ramp_remaining_ = 0;
}

void StereoBalance::set_config(const StereoBalanceConfig& config) {
  validate_config(config);
  // Every field is read per sample; nothing is rebuilt.
  rt::apply_config_diff(config_, config, [](const rt::ConfigDiff<StereoBalanceConfig>&) {});
}

bool StereoBalance::set_parameter_impl(unsigned int param_id, float value) {
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
