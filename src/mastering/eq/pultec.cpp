#include "mastering/eq/pultec.h"

#include <algorithm>
#include <cmath>

#include "mastering/dynamics/channel_limits.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::eq {

using sonare::mastering::dynamics::kRealtimePreparedChannels;

using sonare::constants::kTwoPiD;

namespace {

// One-pole low-pass smoothing coefficient: alpha = 1 - exp(-2*pi*f/fs).
// Computed in double precision (filter design) and clamped to a sane
// sub-Nyquist corner before the formula so a high/aliased frequency cannot
// produce alpha > 1 (which would make the one-pole unstable). The previous
// implementation used the raw digital radian frequency (2*pi*f/fs) directly as
// the coefficient, which diverges badly from the intended corner at high
// frequencies.
float one_pole_alpha(float frequency_hz, double sample_rate) {
  const double max_frequency = sample_rate * 0.49;
  const double clamped =
      std::clamp(static_cast<double>(frequency_hz), 1.0, std::max(max_frequency, 1.0));
  return static_cast<float>(1.0 - std::exp(-kTwoPiD * clamped / sample_rate));
}

}  // namespace

void PultecEq::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  sample_rate_ = sample_rate;
  eq_.prepare(sample_rate, max_block_size);
  // Preallocate component state so the audio-thread process() never resizes
  // (mirrors the kRealtimePreparedChannels pattern used by ParametricEq/DynamicEq).
  component_state_.assign(kRealtimePreparedChannels, {});
  update_component_coefficients();
  rebuild();
}

void PultecEq::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  eq_.process(channels, num_channels, num_samples);
  if (component_model_ == PultecComponentModel::CurveOnly && output_drive_ <= 0.0f) {
    return;
  }
  if (num_channels <= 0 || num_samples <= 0 || channels == nullptr) {
    return;
  }
  prepare_component_state(num_channels);
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) continue;
    for (int i = 0; i < num_samples; ++i) {
      channels[ch][i] = process_component_sample(channels[ch][i], ch);
    }
  }
}

void PultecEq::reset() {
  eq_.reset();
  for (auto& state : component_state_) state = {};
}

void PultecEq::set_low_frequency(float frequency_hz) {
  low_frequency_hz_ = validate_frequency(frequency_hz);
  update_component_coefficients();
  rebuild();
}

void PultecEq::set_low_boost(float amount) {
  low_boost_ = clamp_amount(amount);
  rebuild();
}

void PultecEq::set_low_attenuation(float amount) {
  low_attenuation_ = clamp_amount(amount);
  rebuild();
}

void PultecEq::set_high_boost(float frequency_hz, float amount, float bandwidth) {
  high_boost_frequency_hz_ = validate_frequency(frequency_hz);
  high_boost_ = clamp_amount(amount);
  high_bandwidth_ = std::clamp(bandwidth, 0.0f, 1.0f);
  rebuild();
}

void PultecEq::set_high_attenuation(float frequency_hz, float amount) {
  high_attenuation_frequency_hz_ = validate_frequency(frequency_hz);
  high_attenuation_ = clamp_amount(amount);
  update_component_coefficients();
  rebuild();
}

void PultecEq::set_component_model(PultecComponentModel model) {
  component_model_ = model;
  rebuild();
}

void PultecEq::set_output_drive(float drive) {
  if (drive < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "Pultec output drive must be non-negative");
  }
  output_drive_ = std::min(drive, 10.0f);
}

void PultecEq::clear() {
  low_boost_ = 0.0f;
  low_attenuation_ = 0.0f;
  high_boost_ = 0.0f;
  high_attenuation_ = 0.0f;
  output_drive_ = 0.0f;
  rebuild();
}

void PultecEq::rebuild() {
  // Each of the four shelving/peak bands is set explicitly below, including the
  // disabled case (ParametricEq skips disabled bands during processing), so we
  // do not clear() first. This lets rebuild() run on the audio thread for
  // parameter automation: enabled bands get fresh coefficients while their
  // filter state (z1/z2) is preserved.
  const bool low_boost_enabled = low_boost_ > 0.0f;
  const bool low_atten_enabled = low_attenuation_ > 0.0f;
  const bool high_boost_enabled = high_boost_ > 0.0f;
  const bool high_atten_enabled = high_attenuation_ > 0.0f;

  const float low_interaction = component_model_ == PultecComponentModel::Eqp1aWdf
                                    ? std::min(low_boost_, low_attenuation_) * 0.12f
                                    : 0.0f;
  eq_.set_band(0, {EqBandType::LowShelf, low_frequency_hz_, low_boost_ * 1.7f - low_interaction,
                   0.65f, low_boost_enabled});
  eq_.set_band(1, {EqBandType::Peak, low_frequency_hz_ * 1.45f,
                   -low_attenuation_ * 1.2f - low_interaction, 0.75f, low_atten_enabled});

  const float high_q = 0.6f + high_bandwidth_ * 3.0f;
  eq_.set_band(2, {EqBandType::Peak, high_boost_frequency_hz_, high_boost_ * 1.5f, high_q,
                   high_boost_enabled});
  eq_.set_band(3, {EqBandType::HighShelf, high_attenuation_frequency_hz_, -high_attenuation_ * 1.2f,
                   0.7f, high_atten_enabled});
}

