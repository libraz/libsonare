#include "mastering/saturation/tube.h"

#include <algorithm>
#include <cmath>

#include "mastering/dynamics/channel_limits.h"
#include "rt/biquad_design.h"
#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::saturation {

namespace {

// Operating-point and gain-staging constants for the triode model. These are
// circuit/voltage quantities specific to this processor (not universal math
// constants), so they live here rather than in util/constants.h.
// Fixed plate (anode) supply voltage, in volts, used as the operating point for
// the Dempwolf 12AX7 current law.
constexpr float kPlateVoltageV = 250.0f;
// Maps the normalized input sample (after drive) into the grid-voltage swing
// the triode model expects.
constexpr float kGridDriveScaleV = 1.2f;
// Shaping factor applied to the plate-current delta before the tanh soft clip;
// sets how quickly the harmonic stage saturates.
constexpr float kPlateClipShaping = 2.5f;

struct Dempwolf12Ax7 {
  // Dempwolf & Zoelzer, "A Physically-motivated Triode Model for Circuit
  // Simulations", DAFx-11, equations (10)-(12), Table 1 first fitted 12AX7
  // system. Voltages are Vg/Va relative to cathode; fitted currents are used
  // in the same milliampere scale as the paper's figures.
  static constexpr float G = 2.242e-3f;
  static constexpr float mu = 103.2f;
  static constexpr float gamma = 1.26f;
  static constexpr float C = 3.40f;
  static constexpr float Gg = 6.177e-4f;
  static constexpr float xi = 1.314f;
  static constexpr float Cg = 9.901f;
  static constexpr float Ig0 = 8.025e-8f;
};

float smooth_positive(float c, float x) {
  const float z = c * x;
  if (z > 30.0f) return x;
  if (z < -30.0f) return std::exp(z) / c;
  return std::log1p(std::exp(z)) / c;
}

float cathode_current_ma(float vg, float va) {
  const float effective = va / Dempwolf12Ax7::mu + vg;
  return Dempwolf12Ax7::G *
         std::pow(smooth_positive(Dempwolf12Ax7::C, effective), Dempwolf12Ax7::gamma);
}

float grid_current_ma(float vg) {
  return Dempwolf12Ax7::Gg * std::pow(smooth_positive(Dempwolf12Ax7::Cg, vg), Dempwolf12Ax7::xi) +
         Dempwolf12Ax7::Ig0;
}

float plate_current_ma(float vg, float va) {
  return cathode_current_ma(vg, va) - grid_current_ma(vg);
}

}  // namespace

Tube::Tube(TubeConfig config) : tube_config_(config) {
  validate_config(tube_config_);
  if (tube_config_.oversample_factor != 1) {
    oversampler_.set_factor(tube_config_.oversample_factor);
  }
}

void Tube::set_config(const TubeConfig& config) {
  validate_config(config);
  tube_config_ = config;
  if (tube_config_.oversample_factor != 1) {
    oversampler_.set_factor(tube_config_.oversample_factor);
  }
  if (prepared_) {
    // oversample_factor may have changed; resize the scratch on this
    // control-thread path (allocation here is acceptable, never on the audio
    // thread). Matches the sizing done in prepare().
    allocate_scratch();
  }
}

void Tube::allocate_scratch() {
  // Preallocate the oversampling scratch up front so the audio-thread process()
  // path never allocates. Sized to the worst case (max block * factor); the
  // factor==1 path leaves these empty. Blocks wider than max_block_size_ throw
  // instead of resizing on the audio thread.
  const size_t base = static_cast<size_t>(std::max(0, max_block_size_));
  const size_t scratch = base * static_cast<size_t>(std::max(1, tube_config_.oversample_factor));
  up_scratch_.assign(scratch, 0.0f);
  down_scratch_.assign(base, 0.0f);
}

void Tube::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0))
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  if (max_block_size < 0)
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  allocate_scratch();
  // Preallocate per-channel Miller-filter state so process() never resizes on
  // the audio thread for any channel count up to the realtime limit.
  miller_state_.assign(dynamics::kRealtimePreparedChannels, 0.0f);
  prepared_ = true;
  reset();
}

