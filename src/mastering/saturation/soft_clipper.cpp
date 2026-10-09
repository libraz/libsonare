#include "mastering/saturation/soft_clipper.h"

#include <algorithm>
#include <cmath>

#include "mastering/common/prepare_args.h"
#include "mastering/dynamics/channel_limits.h"
#include "mastering/saturation/saturation_process.h"
#include "rt/scoped_no_denormals.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare::mastering::saturation {

SoftClipper::SoftClipper(SoftClipperConfig config) : config_(config) {
  validate_config(config_);
  declare_path_latencies();
}

void SoftClipper::prepare(double sample_rate, int max_block_size) {
  validate_prepare_args(sample_rate, max_block_size);
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
  paths_.ensure_channels(dynamics::kRealtimePreparedChannels);
  reset();
}

void SoftClipper::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "SoftClipper");
  if (!validate_process_buffers(channels, num_channels, num_samples)) return;
  ensure_state(num_channels);
  if (config_.aliasing != sonare::rt::AliasingControl::Oversample4x) {
    detail::process_direct(channels, num_channels, num_samples,
                           [this](float x, int ch) { return process_sample(x, ch); });
    return;
  }

  // Oversampled path runs on the preallocated scratch from prepare().
  const float drive = Waveshaper::db_to_linear(config_.drive_db);
  detail::process_oversampled(
      channels, num_channels, num_samples, kOversampleFactor, oversampler_, oversampler_states_,
      paths_, up_scratch_, down_scratch_, config_.mix, "SoftClipper",
      [&](float x) { return config_.ceiling * std::tanh(x * drive / config_.ceiling); },
      [](float wet) { return wet; });
}

void SoftClipper::reset() {
  for (auto& state : tanh_adaa_) state.reset();
  for (auto& state : oversampler_states_) oversampler_.reset_streaming(&state);
  paths_.reset();
}

void SoftClipper::set_config(const SoftClipperConfig& config) {
  validate_config(config);
  rt::apply_config_diff(config_, config, [this](const rt::ConfigDiff<SoftClipperConfig>& diff) {
    declare_path_latencies();
    // The antiderivative state is relative to the ceiling, and a different
    // aliasing path starts from silence; every other field is read per sample.
    if (diff.changed(&SoftClipperConfig::ceiling, &SoftClipperConfig::aliasing)) reset();
  });
}

bool SoftClipper::set_parameter_impl(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      if (!numeric::finite(Waveshaper::db_to_linear(value))) return false;
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
  if (!numeric::finite(Waveshaper::db_to_linear(config.drive_db))) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "soft clipper drive must produce a finite linear gain");
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
  paths_.ensure_channels(static_cast<size_t>(num_channels));
}

void SoftClipper::declare_path_latencies() {
  paths_.set_path_latencies_q8(
      {0, sonare::rt::aliasing_latency_samples_q8(
              config_.aliasing, oversampler_.streaming_round_trip_latency_samples())});
}

int SoftClipper::tail_samples() const noexcept {
  const sonare::rt::TailBudget tail = sonare::rt::aliasing_tail(
      config_.aliasing, oversampler_.streaming_round_trip_latency_samples());
  return tail.samples();
}

int SoftClipper::latency_samples() const noexcept { return paths_.latency_samples(); }

int SoftClipper::latency_samples_q8() const noexcept { return paths_.latency_samples_q8(); }

float SoftClipper::process_sample(float sample, int channel) {
  const float drive = Waveshaper::db_to_linear(config_.drive_db);
  const float normalized = sample * drive / config_.ceiling;
  const float wet =
      config_.ceiling * (config_.aliasing == sonare::rt::AliasingControl::Adaa1
                             ? tanh_adaa_[static_cast<size_t>(channel)].process(normalized)
                             : std::tanh(normalized));
  const auto ch = static_cast<size_t>(channel);
  return paths_.align(0, ch, sample) * (1.0f - config_.mix) +
         paths_.align(1, ch, wet) * config_.mix;
}

}  // namespace sonare::mastering::saturation
