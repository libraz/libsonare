#include "mastering/stereo/stereo_balance.h"

#include <algorithm>

#include "rt/pan_law.h"
#include "rt/scoped_no_denormals.h"
#include "util/exception.h"

namespace sonare::mastering::stereo {

namespace {

using sonare::rt::compute_pan_gains;
using sonare::rt::PanGains;
using sonare::rt::PanLaw;
using sonare::rt::PanNormalization;

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
  prepared_ = true;
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

std::vector<rt::ParamDescriptor> StereoBalance::parameter_descriptors() const {
  return {{"balance", 0}};
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
      config.constant_power
          ? compute_pan_gains(config.balance, PanLaw::Const3dB, PanNormalization::CenterUnity)
          : compute_pan_gains(config.balance, PanLaw::Linear0dB, PanNormalization::NearUnity);
  left = g.left;
  right = g.right;
}

}  // namespace sonare::mastering::stereo
