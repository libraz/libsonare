#include "mastering/saturation/exciter.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "mastering/dynamics/channel_limits.h"
#include "rt/biquad_design.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::saturation {

namespace {
using sonare::constants::kPiD;

constexpr double kEvenDcCutoffHz = 20.0;

float even_dc_coefficient_for_rate(double sample_rate) {
  return static_cast<float>(1.0 - std::exp(-2.0 * kPiD * kEvenDcCutoffHz / sample_rate));
}

}  // namespace

Exciter::Exciter(ExciterConfig config) : config_(config) { validate_config(config_); }

void Exciter::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0))
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  if (max_block_size < 0)
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  prepared_ = true;
  compute_coeffs();
  even_dc_coefficient_ = even_dc_coefficient_for_rate(sample_rate_);
  even_dc_coefficient_oversampled_ =
      even_dc_coefficient_for_rate(sample_rate_ * static_cast<double>(kHarmonicOversampleFactor));
  // Preallocate per-channel filter state so process() never resizes on the
  // audio thread (matches Tube/AmpSim).
  bandpass_.assign(dynamics::kRealtimePreparedChannels, bandpass_coeffs_);
  allpass_.assign(dynamics::kRealtimePreparedChannels, allpass_coeffs_);
  even_dc_.assign(dynamics::kRealtimePreparedChannels, 0.0f);
  // Preallocate the Oversample4x scratch, per-channel streaming state, and the
  // dry/aligned delays so the audio-thread process() path never allocates.
  band_scratch_.assign(static_cast<size_t>(max_block_size_), 0.0f);
  aligned_scratch_.assign(static_cast<size_t>(max_block_size_), 0.0f);
  oversampled_scratch_.assign(
      static_cast<size_t>(max_block_size_) * static_cast<size_t>(kHarmonicOversampleFactor), 0.0f);
  harmonic_scratch_.assign(static_cast<size_t>(max_block_size_), 0.0f);
  harmonic_oversampler_states_.resize(dynamics::kRealtimePreparedChannels);
  for (auto& state : harmonic_oversampler_states_) {
    harmonic_oversampler_.prepare_streaming(&state, static_cast<size_t>(max_block_size_));
  }
  dry_delays_.resize(dynamics::kRealtimePreparedChannels);
  aligned_delays_.resize(dynamics::kRealtimePreparedChannels);
  for (auto& delay : dry_delays_) {
    delay.prepare(
        static_cast<size_t>(harmonic_oversampler_.streaming_round_trip_latency_samples()));
  }
  for (auto& delay : aligned_delays_) {
    delay.prepare(
        static_cast<size_t>(harmonic_oversampler_.streaming_round_trip_latency_samples()));
  }
  reset();
}

