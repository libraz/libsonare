#include "mastering/saturation/transformer.h"

#include <algorithm>
#include <cmath>

#include "mastering/common/prepare_args.h"
#include "mastering/dynamics/channel_limits.h"
#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/non_finite_state.h"
#include "util/numeric_validation.h"

namespace sonare::mastering::saturation {

using sonare::discard_group_if_non_finite;

namespace {

struct TransformerScalars {
  float drive = 0.0f;
  float bias_field = 0.0f;
  float mix = 0.0f;
  float sample_rate = 0.0f;
};

TransformerScalars make_scalars(const TransformerConfig& config, double sample_rate) {
  return {db_to_linear(config.drive_db), 0.08f * config.asymmetry, config.mix,
          static_cast<float>(sample_rate)};
}

float process_sample_with_scalars(const common::JilesAtherton& hysteresis,
                                  common::JilesAthertonState& state, float input,
                                  const TransformerScalars& scalars) {
  const float wet =
      hysteresis.process(state, input * scalars.drive + scalars.bias_field, scalars.sample_rate) -
      scalars.bias_field;
  return input * (1.0f - scalars.mix) + wet * scalars.mix;
}

bool valid_mix(float v) noexcept { return v >= 0.0f && v <= 1.0f; }
bool valid_asymmetry(float v) noexcept { return v >= -1.0f && v <= 1.0f; }

}  // namespace

Transformer::Transformer(TransformerConfig config)
    : transformer_config_(config), hysteresis_(make_ja_config(config)) {
  validate_config(transformer_config_);
}

void Transformer::prepare(double sample_rate, int max_block_size) {
  validate_prepare_args(sample_rate, max_block_size);
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
  if (!validate_process_buffers(channels, num_channels, num_samples)) return;
  ensure_state(num_channels);
  const TransformerScalars scalars = make_scalars(transformer_config_, sample_rate_);

  bool discarded = false;
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr)
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
    auto& state = states_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      channels[ch][i] = process_sample_with_scalars(hysteresis_, state, channels[ch][i], scalars);
    }
    // Two floats per channel, once per block. The integrator takes its field
    // step from previous_field and advances magnetization from its own value, so
    // one non-finite sample leaves the core stranded for every later block. Rest
    // is the demagnetised state JilesAtherton::reset writes.
    discarded |= discard_group_if_non_finite(state.magnetization, state.previous_field);
  }
  if (discarded) note_non_finite_discard();
}

void Transformer::reset() {
  for (auto& state : states_) {
    common::JilesAtherton::reset(state);
  }
}

int Transformer::tail_samples() const noexcept {
  if (!(transformer_config_.mix > 0.0f)) return 0;
  if (transformer_config_.asymmetry != 0.0f) return rt::TailBudget::kUnbounded;

  const common::JilesAthertonConfig& ja = hysteresis_.config();
  const float coupling =
      ja.mean_field_coupling * ja.saturation_magnetization / (3.0f * ja.anhysteretic_shape);
  if (!(ja.viscosity_time_constant_s > 0.0f) || !(sample_rate_ > 0.0) ||
      !std::isfinite(sample_rate_) || !(coupling < 1.0f)) {
    return rt::TailBudget::kUnbounded;
  }

  // Match the float relaxation arithmetic in JilesAtherton::process().
  const float rate = static_cast<float>(sample_rate_);
  const float relax_rate = 1.0f - std::exp(-1.0f / (ja.viscosity_time_constant_s * rate));
  const float relax_pole = 1.0f - relax_rate * (1.0f - coupling);
  if (!(relax_pole > 0.0f)) return 1;
  if (!(relax_pole < 1.0f) || !std::isfinite(relax_pole)) {
    return rt::TailBudget::kUnbounded;
  }

  // The output bound includes magnetization scale and one field-transition sample.
  const double scale = 2.4 * static_cast<double>(ja.saturation_magnetization) *
                       static_cast<double>(transformer_config_.mix);
  rt::TailBudget tail;
  tail.delay(1.0).decay(static_cast<double>(relax_pole), scale);
  return tail.samples();
}

void Transformer::set_config(const TransformerConfig& config) {
  validate_config(config);
  // The magnetics receive segment-derived scalars; the hysteresis state carries over.
  rt::apply_config_diff(transformer_config_, config,
                        [this](const rt::ConfigDiff<TransformerConfig>&) {
                          hysteresis_.set_config(make_ja_config(transformer_config_));
                        });
}

bool Transformer::set_parameter_impl(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      if (!numeric::finite(db_to_linear(value))) return false;
      transformer_config_.drive_db = value;
      return true;
    case 1:
      if (!valid_asymmetry(value)) return false;
      transformer_config_.asymmetry = value;
      hysteresis_.set_config(make_ja_config(transformer_config_));
      return true;
    case 2:
      if (!valid_mix(value)) return false;
      transformer_config_.mix = value;
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> Transformer::parameter_descriptors() const {
  return {{"driveDb", 0}, {"asymmetry", 1}, {"mix", 2}};
}

void Transformer::validate_config(const TransformerConfig& config) {
  if (!valid_mix(config.mix) || !valid_asymmetry(config.asymmetry)) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid transformer configuration");
  }
  if (!numeric::finite(db_to_linear(config.drive_db))) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "transformer drive must produce a finite linear gain");
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

void Transformer::ensure_state(int num_channels) {
  require_prepared_channels(num_channels, states_.size(), "Transformer");
}

}  // namespace sonare::mastering::saturation