bool PultecEq::set_parameter(unsigned int param_id, float value) {
  // Keep frequencies inside (0 Hz, Nyquist) so coefficient design never throws
  // on the audio thread. Band 1 derives its center as low_frequency_hz * 1.45,
  // so the low frequency is clamped against Nyquist / 1.45.
  const float nyquist = static_cast<float>(sample_rate_ * 0.5);
  const float low_freq_max = std::max(nyquist / 1.45f - 1.0e-3f, 1.0e-3f);
  const float high_freq_max = std::max(nyquist - 1.0e-3f, 1.0e-3f);
  switch (param_id) {
    case 0:
      low_frequency_hz_ = std::clamp(value, 1.0e-3f, low_freq_max);
      update_component_coefficients();
      break;
    case 1:
      low_boost_ = clamp_amount(value);
      break;
    case 2:
      low_attenuation_ = clamp_amount(value);
      break;
    case 3:
      high_boost_frequency_hz_ = std::clamp(value, 1.0e-3f, high_freq_max);
      break;
    case 4:
      high_boost_ = clamp_amount(value);
      break;
    case 5:
      high_bandwidth_ = std::clamp(value, 0.0f, 1.0f);
      break;
    case 6:
      high_attenuation_frequency_hz_ = std::clamp(value, 1.0e-3f, high_freq_max);
      update_component_coefficients();
      break;
    case 7:
      high_attenuation_ = clamp_amount(value);
      break;
    case 8:
      // Output drive affects only the waveshaper; no biquad rebuild needed.
      output_drive_ = std::clamp(value, 0.0f, 10.0f);
      return true;
    default:
      return false;
  }
  rebuild();
  return true;
}

std::vector<rt::ParamDescriptor> PultecEq::parameter_descriptors() const {
  return {{"lowFrequencyHz", 0},
          {"lowBoost", 1},
          {"lowAttenuation", 2},
          {"highBoostFrequencyHz", 3},
          {"highBoost", 4},
          {"highBandwidth", 5},
          {"highAttenuationFrequencyHz", 6},
          {"highAttenuation", 7},
          {"outputDrive", 8}};
}

float PultecEq::clamp_amount(float amount) { return std::clamp(amount, 0.0f, 10.0f); }

float PultecEq::validate_frequency(float frequency_hz) {
  if (!(frequency_hz > 0.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "frequency_hz must be positive");
  }
  return frequency_hz;
}

void PultecEq::update_component_coefficients() {
  low_component_alpha_ = one_pole_alpha(low_frequency_hz_, sample_rate_);
  high_component_alpha_ = one_pole_alpha(high_attenuation_frequency_hz_, sample_rate_);
}

void PultecEq::prepare_component_state(int num_channels) {
  if (num_channels <= 0) return;
  // Steady state: component_state_ is preallocated to kRealtimePreparedChannels
  // in prepare(), so no audio-thread allocation occurs. Only grow (off the
  // common path) if a caller exceeds that — and never shrink.
  if (static_cast<size_t>(num_channels) > component_state_.size()) {
    component_state_.resize(static_cast<size_t>(num_channels));
  }
}

float PultecEq::process_component_sample(float input, int channel) {
  float output = input;
  if (component_model_ == PultecComponentModel::Eqp1aWdf) {
    auto& state = component_state_[static_cast<size_t>(channel)];
    state.low_charge += low_component_alpha_ * (output - state.low_charge);
    state.high_charge += high_component_alpha_ * (output - state.high_charge);
    const float low_reactive = state.low_charge;
    const float high_reactive = output - state.high_charge;

    const float knob_sum = low_boost_ + low_attenuation_ + high_boost_ + high_attenuation_;
    const float insertion_loss_db = -1.0f - 0.06f * knob_sum;
    const float makeup_db = 0.8f + 0.04f * knob_sum;
    output *= db_to_linear(insertion_loss_db + makeup_db);
    output += 0.004f * low_boost_ * low_reactive;
    output -= 0.004f * low_attenuation_ * low_reactive;
    output += 0.003f * high_boost_ * high_reactive;
    output -= 0.003f * high_attenuation_ * high_reactive;
  }
  if (output_drive_ > 0.0f || component_model_ == PultecComponentModel::Eqp1aWdf) {
    const float drive = 1.0f + 0.15f * output_drive_;
    output = std::tanh(output * drive) / std::tanh(drive);
  }
  return output;
}

}  // namespace sonare::mastering::eq
