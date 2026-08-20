#include "mastering/saturation/waveshaper.h"

#include <algorithm>
#include <cmath>

#include "mastering/dynamics/channel_limits.h"
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
  max_block_size_ = max_block_size;
  prepared_ = true;
  // Preallocate per-channel ADAA state so process() never resizes on the audio
  // thread (matches Tube/AmpSim).
  tanh_adaa_.clear();
  tanh_adaa_.resize(dynamics::kRealtimePreparedChannels);
  arctan_adaa_.clear();
  arctan_adaa_.resize(dynamics::kRealtimePreparedChannels);
  // Preallocate the Oversample4x scratch, per-channel streaming state, and the
  // dry-path delay so the audio-thread process() path never allocates.
  const size_t scratch =
      static_cast<size_t>(max_block_size_) * static_cast<size_t>(kOversampleFactor);
  up_scratch_.assign(scratch, 0.0f);
  down_scratch_.assign(static_cast<size_t>(max_block_size_), 0.0f);
  oversampler_states_.resize(dynamics::kRealtimePreparedChannels);
  for (auto& state : oversampler_states_) {
    oversampler_.prepare_streaming(&state, static_cast<size_t>(max_block_size_));
  }
  dry_delays_.resize(dynamics::kRealtimePreparedChannels);
  for (auto& delay : dry_delays_) {
    delay.prepare(static_cast<size_t>(oversampler_.streaming_round_trip_latency_samples()));
  }
  reset();
}

void Waveshaper::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "Waveshaper");
  if (!validate_process_buffers(channels, num_channels, num_samples)) {
    return;
  }
  ensure_state(num_channels);

  if (config_.aliasing != sonare::rt::AliasingControl::Oversample4x) {
    for (int ch = 0; ch < num_channels; ++ch) {
      for (int i = 0; i < num_samples; ++i) channels[ch][i] = shape_sample(channels[ch][i], ch);
    }
    return;
  }

  // Oversampled path. Reuse the preallocated scratch buffers (sized in
  // prepare()) so this audio-thread path never allocates; reject blocks wider
  // than the prepared size instead of resizing here. Curve + drive + bias run
  // at the oversampled rate; output gain and the dry/wet mix run at the base
  // rate after downsampling, matching shape()'s split.
  const float drive = db_to_linear(config_.drive_db);
  const float output_gain = db_to_linear(config_.output_gain_db);
  const size_t os_samples =
      static_cast<size_t>(num_samples) * static_cast<size_t>(kOversampleFactor);
  if (os_samples > up_scratch_.size() || static_cast<size_t>(num_samples) > down_scratch_.size()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_samples exceeds prepared Waveshaper oversampling scratch");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    const float* input = channels[ch];
    auto& state = oversampler_states_[static_cast<size_t>(ch)];
    oversampler_.upsample_to_streaming(input, static_cast<size_t>(num_samples), up_scratch_.data(),
                                       up_scratch_.size(), &state);
    for (size_t i = 0; i < os_samples; ++i) {
      const float driven = up_scratch_[i] * drive + config_.bias;
      up_scratch_[i] = apply_curve(driven, config_.curve);
    }
    oversampler_.downsample_to_streaming(up_scratch_.data(), os_samples, down_scratch_.data(),
                                         down_scratch_.size(), &state);
    for (int i = 0; i < num_samples; ++i) {
      const float dry = dry_delays_[static_cast<size_t>(ch)].process(input[i]);
      const float wet = down_scratch_[static_cast<size_t>(i)] * output_gain;
      channels[ch][i] = dry * (1.0f - config_.mix) + wet * config_.mix;
    }
  }
}

void Waveshaper::reset() {
  for (auto& state : tanh_adaa_) state.reset();
  for (auto& state : arctan_adaa_) state.reset();
  for (auto& state : oversampler_states_) oversampler_.reset_streaming(&state);
  for (auto& delay : dry_delays_) delay.reset();
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

std::vector<rt::ParamDescriptor> Waveshaper::parameter_descriptors() const {
  return {{"driveDb", 0}, {"mix", 1}, {"outputGainDb", 2}};
}

float Waveshaper::db_to_linear(float db) { return ::sonare::db_to_linear(db); }

float Waveshaper::apply_curve(float driven, WaveshaperCurve curve) {
  switch (curve) {
    case WaveshaperCurve::Tanh:
      return std::tanh(driven);
    case WaveshaperCurve::Arctan:
      return (2.0f / kPi) * std::atan(driven);
    case WaveshaperCurve::Asymmetric:
      return std::tanh(driven + 0.35f * driven * driven);
  }
  return driven;
}

float Waveshaper::shape(float sample, const WaveshaperConfig& config) {
  const float driven = sample * db_to_linear(config.drive_db) + config.bias;
  const float wet = apply_curve(driven, config.curve) * db_to_linear(config.output_gain_db);
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
        "use AliasingControl::None, Oversample4x, or a Tanh/Arctan curve");
  }
  // ADAA2 has no implementation here for any curve (Tanh has no closed-form
  // second antiderivative, and Arctan/Asymmetric are not wired to Adaa2
  // either). Reject rather than silently behaving like None.
  if (config.aliasing == sonare::rt::AliasingControl::Adaa2) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "waveshaper ADAA2 anti-aliasing is not supported; use None, Adaa1, or Oversample4x");
  }
}

void Waveshaper::ensure_state(int num_channels) {
  // prepare() preallocates kRealtimePreparedChannels; only grow (control thread)
  // if a caller exceeds it, preserving existing channels' state.
  if (tanh_adaa_.size() < static_cast<size_t>(num_channels)) {
    tanh_adaa_.resize(static_cast<size_t>(num_channels));
    arctan_adaa_.resize(static_cast<size_t>(num_channels));
  }
  if (oversampler_states_.size() < static_cast<size_t>(num_channels)) {
    const size_t old_size = oversampler_states_.size();
    oversampler_states_.resize(static_cast<size_t>(num_channels));
    for (size_t i = old_size; i < oversampler_states_.size(); ++i) {
      oversampler_.prepare_streaming(&oversampler_states_[i], static_cast<size_t>(max_block_size_));
    }
  }
  if (dry_delays_.size() < static_cast<size_t>(num_channels)) {
    const size_t old_size = dry_delays_.size();
    dry_delays_.resize(static_cast<size_t>(num_channels));
    for (size_t i = old_size; i < dry_delays_.size(); ++i) {
      dry_delays_[i].prepare(
          static_cast<size_t>(oversampler_.streaming_round_trip_latency_samples()));
    }
  }
}

int Waveshaper::latency_samples() const noexcept {
  return config_.aliasing == sonare::rt::AliasingControl::Oversample4x
             ? oversampler_.streaming_round_trip_latency_samples()
             : 0;
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
