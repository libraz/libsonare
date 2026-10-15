#include "mastering/saturation/waveshaper.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "mastering/common/prepare_args.h"
#include "mastering/dynamics/channel_limits.h"
#include "mastering/saturation/saturation_process.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare::mastering::saturation {

namespace {

using sonare::constants::kPi;

struct WaveshaperScalars {
  float drive = 0.0f;
  float output_gain = 0.0f;
  float bias = 0.0f;
  float mix = 0.0f;
  WaveshaperCurve curve = WaveshaperCurve::Tanh;
  sonare::rt::AliasingControl aliasing = sonare::rt::AliasingControl::None;
};

WaveshaperScalars make_scalars(const WaveshaperConfig& config) {
  return {Waveshaper::db_to_linear(config.drive_db),
          Waveshaper::db_to_linear(config.output_gain_db),
          config.bias,
          config.mix,
          config.curve,
          config.aliasing};
}

template <typename Curve>
float shape_with_scalars(float sample, const WaveshaperScalars& scalars, Curve&& curve) {
  const float driven = sample * scalars.drive + scalars.bias;
  const float wet = curve(driven) * scalars.output_gain;
  return sample * (1.0f - scalars.mix) + wet * scalars.mix;
}

template <typename Curve>
float shape_sample_with_scalars(
    float sample, int channel, const WaveshaperScalars& scalars,
    std::vector<sonare::rt::Adaa1<sonare::rt::TanhNonlinearity>>& tanh_adaa,
    std::vector<sonare::rt::Adaa1<sonare::rt::ArctanNonlinearity>>& arctan_adaa,
    sonare::rt::ParallelPaths& paths, Curve&& curve) {
  if (scalars.aliasing != sonare::rt::AliasingControl::Adaa1 ||
      scalars.curve == WaveshaperCurve::Asymmetric) {
    return shape_with_scalars(sample, scalars, std::forward<Curve>(curve));
  }

  const float driven = sample * scalars.drive + scalars.bias;
  float wet = driven;
  switch (scalars.curve) {
    case WaveshaperCurve::Tanh:
      wet = tanh_adaa[static_cast<size_t>(channel)].process(driven);
      break;
    case WaveshaperCurve::Arctan:
      wet = (2.0f / kPi) * arctan_adaa[static_cast<size_t>(channel)].process(driven);
      break;
    case WaveshaperCurve::Asymmetric:
      break;
  }
  wet *= scalars.output_gain;
  const auto ch = static_cast<size_t>(channel);
  return paths.align(0, ch, sample) * (1.0f - scalars.mix) + paths.align(1, ch, wet) * scalars.mix;
}

}  // namespace

Waveshaper::Waveshaper(WaveshaperConfig config) : config_(config) {
  validate_config(config_);
  declare_path_latencies();
}

void Waveshaper::prepare(double sample_rate, int max_block_size) {
  validate_prepare_args(sample_rate, max_block_size);
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
  paths_.ensure_channels(dynamics::kRealtimePreparedChannels);
  reset();
}

void Waveshaper::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "Waveshaper");
  if (!validate_process_buffers(channels, num_channels, num_samples)) {
    return;
  }
  ensure_state(num_channels);

  const WaveshaperScalars scalars = make_scalars(config_);
  if (config_.aliasing != sonare::rt::AliasingControl::Oversample4x) {
    detail::process_direct(channels, num_channels, num_samples, [this, &scalars](float x, int ch) {
      return shape_sample_with_scalars(
          x, ch, scalars, tanh_adaa_, arctan_adaa_, paths_,
          [curve = scalars.curve](float driven) { return apply_curve(driven, curve); });
    });
    return;
  }

  // Oversampled path: curve + drive + bias run at the oversampled rate; output gain and the
  // dry/wet mix run at the base rate after downsampling, matching shape()'s split.
  detail::process_oversampled(
      channels, num_channels, num_samples, kOversampleFactor, oversampler_, oversampler_states_,
      paths_, up_scratch_, down_scratch_, scalars.mix, "Waveshaper",
      [&](float x) {
        const float driven = x * scalars.drive + scalars.bias;
        return apply_curve(driven, scalars.curve);
      },
      [output_gain = scalars.output_gain](float wet) { return wet * output_gain; });
}

void Waveshaper::reset() {
  for (auto& state : tanh_adaa_) state.reset();
  for (auto& state : arctan_adaa_) state.reset();
  for (auto& state : oversampler_states_) oversampler_.reset_streaming(&state);
  paths_.reset();
}

void Waveshaper::set_config(const WaveshaperConfig& config) {
  validate_config(config);
  rt::apply_config_diff(config_, config, [this](const rt::ConfigDiff<WaveshaperConfig>& diff) {
    declare_path_latencies();
    // The antiderivative state belongs to one curve and bias, and a different
    // aliasing path starts from silence; other scalars are derived per segment.
    if (diff.changed(&WaveshaperConfig::curve, &WaveshaperConfig::aliasing,
                     &WaveshaperConfig::bias)) {
      reset();
    }
  });
}

bool Waveshaper::set_parameter_impl(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      if (!numeric::finite(db_to_linear(value))) return false;
      config_.drive_db = value;
      return true;
    case 1:
      config_.mix = std::clamp(value, 0.0f, 1.0f);
      return true;
    case 2:
      if (!numeric::finite(db_to_linear(value))) return false;
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
  if (!numeric::finite(db_to_linear(config.drive_db)) ||
      !numeric::finite(db_to_linear(config.output_gain_db))) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "waveshaper drive and output gain must produce finite linear gains");
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
  require_prepared_channels(num_channels,
                            std::min({tanh_adaa_.size(), arctan_adaa_.size(),
                                      oversampler_states_.size(), paths_.num_channels()}),
                            "Waveshaper");
}

void Waveshaper::declare_path_latencies() {
  paths_.set_path_latencies_q8(
      {0, sonare::rt::aliasing_latency_samples_q8(
              config_.aliasing, oversampler_.streaming_round_trip_latency_samples())});
}

int Waveshaper::tail_samples() const noexcept {
  const sonare::rt::TailBudget tail = sonare::rt::aliasing_tail(
      config_.aliasing, oversampler_.streaming_round_trip_latency_samples());
  return tail.samples();
}

int Waveshaper::latency_samples() const noexcept { return paths_.latency_samples(); }

int Waveshaper::latency_samples_q8() const noexcept { return paths_.latency_samples_q8(); }

float Waveshaper::shape_sample(float sample, int channel) {
  // ADAA1 (first-order antiderivative anti-aliasing) is only implemented for the
  // odd Tanh/Arctan curves. The Asymmetric + Adaa1 combination is rejected up
  // front by validate_config(), so it can never reach this point; the
  // Asymmetric branch below remains only as defensive direct evaluation for any
  // future non-Adaa1 aliasing mode that is not separately handled.
  const WaveshaperScalars scalars = make_scalars(config_);
  return shape_sample_with_scalars(
      sample, channel, scalars, tanh_adaa_, arctan_adaa_, paths_,
      [this](float driven) { return apply_curve(driven, config_.curve); });
}

}  // namespace sonare::mastering::saturation
