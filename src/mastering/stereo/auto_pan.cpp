#include "mastering/stereo/auto_pan.h"

#include <algorithm>
#include <cmath>

#include "rt/pan_law.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare::mastering::stereo {

namespace {

using sonare::constants::kTwoPiD;
using sonare::rt::compute_pan_gains;
using sonare::rt::PanGains;
using sonare::rt::PanLaw;
using sonare::rt::PanNormalization;

}  // namespace

AutoPan::AutoPan(AutoPanConfig config) : config_(config) { validate_config(config_); }

void AutoPan::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }
  sample_rate_ = sample_rate;
  prepared_ = true;
  reset();
}

void AutoPan::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "AutoPan");
  if (!validate_process_buffers(channels, num_channels, num_samples)) {
    return;
  }
  if (num_channels < 2) {
    return;
  }

  // Centre-unity constant power: the pair carries the input's stereo energy
  // unchanged (left^2 + right^2 == 2) at every LFO position, and a centred pan —
  // which is the whole signal at depth 0 — passes both channels at unity. The
  // law's raw gains would instead land on 1/sqrt(2) at centre, so merely
  // inserting the panner would cost 3 dB and depth 0 would not be a bypass.
  const double increment = config_.rate_hz / sample_rate_;
  for (int i = 0; i < num_samples; ++i) {
    const float pan =
        static_cast<float>(std::sin((phase_ + config_.phase) * kTwoPiD)) * config_.depth;
    const PanGains g = compute_pan_gains(pan, PanLaw::Const3dB, PanNormalization::CenterUnity);
    channels[0][i] *= g.left;
    channels[1][i] *= g.right;
    phase_ += increment;
    phase_ -= std::floor(phase_);
  }
}

void AutoPan::reset() { phase_ = 0.0; }

void AutoPan::set_config(const AutoPanConfig& config) {
  validate_config(config);
  config_ = config;
}

bool AutoPan::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.rate_hz = std::max(0.0f, value);
      return true;
    case 1:
      config_.depth = std::clamp(value, 0.0f, 1.0f);
      return true;
    case 2:
      config_.phase = value;
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> AutoPan::parameter_descriptors() const {
  return {{"rateHz", 0}, {"depth", 1}, {"phase", 2}};
}

void AutoPan::validate_config(const AutoPanConfig& config) {
  if (config.rate_hz < 0.0f || config.depth < 0.0f || config.depth > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid auto pan configuration");
  }
}

}  // namespace sonare::mastering::stereo
