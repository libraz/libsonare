#include "mastering/spectral/presence_enhancer.h"

#include <algorithm>
#include <cmath>

#include "mastering/dynamics/channel_limits.h"
#include "rt/biquad_design.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare::mastering::spectral {
namespace {

using sonare::constants::kPiD;

PresenceEnhancer::Biquad make_bandpass(double frequency_hz, double sample_rate, double q) {
  const float w0 = static_cast<float>(
      2.0 * kPiD * std::clamp(frequency_hz, 20.0, sample_rate * 0.49) / sample_rate);
  PresenceEnhancer::Biquad b;
  b.c = rt::rbj_bandpass(w0, static_cast<float>(q));
  return b;
}

}  // namespace

PresenceEnhancer::PresenceEnhancer(PresenceEnhancerConfig config) : config_(config) {
  validate_config(config_);
}

void PresenceEnhancer::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0) || max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid prepare arguments");
  }
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  prepared_ = true;
  // Preallocate per-channel filter state so process() never resizes on the
  // audio thread (matches Tube/AmpSim).
  bandpass_.assign(dynamics::kRealtimePreparedChannels,
                   make_bandpass(config_.center_frequency_hz, sample_rate_, config_.q));
  // Preallocate the Oversample4x scratch, per-channel streaming state, and
  // the dry-path delay so the audio-thread process() path never allocates.
  band_scratch_.assign(static_cast<size_t>(max_block_size_), 0.0f);
  oversampled_scratch_.assign(
      static_cast<size_t>(max_block_size_) * static_cast<size_t>(kHarmonicOversampleFactor), 0.0f);
  harmonic_scratch_.assign(static_cast<size_t>(max_block_size_), 0.0f);
  harmonic_oversampler_states_.resize(dynamics::kRealtimePreparedChannels);
  for (auto& state : harmonic_oversampler_states_) {
    harmonic_oversampler_.prepare_streaming(&state, static_cast<size_t>(max_block_size_));
  }
  dry_delays_.resize(dynamics::kRealtimePreparedChannels);
  for (auto& delay : dry_delays_) {
    delay.prepare(
        static_cast<size_t>(harmonic_oversampler_.streaming_round_trip_latency_samples()));
  }
  reset();
}

void PresenceEnhancer::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "PresenceEnhancer");
  if (num_channels < 0 || num_samples < 0)
    throw SonareException(ErrorCode::InvalidParameter, "invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr)
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  ensure_state(num_channels);

  if (config_.aliasing != sonare::rt::AliasingControl::Oversample4x) {
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] == nullptr)
        throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
      for (int i = 0; i < num_samples; ++i) {
        const float presence = bandpass_[static_cast<size_t>(ch)].process(channels[ch][i]);
        const float harmonic = std::tanh(presence * config_.drive);
        channels[ch][i] += harmonic * config_.amount;
      }
    }
    return;
  }

  // Oversampled path: the bandpass filter is linear and stays at the base
  // rate; only the tanh harmonic generation runs on the oversampled band
  // signal. Reuse the preallocated scratch buffers (sized in prepare()) so
  // this audio-thread path never allocates; reject blocks wider than the
  // prepared size instead of resizing here.
  const size_t os_samples =
      static_cast<size_t>(num_samples) * static_cast<size_t>(kHarmonicOversampleFactor);
  if (os_samples > oversampled_scratch_.size() ||
      static_cast<size_t>(num_samples) > band_scratch_.size() ||
      static_cast<size_t>(num_samples) > harmonic_scratch_.size()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_samples exceeds prepared PresenceEnhancer oversampling scratch");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr)
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
    for (int i = 0; i < num_samples; ++i) {
      band_scratch_[static_cast<size_t>(i)] =
          bandpass_[static_cast<size_t>(ch)].process(channels[ch][i]);
    }

    auto& oversampler_state = harmonic_oversampler_states_[static_cast<size_t>(ch)];
    harmonic_oversampler_.upsample_to_streaming(
        band_scratch_.data(), static_cast<size_t>(num_samples), oversampled_scratch_.data(),
        oversampled_scratch_.size(), &oversampler_state);
    for (size_t i = 0; i < os_samples; ++i) {
      oversampled_scratch_[i] = std::tanh(oversampled_scratch_[i] * config_.drive);
    }
    harmonic_oversampler_.downsample_to_streaming(oversampled_scratch_.data(), os_samples,
                                                  harmonic_scratch_.data(),
                                                  harmonic_scratch_.size(), &oversampler_state);

    for (int i = 0; i < num_samples; ++i) {
      const float dry = dry_delays_[static_cast<size_t>(ch)].process(channels[ch][i]);
      channels[ch][i] = dry + harmonic_scratch_[static_cast<size_t>(i)] * config_.amount;
    }
  }
}

