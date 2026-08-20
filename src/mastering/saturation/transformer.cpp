#include "mastering/saturation/transformer.h"

#include <algorithm>
#include <cmath>

#include "mastering/dynamics/channel_limits.h"
#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::saturation {

Transformer::Transformer(TransformerConfig config)
    : transformer_config_(config), hysteresis_(make_ja_config(config)) {
  validate_config(transformer_config_);
}

void Transformer::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0))
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  if (max_block_size < 0)
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  sample_rate_ = sample_rate;
  prepared_ = true;
  // Preallocate per-channel hysteresis state so process() never resizes on the
  // audio thread (matches Tube/AmpSim).
  states_.assign(dynamics::kRealtimePreparedChannels, common::JilesAthertonState{});
  reset();
}

void Transformer::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "Transformer");
  if (num_channels < 0 || num_samples < 0)
    throw SonareException(ErrorCode::InvalidParameter, "invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr)
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  ensure_state(num_channels);

  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr)
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
    auto& state = states_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      channels[ch][i] = process_sample(state, channels[ch][i]);
    }
  }
}

void Transformer::reset() {
  for (auto& state : states_) {
    common::JilesAtherton::reset(state);
  }
}

void Transformer::set_config(const TransformerConfig& config) {
  validate_config(config);
  transformer_config_ = config;
  hysteresis_.set_config(make_ja_config(config));
}

bool Transformer::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      transformer_config_.drive_db = value;
      return true;
    case 1:
      transformer_config_.asymmetry = std::clamp(value, -1.0f, 1.0f);
      hysteresis_.set_config(make_ja_config(transformer_config_));
      return true;
    case 2:
      transformer_config_.mix = std::clamp(value, 0.0f, 1.0f);
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> Transformer::parameter_descriptors() const {
  return {{"driveDb", 0}, {"asymmetry", 1}, {"mix", 2}};
}

void Transformer::validate_config(const TransformerConfig& config) {
  if (config.mix < 0.0f || config.mix > 1.0f || config.asymmetry < -1.0f ||
      config.asymmetry > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid transformer configuration");
  }
}

common::JilesAthertonConfig Transformer::make_ja_config(const TransformerConfig& config) {
  auto ja = common::jiles_atherton_presets::silicon_steel();
  ja.coercivity = std::max(0.02f, ja.coercivity * (1.0f + 0.35f * std::abs(config.asymmetry)));
  // Sub-step the core once a field change reaches the coercivity, which is the
  // scale at which a single Euler step stops tracking the loop. Transformer has
  // no oversampling option, so this is its only guard against a fast field
  // change running the magnetization into the saturation clamp.
  ja.max_field_step = ja.coercivity;
  return ja;
}

float Transformer::process_sample(common::JilesAthertonState& state, float input) const {
  const float drive = db_to_linear(transformer_config_.drive_db);
  const float bias_field = 0.08f * transformer_config_.asymmetry;
  const float wet =
      hysteresis_.process(state, input * drive + bias_field, static_cast<float>(sample_rate_)) -
      bias_field;
  const float mix = transformer_config_.mix;
  return input * (1.0f - mix) + wet * mix;
}

void Transformer::ensure_state(int num_channels) {
  // prepare() preallocates kRealtimePreparedChannels; only grow (control thread)
  // if a caller exceeds it, preserving existing channels' hysteresis state.
  if (states_.size() < static_cast<size_t>(num_channels)) {
    states_.resize(static_cast<size_t>(num_channels), common::JilesAthertonState{});
  }
}

}  // namespace sonare::mastering::saturation