void Tube::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "Tube");
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

  if (tube_config_.oversample_factor == 1) {
    for (int ch = 0; ch < num_channels; ++ch) {
      for (int i = 0; i < num_samples; ++i) {
        const float wet = apply_miller_filter(ch, process_model(channels[ch][i], tube_config_));
        channels[ch][i] = channels[ch][i] * (1.0f - tube_config_.mix) + wet * tube_config_.mix;
      }
    }
    return;
  }

  // Oversampled path. Reuse the preallocated scratch buffers (sized in
  // prepare()) so this audio-thread path never allocates; reject blocks wider
  // than the prepared size instead of resizing here.
  const size_t os_samples =
      static_cast<size_t>(num_samples) * static_cast<size_t>(tube_config_.oversample_factor);
  if (os_samples > up_scratch_.size() || static_cast<size_t>(num_samples) > down_scratch_.size()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_samples exceeds prepared Tube oversampling scratch");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    oversampler_.upsample_to(channels[ch], static_cast<size_t>(num_samples), up_scratch_.data(),
                             up_scratch_.size());
    for (size_t i = 0; i < os_samples; ++i) {
      up_scratch_[i] = process_model(up_scratch_[i], tube_config_);
    }
    oversampler_.downsample_to(up_scratch_.data(), os_samples, down_scratch_.data(),
                               down_scratch_.size());
    for (int i = 0; i < num_samples; ++i) {
      const float wet = apply_miller_filter(ch, down_scratch_[static_cast<size_t>(i)]);
      channels[ch][i] = channels[ch][i] * (1.0f - tube_config_.mix) + wet * tube_config_.mix;
    }
  }
}

void Tube::reset() {
  std::fill(up_scratch_.begin(), up_scratch_.end(), 0.0f);
  std::fill(down_scratch_.begin(), down_scratch_.end(), 0.0f);
  std::fill(miller_state_.begin(), miller_state_.end(), 0.0f);
}

float Tube::process_model(float sample, const TubeConfig& config) {
  const float drive = db_to_linear(config.drive_db);
  const float grid_bias_v = config.bias_v + config.bias * 2.0f;
  const float grid_signal_v = sample * drive * kGridDriveScaleV;
  const float plate_v = kPlateVoltageV;
  const float idle = plate_current_ma(grid_bias_v, plate_v);
  const float current_delta = plate_current_ma(grid_bias_v + grid_signal_v, plate_v) - idle;
  const float clipped = std::tanh(current_delta * kPlateClipShaping);
  return config.harmonic_drive * clipped + (1.0f - config.harmonic_drive) * current_delta;
}

bool Tube::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      tube_config_.drive_db = value;
      return true;
    case 1:
      tube_config_.bias = value;
      return true;
    case 2:
      tube_config_.mix = std::clamp(value, 0.0f, 1.0f);
      return true;
    case 3:
      // validate_config requires bias_v to be finite; reject non-finite values.
      if (!std::isfinite(value)) return false;
      tube_config_.bias_v = value;
      return true;
    case 4:
      tube_config_.harmonic_drive = std::clamp(value, 0.0f, 1.0f);
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> Tube::parameter_descriptors() const {
  return {{"driveDb", 0}, {"bias", 1}, {"mix", 2}, {"biasV", 3}, {"harmonicDrive", 4}};
}

void Tube::validate_config(const TubeConfig& config) {
  if (config.mix < 0.0f || config.mix > 1.0f || !std::isfinite(config.bias_v) ||
      config.harmonic_drive < 0.0f || config.harmonic_drive > 1.0f ||
      config.oversample_factor < 1 ||
      (config.oversample_factor != 1 && config.oversample_factor != 2 &&
       config.oversample_factor != 4 && config.oversample_factor != 8)) {
    throw SonareException(ErrorCode::InvalidParameter, "tube mix must be in [0, 1]");
  }
}

void Tube::ensure_state(int num_channels) {
  // prepare() preallocates kRealtimePreparedChannels states so the common audio
  // path never resizes. Only grow (control thread) if a caller exceeds it.
  if (miller_state_.size() < static_cast<size_t>(num_channels)) {
    miller_state_.assign(static_cast<size_t>(num_channels), 0.0f);
  }
}

float Tube::apply_miller_filter(int channel, float sample) {
  // Dempwolf/Zoelzer DAFx-11 section 5.6 lists 12AX7 parasitic capacitances
  // (Cak=0.9 pF, Cgk=2.3 pF, Cag=2.4 pF). We approximate their Miller effect
  // as a drive-dependent one-pole low-pass in the audio output path.
  const float drive = db_to_linear(tube_config_.drive_db);
  const float cutoff = std::clamp(22000.0f / (1.0f + 0.08f * drive), 2500.0f,
                                  static_cast<float>(sample_rate_ * 0.45));
  const float coeff = rt::one_pole_lowpass_alpha_matched(cutoff, sample_rate_);
  auto& state = miller_state_[static_cast<size_t>(channel)];
  state += coeff * (sample - state);
  return state;
}

}  // namespace sonare::mastering::saturation