void Exciter::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "Exciter");
  if (num_channels < 0 || num_samples < 0)
    throw SonareException(ErrorCode::InvalidParameter, "invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr)
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  ensure_state(num_channels);
  const float drive = db_to_linear(config_.drive_db);
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr)
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
  }

  if (config_.aliasing != sonare::rt::AliasingControl::Oversample4x) {
    for (int ch = 0; ch < num_channels; ++ch) {
      auto& bandpass = bandpass_[static_cast<size_t>(ch)];
      auto& allpass = allpass_[static_cast<size_t>(ch)];
      float even_dc = even_dc_[static_cast<size_t>(ch)];
      for (int i = 0; i < num_samples; ++i) {
        const float band = bandpass.process(channels[ch][i]);
        const float aligned = allpass.process(band);
        const float even_raw = band * band;
        even_dc += even_dc_coefficient_ * (even_raw - even_dc);
        const float even = even_raw - even_dc;
        const float odd = std::tanh(band * drive);
        const float harmonic =
            (1.0f - config_.even_odd_mix) * even * drive + config_.even_odd_mix * odd;
        channels[ch][i] += aligned * 0.05f * config_.amount + harmonic * config_.amount;
      }
      even_dc_[static_cast<size_t>(ch)] = even_dc;
    }
    return;
  }

  // Oversampled path: the bandpass/allpass filters are linear and stay at the
  // base rate; only the squaring/tanh harmonic generation runs on the
  // oversampled band signal. Reuse the preallocated scratch buffers (sized in
  // prepare()) so this audio-thread path never allocates; reject blocks wider
  // than the prepared size instead of resizing here.
  const size_t os_samples =
      static_cast<size_t>(num_samples) * static_cast<size_t>(kHarmonicOversampleFactor);
  if (os_samples > oversampled_scratch_.size() ||
      static_cast<size_t>(num_samples) > band_scratch_.size() ||
      static_cast<size_t>(num_samples) > harmonic_scratch_.size()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_samples exceeds prepared Exciter oversampling scratch");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    auto& bandpass = bandpass_[static_cast<size_t>(ch)];
    auto& allpass = allpass_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      const float band = bandpass.process(channels[ch][i]);
      band_scratch_[static_cast<size_t>(i)] = band;
      aligned_scratch_[static_cast<size_t>(i)] = allpass.process(band);
    }

    auto& oversampler_state = harmonic_oversampler_states_[static_cast<size_t>(ch)];
    harmonic_oversampler_.upsample_to_streaming(
        band_scratch_.data(), static_cast<size_t>(num_samples), oversampled_scratch_.data(),
        oversampled_scratch_.size(), &oversampler_state);
    float even_dc = even_dc_[static_cast<size_t>(ch)];
    for (size_t i = 0; i < os_samples; ++i) {
      const float band_os = oversampled_scratch_[i];
      const float even_raw = band_os * band_os;
      even_dc += even_dc_coefficient_oversampled_ * (even_raw - even_dc);
      const float even = even_raw - even_dc;
      const float odd = std::tanh(band_os * drive);
      oversampled_scratch_[i] =
          (1.0f - config_.even_odd_mix) * even * drive + config_.even_odd_mix * odd;
    }
    even_dc_[static_cast<size_t>(ch)] = even_dc;
    harmonic_oversampler_.downsample_to_streaming(oversampled_scratch_.data(), os_samples,
                                                  harmonic_scratch_.data(),
                                                  harmonic_scratch_.size(), &oversampler_state);

    for (int i = 0; i < num_samples; ++i) {
      const float dry = dry_delays_[static_cast<size_t>(ch)].process(channels[ch][i]);
      const float aligned = aligned_delays_[static_cast<size_t>(ch)].process(
          aligned_scratch_[static_cast<size_t>(i)]);
      channels[ch][i] = dry + aligned * 0.05f * config_.amount +
                        harmonic_scratch_[static_cast<size_t>(i)] * config_.amount;
    }
  }
}

void Exciter::reset() {
  for (auto& filter : bandpass_) filter.reset();
  for (auto& filter : allpass_) filter.reset();
  std::fill(even_dc_.begin(), even_dc_.end(), 0.0f);
  for (auto& state : harmonic_oversampler_states_) {
    harmonic_oversampler_.reset_streaming(&state);
  }
  for (auto& delay : dry_delays_) delay.reset();
  for (auto& delay : aligned_delays_) delay.reset();
}

void Exciter::set_config(const ExciterConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    update_coeff();
  }
}

void Exciter::validate_config(const ExciterConfig& config) {
  if (!(config.frequency_hz > 0.0f) || config.amount < 0.0f || !(config.q > 0.0f) ||
      config.even_odd_mix < 0.0f || config.even_odd_mix > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid exciter configuration");
  }
  // Only None and Oversample4x are implemented: the harmonic generator mixes
  // a squaring (even) and a tanh (odd) stage that has no ADAA antiderivative
  // wired up here. Reject Adaa1/Adaa2 instead of silently behaving like None.
  if (config.aliasing != sonare::rt::AliasingControl::None &&
      config.aliasing != sonare::rt::AliasingControl::Oversample4x) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "exciter ADAA anti-aliasing is not supported; use None or Oversample4x");
  }
}

void Exciter::compute_coeffs() {
  const float cutoff =
      std::clamp(config_.frequency_hz, 10.0f, static_cast<float>(sample_rate_ * 0.49));
  const float w0 = static_cast<float>(2.0 * kPiD * cutoff / sample_rate_);
  const auto coeffs = rt::rbj_bandpass(w0, config_.q);
  bandpass_coeffs_.c = coeffs;
  allpass_coeffs_.c.b0 = coeffs.a2;
  allpass_coeffs_.c.b1 = coeffs.a1;
  allpass_coeffs_.c.b2 = 1.0f;
  allpass_coeffs_.c.a1 = coeffs.a1;
  allpass_coeffs_.c.a2 = coeffs.a2;
}