void PresenceEnhancer::set_config(const PresenceEnhancerConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    // Rebuild the preallocated per-channel filters in place so the next
    // process() never resizes on the audio thread; keep the channel count.
    // Also reset the Oversample4x streaming state and dry-path delay so a
    // config change (including toggling aliasing) never leaks stale history.
    const Biquad fresh = make_bandpass(config_.center_frequency_hz, sample_rate_, config_.q);
    for (auto& filter : bandpass_) filter = fresh;
    for (auto& state : harmonic_oversampler_states_) {
      harmonic_oversampler_.reset_streaming(&state);
    }
    for (auto& delay : dry_delays_) delay.reset();
  } else {
    bandpass_.clear();
  }
}

void PresenceEnhancer::reset() {
  for (auto& filter : bandpass_) filter.reset();
  for (auto& state : harmonic_oversampler_states_) {
    harmonic_oversampler_.reset_streaming(&state);
  }
  for (auto& delay : dry_delays_) delay.reset();
}

bool PresenceEnhancer::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.amount = std::clamp(value, 0.0f, 1.0f);
      return true;
    case 1:
      config_.drive = std::max(value, 1.0e-6f);
      return true;
    case 2:
    case 3: {
      if (param_id == 2) {
        config_.center_frequency_hz = std::max(value, 1.0e-3f);
      } else {
        config_.q = std::max(value, 1.0e-6f);
      }
      // Recompute the cached bandpass coefficients in place, preserving each
      // channel's filter state (z1/z2).
      for (auto& filter : bandpass_) {
        const Biquad updated = make_bandpass(config_.center_frequency_hz, sample_rate_, config_.q);
        filter.c = updated.c;
      }
      return true;
    }
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> PresenceEnhancer::parameter_descriptors() const {
  return {{"amount", 0}, {"drive", 1}, {"centerFrequencyHz", 2}, {"q", 3}};
}

void PresenceEnhancer::validate_config(const PresenceEnhancerConfig& config) {
  if (!(config.amount >= 0.0f && config.amount <= 1.0f) || !(config.drive > 0.0f) ||
      !(config.center_frequency_hz > 0.0f) || !(config.q > 0.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid presence enhancer configuration");
  }
  // Only None and Oversample4x are implemented: the harmonic generator is a
  // plain tanh with no ADAA antiderivative wired up here. Reject Adaa1/Adaa2
  // instead of silently behaving like None.
  if (config.aliasing != sonare::rt::AliasingControl::None &&
      config.aliasing != sonare::rt::AliasingControl::Oversample4x) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "presence enhancer ADAA anti-aliasing is not supported; use None or Oversample4x");
  }
}

void PresenceEnhancer::ensure_state(int num_channels) {
  // prepare() preallocates kRealtimePreparedChannels; only grow (control thread)
  // if a caller exceeds it, preserving existing channels' filter state.
  const auto target_size = static_cast<size_t>(num_channels);
  if (bandpass_.size() < target_size) {
    const size_t old_size = bandpass_.size();
    bandpass_.resize(target_size);
    for (size_t i = old_size; i < target_size; ++i) {
      bandpass_[i] = make_bandpass(config_.center_frequency_hz, sample_rate_, config_.q);
    }
  }
  if (harmonic_oversampler_states_.size() < target_size) {
    const size_t old_size = harmonic_oversampler_states_.size();
    harmonic_oversampler_states_.resize(target_size);
    for (size_t i = old_size; i < target_size; ++i) {
      harmonic_oversampler_.prepare_streaming(&harmonic_oversampler_states_[i],
                                              static_cast<size_t>(max_block_size_));
    }
  }
  if (dry_delays_.size() < target_size) {
    const size_t old_size = dry_delays_.size();
    dry_delays_.resize(target_size);
    for (size_t i = old_size; i < target_size; ++i) {
      dry_delays_[i].prepare(
          static_cast<size_t>(harmonic_oversampler_.streaming_round_trip_latency_samples()));
    }
  }
}

int PresenceEnhancer::latency_samples() const noexcept {
  return config_.aliasing == sonare::rt::AliasingControl::Oversample4x
             ? harmonic_oversampler_.streaming_round_trip_latency_samples()
             : 0;
}

}  // namespace sonare::mastering::spectral
