#include "mastering/saturation/soft_clipper.h"

#include <algorithm>
#include <cmath>

#include "mastering/dynamics/channel_limits.h"
#include "rt/scoped_no_denormals.h"
#include "util/exception.h"

namespace sonare::mastering::saturation {

SoftClipper::SoftClipper(SoftClipperConfig config) : config_(config) { validate_config(config_); }

void SoftClipper::prepare(double sample_rate, int max_block_size) {
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

void SoftClipper::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "SoftClipper");
  if (num_channels < 0 || num_samples < 0)
    throw SonareException(ErrorCode::InvalidParameter, "invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr)
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  ensure_state(num_channels);
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr)
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
  }

  if (config_.aliasing != sonare::rt::AliasingControl::Oversample4x) {
    for (int ch = 0; ch < num_channels; ++ch) {
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] = process_sample(channels[ch][i], ch);
      }
    }
    return;
  }

  // Oversampled path. Reuse the preallocated scratch buffers (sized in
  // prepare()) so this audio-thread path never allocates; reject blocks wider
  // than the prepared size instead of resizing here.
  const float drive = Waveshaper::db_to_linear(config_.drive_db);
  const size_t os_samples =
      static_cast<size_t>(num_samples) * static_cast<size_t>(kOversampleFactor);
  if (os_samples > up_scratch_.size() || static_cast<size_t>(num_samples) > down_scratch_.size()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_samples exceeds prepared SoftClipper oversampling scratch");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    const float* input = channels[ch];
    auto& state = oversampler_states_[static_cast<size_t>(ch)];
    oversampler_.upsample_to_streaming(input, static_cast<size_t>(num_samples), up_scratch_.data(),
                                       up_scratch_.size(), &state);
    for (size_t i = 0; i < os_samples; ++i) {
      up_scratch_[i] = config_.ceiling * std::tanh(up_scratch_[i] * drive / config_.ceiling);
    }
    oversampler_.downsample_to_streaming(up_scratch_.data(), os_samples, down_scratch_.data(),
                                         down_scratch_.size(), &state);
    for (int i = 0; i < num_samples; ++i) {
      const float dry = dry_delays_[static_cast<size_t>(ch)].process(input[i]);
      const float wet = down_scratch_[static_cast<size_t>(i)];
      channels[ch][i] = dry * (1.0f - config_.mix) + wet * config_.mix;
    }
  }
}

void SoftClipper::reset() {
  for (auto& state : tanh_adaa_) state.reset();
  for (auto& state : oversampler_states_) oversampler_.reset_streaming(&state);
  for (auto& delay : dry_delays_) delay.reset();
}

void SoftClipper::set_config(const SoftClipperConfig& config) {
  validate_config(config);
  const bool reset_state = config_.ceiling != config.ceiling || config_.aliasing != config.aliasing;
  config_ = config;
  if (reset_state) reset();
}

bool SoftClipper::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.drive_db = value;
      return true;
    case 1:
      config_.mix = std::clamp(value, 0.0f, 1.0f);
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> SoftClipper::parameter_descriptors() const {
  return {{"driveDb", 0}, {"mix", 1}};
}

void SoftClipper::validate_config(const SoftClipperConfig& config) {
  if (!(config.ceiling > 0.0f) || config.mix < 0.0f || config.mix > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid soft clipper configuration");
  }
  // ADAA2 (second-order antiderivative antialiasing) has no closed-form
  // second antiderivative for tanh, so it is not implemented here. Reject the
  // combination instead of silently behaving like AliasingControl::None.
  if (config.aliasing == sonare::rt::AliasingControl::Adaa2) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "soft clipper ADAA2 anti-aliasing is not supported; use None, Adaa1, or Oversample4x");
  }
}

void SoftClipper::ensure_state(int num_channels) {
  // prepare() preallocates kRealtimePreparedChannels; only grow (control thread)
  // if a caller exceeds it, preserving existing channels' state.
  if (tanh_adaa_.size() < static_cast<size_t>(num_channels)) {
    tanh_adaa_.resize(static_cast<size_t>(num_channels));
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

int SoftClipper::latency_samples() const noexcept {
  return config_.aliasing == sonare::rt::AliasingControl::Oversample4x
             ? oversampler_.streaming_round_trip_latency_samples()
             : 0;
}

float SoftClipper::process_sample(float sample, int channel) {
  const float drive = Waveshaper::db_to_linear(config_.drive_db);
  const float normalized = sample * drive / config_.ceiling;
  const float wet =
      config_.ceiling * (config_.aliasing == sonare::rt::AliasingControl::Adaa1
                             ? tanh_adaa_[static_cast<size_t>(channel)].process(normalized)
                             : std::tanh(normalized));
  return sample * (1.0f - config_.mix) + wet * config_.mix;
}

}  // namespace sonare::mastering::saturation