void Exciter::update_coeff() {
  // Rebuild the prototype coefficients and reset every live filter (clears the
  // delay state), including the oversampler/delay state the Oversample4x path
  // depends on. Used by prepare()/set_config(), where resetting is intended.
  compute_coeffs();
  for (auto& filter : bandpass_) filter = bandpass_coeffs_;
  for (auto& filter : allpass_) filter = allpass_coeffs_;
  std::fill(even_dc_.begin(), even_dc_.end(), 0.0f);
  for (auto& state : harmonic_oversampler_states_) {
    harmonic_oversampler_.reset_streaming(&state);
  }
  for (auto& delay : dry_delays_) delay.reset();
  for (auto& delay : aligned_delays_) delay.reset();
}

bool Exciter::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.frequency_hz = std::max(value, std::numeric_limits<float>::min());
      if (prepared_) update_coeff_preserving_state();
      return true;
    case 1:
      config_.drive_db = value;
      return true;
    case 2:
      config_.amount = std::max(0.0f, value);
      return true;
    case 3:
      config_.q = std::max(value, std::numeric_limits<float>::min());
      if (prepared_) update_coeff_preserving_state();
      return true;
    case 4:
      config_.even_odd_mix = std::clamp(value, 0.0f, 1.0f);
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> Exciter::parameter_descriptors() const {
  return {{"frequencyHz", 0}, {"driveDb", 1}, {"amount", 2}, {"q", 3}, {"evenOddMix", 4}};
}

void Exciter::update_coeff_preserving_state() {
  // RT-safe automation path: recompute the prototype coefficients, then copy only
  // the coefficient fields into each live filter, leaving the delay state (z1/z2)
  // untouched. No allocation, so this is safe to call from the audio thread.
  compute_coeffs();
  for (auto& filter : bandpass_) {
    filter.c = bandpass_coeffs_.c;
  }
  for (auto& filter : allpass_) {
    filter.c = allpass_coeffs_.c;
  }
}

void Exciter::ensure_state(int num_channels) {
  // prepare() preallocates kRealtimePreparedChannels filters so the audio path
  // never resizes. Only grow (control thread) if a caller exceeds it, and use
  // resize (not assign) so a stereo->mono->stereo reconfigure keeps each
  // existing channel's filter state instead of wiping every channel.
  if (bandpass_.size() < static_cast<size_t>(num_channels)) {
    bandpass_.resize(static_cast<size_t>(num_channels), bandpass_coeffs_);
    allpass_.resize(static_cast<size_t>(num_channels), allpass_coeffs_);
    even_dc_.resize(static_cast<size_t>(num_channels), 0.0f);
  }
  if (harmonic_oversampler_states_.size() < static_cast<size_t>(num_channels)) {
    const size_t old_size = harmonic_oversampler_states_.size();
    harmonic_oversampler_states_.resize(static_cast<size_t>(num_channels));
    for (size_t i = old_size; i < harmonic_oversampler_states_.size(); ++i) {
      harmonic_oversampler_.prepare_streaming(&harmonic_oversampler_states_[i],
                                              static_cast<size_t>(max_block_size_));
    }
  }
  if (dry_delays_.size() < static_cast<size_t>(num_channels)) {
    const size_t old_size = dry_delays_.size();
    dry_delays_.resize(static_cast<size_t>(num_channels));
    aligned_delays_.resize(static_cast<size_t>(num_channels));
    for (size_t i = old_size; i < dry_delays_.size(); ++i) {
      dry_delays_[i].prepare(
          static_cast<size_t>(harmonic_oversampler_.streaming_round_trip_latency_samples()));
      aligned_delays_[i].prepare(
          static_cast<size_t>(harmonic_oversampler_.streaming_round_trip_latency_samples()));
    }
  }
}

int Exciter::latency_samples() const noexcept {
  return config_.aliasing == sonare::rt::AliasingControl::Oversample4x
             ? harmonic_oversampler_.streaming_round_trip_latency_samples()
             : 0;
}

}  // namespace sonare::mastering::saturation
