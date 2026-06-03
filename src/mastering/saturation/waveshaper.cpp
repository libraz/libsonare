#include "mastering/saturation/waveshaper.h"

#include <algorithm>
#include <cmath>

#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::saturation {

namespace {

using sonare::constants::kPi;

}  // namespace

Waveshaper::Waveshaper(WaveshaperConfig config) : config_(config) { validate_config(config_); }

void Waveshaper::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0))
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  if (max_block_size < 0)
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  prepared_ = true;
}

void Waveshaper::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "Waveshaper");
  if (num_channels < 0 || num_samples < 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr)
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  ensure_state(num_channels);
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr)
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
    for (int i = 0; i < num_samples; ++i) channels[ch][i] = shape_sample(channels[ch][i], ch);
  }
}

void Waveshaper::reset() {
  for (auto& state : tanh_adaa_) state.reset();
  for (auto& state : arctan_adaa_) state.reset();
}

void Waveshaper::set_config(const WaveshaperConfig& config) {
  validate_config(config);
  const bool reset_state = config_.curve != config.curve || config_.aliasing != config.aliasing ||
                           config_.bias != config.bias;
  config_ = config;
  if (reset_state) reset();
}

bool Waveshaper::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.drive_db = value;
      return true;
    case 1:
      config_.mix = std::clamp(value, 0.0f, 1.0f);
      return true;
    case 2:
      config_.output_gain_db = value;
      return true;
    default:
      return false;
  }
}

float Waveshaper::db_to_linear(float db) { return ::sonare::db_to_linear(db); }

float Waveshaper::shape(float sample, const WaveshaperConfig& config) {
  const float driven = sample * db_to_linear(config.drive_db) + config.bias;
  float wet = driven;
  switch (config.curve) {
    case WaveshaperCurve::Tanh:
      wet = std::tanh(driven);
      break;
    case WaveshaperCurve::Arctan:
      wet = (2.0f / kPi) * std::atan(driven);
      break;
    case WaveshaperCurve::Asymmetric:
      wet = std::tanh(driven + 0.35f * driven * driven);
      break;
  }
  wet *= db_to_linear(config.output_gain_db);
  return sample * (1.0f - config.mix) + wet * config.mix;
}

void Waveshaper::validate_config(const WaveshaperConfig& config) {
  if (config.mix < 0.0f || config.mix > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "waveshaper mix must be in [0, 1]");
  }
  // ADAA1 (first-order antiderivative anti-aliasing) is only implemented for the
  // odd Tanh/Arctan curves; the Asymmetric curve has no ADAA antiderivative
  // here and there is no oversampling fallback in this processor. Rather than
  // silently degrading to direct (aliasing-prone) evaluation in release builds
  // (where the historical assert is compiled out), reject the unsupported
  // combination so callers get a clear, actionable error instead of audible
  // aliasing they cannot detect.
  if (config.curve == WaveshaperCurve::Asymmetric &&
      config.aliasing == sonare::rt::AliasingControl::Adaa1) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "waveshaper ADAA1 anti-aliasing is not supported for the Asymmetric curve; "
        "use AliasingControl::None or a Tanh/Arctan curve");
  }
}

void Waveshaper::ensure_state(int num_channels) {
  if (tanh_adaa_.size() != static_cast<size_t>(num_channels)) {
    tanh_adaa_.clear();
    arctan_adaa_.clear();
    tanh_adaa_.resize(static_cast<size_t>(num_channels));
    arctan_adaa_.resize(static_cast<size_t>(num_channels));
  }
}

float Waveshaper::shape_sample(float sample, int channel) {
  // ADAA1 (first-order antiderivative anti-aliasing) is only implemented for the
  // odd Tanh/Arctan curves. The Asymmetric + Adaa1 combination is rejected up
  // front by validate_config(), so it can never reach this point; the
  // Asymmetric branch below remains only as defensive direct evaluation for any
  // future non-Adaa1 aliasing mode that is not separately handled.
  if (config_.aliasing != sonare::rt::AliasingControl::Adaa1 ||
      config_.curve == WaveshaperCurve::Asymmetric) {
    return shape(sample, config_);
  }

  const float driven = sample * db_to_linear(config_.drive_db) + config_.bias;
  float wet = driven;
  switch (config_.curve) {
    case WaveshaperCurve::Tanh:
      wet = tanh_adaa_[static_cast<size_t>(channel)].process(driven);
      break;
    case WaveshaperCurve::Arctan:
      wet = (2.0f / kPi) * arctan_adaa_[static_cast<size_t>(channel)].process(driven);
      break;
    case WaveshaperCurve::Asymmetric:
      break;
  }
  wet *= db_to_linear(config_.output_gain_db);
  return sample * (1.0f - config_.mix) + wet * config_.mix;
}

}  // namespace sonare::mastering::saturation
