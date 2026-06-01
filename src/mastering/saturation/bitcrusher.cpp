#include "mastering/saturation/bitcrusher.h"

#include <algorithm>
#include <cmath>

#include "rt/scoped_no_denormals.h"
#include "util/exception.h"

namespace sonare::mastering::saturation {
namespace {

constexpr std::array<float, 9> kNoiseShapingCoeffs = {2.412f,  -3.370f, 3.937f,  -4.174f, 3.353f,
                                                      -2.205f, 1.281f,  -0.569f, 0.0847f};

}  // namespace

BitCrusher::BitCrusher(BitCrusherConfig config) : config_(config) { validate_config(config_); }

void BitCrusher::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0))
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  if (max_block_size < 0)
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  prepared_ = true;
  reset();
}

void BitCrusher::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "BitCrusher");
  if (num_channels < 0 || num_samples < 0)
    throw SonareException(ErrorCode::InvalidParameter, "invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr)
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  ensure_state(num_channels);
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr)
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
    for (int i = 0; i < num_samples; ++i) {
      if (counters_[static_cast<size_t>(ch)] == 0) {
        held_[static_cast<size_t>(ch)] = quantize(channels[ch][i], config_.bit_depth, ch);
      }
      counters_[static_cast<size_t>(ch)] =
          (counters_[static_cast<size_t>(ch)] + 1) % config_.downsample_factor;
      channels[ch][i] =
          channels[ch][i] * (1.0f - config_.mix) + held_[static_cast<size_t>(ch)] * config_.mix;
    }
  }
}

void BitCrusher::reset() {
  std::fill(held_.begin(), held_.end(), 0.0f);
  std::fill(counters_.begin(), counters_.end(), 0);
  for (size_t ch = 0; ch < rng_state_.size(); ++ch) {
    rng_state_[ch] = config_.dither_seed + static_cast<uint32_t>(ch * 747796405u);
  }
  for (auto& history : error_history_) {
    history.fill(0.0f);
  }
}

void BitCrusher::set_config(const BitCrusherConfig& config) {
  validate_config(config);
  config_ = config;
}

bool BitCrusher::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      // Match validate_config's [1, 24] integer range for bit_depth.
      config_.bit_depth = std::clamp(static_cast<int>(std::lround(value)), 1, 24);
      return true;
    case 1:
      config_.mix = std::clamp(value, 0.0f, 1.0f);
      return true;
    default:
      return false;
  }
}

void BitCrusher::validate_config(const BitCrusherConfig& config) {
  if (config.bit_depth < 1 || config.bit_depth > 24 || config.downsample_factor < 1 ||
      config.mix < 0.0f || config.mix > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid bitcrusher configuration");
  }
}

float BitCrusher::quantize(float sample, int bit_depth, int channel) {
  const float levels = std::pow(2.0f, static_cast<float>(bit_depth - 1)) - 1.0f;
  float shaped = 0.0f;
  if (config_.dither_type == final::DitherType::NoiseShaped) {
    auto& history = error_history_[static_cast<size_t>(channel)];
    for (size_t i = 0; i < history.size(); ++i) {
      shaped += kNoiseShapingCoeffs[i] * history[i];
    }
  }
  const float dithered = sample + dither_noise(channel) / std::max(levels, 1.0f) + shaped / levels;
  const float quantized = std::round(std::clamp(dithered, -1.0f, 1.0f) * levels) / levels;
  if (config_.dither_type == final::DitherType::NoiseShaped) {
    auto& history = error_history_[static_cast<size_t>(channel)];
    const float error = (dithered - quantized) * levels;
    for (size_t i = history.size() - 1; i > 0; --i) {
      history[i] = history[i - 1];
    }
    history[0] = error;
  }
  return quantized;
}

float BitCrusher::dither_noise(int channel) {
  if (config_.dither_type == final::DitherType::None) {
    return 0.0f;
  }
  auto& state = rng_state_[static_cast<size_t>(channel)];
  const auto uniform = [&state] {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>((state >> 8) & 0x00FFFFFFu) / 16777216.0f - 0.5f;
  };
  if (config_.dither_type == final::DitherType::Rpdf) {
    return uniform();
  }
  return uniform() + uniform();
}

void BitCrusher::ensure_state(int num_channels) {
  if (held_.size() != static_cast<size_t>(num_channels)) {
    const auto size = static_cast<size_t>(num_channels);
    held_.assign(size, 0.0f);
    counters_.assign(size, 0);
    rng_state_.assign(size, 0);
    error_history_.assign(size, {});
    reset();
  }
}

}  // namespace sonare::mastering::saturation
