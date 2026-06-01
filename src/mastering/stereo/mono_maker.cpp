#include "mastering/stereo/mono_maker.h"

#include <algorithm>

#include "rt/scoped_no_denormals.h"
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
    const float mono = (channels[0][i] + channels[1][i]) * 0.5f;
    channels[0][i] += (mono - channels[0][i]) * config_.amount;
    channels[1][i] += (mono - channels[1][i]) * config_.amount;
  }
}

void MonoMaker::reset() {}

void MonoMaker::set_config(const MonoMakerConfig& config) {
  validate_config(config);
  config_ = config;
}

bool MonoMaker::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.amount = std::clamp(value, 0.0f, 1.0f);
      return true;
    default:
      return false;
  }
}

void MonoMaker::validate_config(const MonoMakerConfig& config) {
  if (config.amount < 0.0f || config.amount > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "mono maker amount must be in [0, 1]");
  }
}

}  // namespace sonare::mastering::stereo
