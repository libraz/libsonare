#include "mastering/saturation/hard_clipper.h"

#include <algorithm>
#include <limits>

#include "mastering/dynamics/channel_limits.h"
#include "rt/scoped_no_denormals.h"
#include "util/exception.h"

namespace sonare::mastering::saturation {

HardClipper::HardClipper(HardClipperConfig config) : config_(config) { validate_config(config_); }

void HardClipper::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0))
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  if (max_block_size < 0)
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  max_block_size_ = max_block_size;
  prepared_ = true;
  // Preallocate per-channel ADAA state so process() never resizes on the audio
  // thread (matches Tube/AmpSim).
  hard_clip_adaa_.clear();
  hard_clip_adaa_.reserve(dynamics::kRealtimePreparedChannels);
  hard_clip_adaa2_.clear();
  hard_clip_adaa2_.reserve(dynamics::kRealtimePreparedChannels);
  for (size_t i = 0; i < dynamics::kRealtimePreparedChannels; ++i) {
    hard_clip_adaa_.emplace_back(sonare::rt::HardClipNonlinearity{config_.ceiling});
    hard_clip_adaa2_.emplace_back(sonare::rt::HardClipNonlinearity{config_.ceiling});
  }
  // Preallocate the Oversample4x scratch and per-channel streaming state up
  // front so the audio-thread process() path never allocates.
  const size_t scratch =
      static_cast<size_t>(max_block_size_) * static_cast<size_t>(kOversampleFactor);
  up_scratch_.assign(scratch, 0.0f);
  down_scratch_.assign(static_cast<size_t>(max_block_size_), 0.0f);
  oversampler_states_.resize(dynamics::kRealtimePreparedChannels);
  for (auto& state : oversampler_states_) {
    oversampler_.prepare_streaming(&state, static_cast<size_t>(max_block_size_));
  }
}

void HardClipper::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "HardClipper");
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

  // Oversampled path: the clamp is fully wet (no dry blend), so nothing else
  // needs a matching delay to stay time-aligned with it. Reuse the
  // preallocated scratch buffers (sized in prepare()) so this audio-thread
  // path never allocates; reject blocks wider than the prepared size instead
  // of resizing here.
  const size_t os_samples =
      static_cast<size_t>(num_samples) * static_cast<size_t>(kOversampleFactor);
  if (os_samples > up_scratch_.size() || static_cast<size_t>(num_samples) > down_scratch_.size()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_samples exceeds prepared HardClipper oversampling scratch");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    auto& state = oversampler_states_[static_cast<size_t>(ch)];
    oversampler_.upsample_to_streaming(channels[ch], static_cast<size_t>(num_samples),
                                       up_scratch_.data(), up_scratch_.size(), &state);
    for (size_t i = 0; i < os_samples; ++i) {
      up_scratch_[i] = std::clamp(up_scratch_[i], -config_.ceiling, config_.ceiling);
    }
    oversampler_.downsample_to_streaming(up_scratch_.data(), os_samples, down_scratch_.data(),
                                         down_scratch_.size(), &state);
    std::copy_n(down_scratch_.data(), static_cast<size_t>(num_samples), channels[ch]);
  }
}

void HardClipper::reset() {
  for (auto& state : hard_clip_adaa_) state.reset();
  for (auto& state : hard_clip_adaa2_) state.reset();
  for (auto& state : oversampler_states_) oversampler_.reset_streaming(&state);
}

void HardClipper::set_config(const HardClipperConfig& config) {
  validate_config(config);
  const bool aliasing_changed = config_.aliasing != config.aliasing;
  const bool reset_state = config_.ceiling != config.ceiling || aliasing_changed;
  config_ = config;
  if (reset_state) rebuild_adaa();
  if (aliasing_changed) {
    for (auto& state : oversampler_states_) oversampler_.reset_streaming(&state);
  }
}

bool HardClipper::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0: {
      const float ceiling = std::max(value, std::numeric_limits<float>::min());
      const bool changed = config_.ceiling != ceiling;
      config_.ceiling = ceiling;
      // None/Oversample4x modes read config_.ceiling per sample; nothing else
      // to do. The ADAA modes cache the threshold inside their nonlinearity
      // objects, so rebuild them (clearing their tiny history) to make the
      // new ceiling take effect.
      if (changed) rebuild_adaa();
      return true;
    }
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> HardClipper::parameter_descriptors() const {
  return {{"ceiling", 0}};
}

void HardClipper::rebuild_adaa() {
  const size_t channels = hard_clip_adaa_.size();
  hard_clip_adaa_.clear();
  hard_clip_adaa_.reserve(channels);
  hard_clip_adaa2_.clear();
  hard_clip_adaa2_.reserve(channels);
  for (size_t i = 0; i < channels; ++i) {
    hard_clip_adaa_.emplace_back(sonare::rt::HardClipNonlinearity{config_.ceiling});
    hard_clip_adaa2_.emplace_back(sonare::rt::HardClipNonlinearity{config_.ceiling});
  }
}

void HardClipper::validate_config(const HardClipperConfig& config) {
  if (!(config.ceiling > 0.0f))
    throw SonareException(ErrorCode::InvalidParameter, "hard clipper ceiling must be positive");
}

void HardClipper::ensure_state(int num_channels) {
  // prepare() preallocates kRealtimePreparedChannels; only grow (control thread)
  // if a caller exceeds it, preserving existing channels' ADAA history.
  for (size_t i = hard_clip_adaa_.size(); i < static_cast<size_t>(num_channels); ++i) {
    hard_clip_adaa_.emplace_back(sonare::rt::HardClipNonlinearity{config_.ceiling});
    hard_clip_adaa2_.emplace_back(sonare::rt::HardClipNonlinearity{config_.ceiling});
  }
  for (size_t i = oversampler_states_.size(); i < static_cast<size_t>(num_channels); ++i) {
    oversampler_states_.emplace_back();
    oversampler_.prepare_streaming(&oversampler_states_.back(),
                                   static_cast<size_t>(max_block_size_));
  }
}

int HardClipper::latency_samples() const noexcept {
  if (config_.aliasing == sonare::rt::AliasingControl::Oversample4x) {
    return oversampler_.streaming_round_trip_latency_samples();
  }
  return config_.aliasing == sonare::rt::AliasingControl::Adaa2 ? 1 : 0;
}

float HardClipper::process_sample(float sample, int channel) {
  if (config_.aliasing == sonare::rt::AliasingControl::Adaa1) {
    return hard_clip_adaa_[static_cast<size_t>(channel)].process(sample);
  }
  if (config_.aliasing == sonare::rt::AliasingControl::Adaa2) {
    return hard_clip_adaa2_[static_cast<size_t>(channel)].process(sample);
  }
  return std::clamp(sample, -config_.ceiling, config_.ceiling);
}

}  // namespace sonare::mastering::saturation
