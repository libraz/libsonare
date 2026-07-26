#include "mastering/stereo/mono_maker.h"

#include <algorithm>
#include <cmath>

#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare::mastering::stereo {

MonoMaker::MonoMaker(MonoMakerConfig config) : config_(config) { validate_config(config_); }

void MonoMaker::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }
  sample_rate_ = sample_rate;
  update_coefficient();
  reset();
  prepared_ = true;
}

void MonoMaker::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "MonoMaker");
  if (num_channels < 0 || num_samples < 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
    }
  }
  if (num_channels < 2 || config_.amount == 0.0f) {
    return;
  }

  for (int i = 0; i < num_samples; ++i) {
    const float mid = (channels[0][i] + channels[1][i]) * 0.5f;
    const float side = (channels[0][i] - channels[1][i]) * 0.5f;
    float high_side = side;
    for (size_t stage = 0; stage < highpass_input_.size(); ++stage) {
      const float output =
          coefficient_ * (highpass_output_[stage] + high_side - highpass_input_[stage]);
      highpass_input_[stage] = high_side;
      highpass_output_[stage] = output;
      high_side = output;
    }
    const float retained_side = side + config_.amount * (high_side - side);
    channels[0][i] = mid + retained_side;
    channels[1][i] = mid - retained_side;
  }
}

void MonoMaker::reset() {
  highpass_input_.fill(0.0f);
  highpass_output_.fill(0.0f);
}

void MonoMaker::set_config(const MonoMakerConfig& config) {
  validate_config(config);
  config_ = config;
  update_coefficient();
}

bool MonoMaker::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.amount = std::clamp(value, 0.0f, 1.0f);
      return true;
    case 1:
      if (!std::isfinite(value) || value <= 0.0f) return false;
      config_.frequency_hz = value;
      update_coefficient();
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> MonoMaker::parameter_descriptors() const {
  return {{"amount", 0}, {"frequencyHz", 1}};
}

void MonoMaker::validate_config(const MonoMakerConfig& config) {
  if (!std::isfinite(config.amount) || config.amount < 0.0f || config.amount > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "mono maker amount must be in [0, 1]");
  }
  if (!std::isfinite(config.frequency_hz) || config.frequency_hz <= 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "mono maker frequency_hz must be positive");
  }
}

void MonoMaker::update_coefficient() noexcept {
  const double frequency =
      std::min<double>(config_.frequency_hz, 0.49 * std::max(sample_rate_, 1.0));
  coefficient_ = static_cast<float>(std::exp(-constants::kTwoPiD * frequency / sample_rate_));
}

}  // namespace sonare::mastering::stereo
