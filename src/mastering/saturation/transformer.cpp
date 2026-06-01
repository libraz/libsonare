#include "mastering/saturation/transformer.h"

#include <algorithm>
#include <cmath>

#include "mastering/common/scoped_no_denormals.h"
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
  prepared_ = true;
  reset();
}

void Transformer::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
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

void Transformer::validate_config(const TransformerConfig& config) {
  if (config.mix < 0.0f || config.mix > 1.0f || config.asymmetry < -1.0f ||
      config.asymmetry > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid transformer configuration");
  }
}

common::JilesAthertonConfig Transformer::make_ja_config(const TransformerConfig& config) {
  auto ja = common::jiles_atherton_presets::silicon_steel();
  ja.coercivity = std::max(0.02f, ja.coercivity * (1.0f + 0.35f * std::abs(config.asymmetry)));
  return ja;
}

float Transformer::process_sample(common::JilesAthertonState& state, float input) const {
  const float drive = db_to_linear(transformer_config_.drive_db);
  const float bias_field = 0.08f * transformer_config_.asymmetry;
  const float wet = hysteresis_.process(state, input * drive + bias_field) - bias_field;
  const float mix = transformer_config_.mix;
  return input * (1.0f - mix) + wet * mix;
}

void Transformer::ensure_state(int num_channels) {
  if (states_.size() != static_cast<size_t>(num_channels)) {
    states_.assign(static_cast<size_t>(num_channels), common::JilesAthertonState{});
  }
}

}  // namespace sonare::mastering::saturation
