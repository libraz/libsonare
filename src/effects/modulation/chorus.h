#pragma once

/// @file chorus.h
/// @brief Stereo chorus built from modulated fractional delays, behind an
///        optional one-pole section.
///
/// The section in front of the delay is declared here and shared with the
/// flanger, because a period module's chorus and flanger measure as one and the
/// same front end: the same two shapes, the same corner, the same skirt. Shared
/// is a statement about the design and not about the running filter -- every
/// instance carries its own state, and two instances never see each other's
/// samples. The corner is kept in hertz and the pole is derived in prepare();
/// it is exponential in the corner over the rate, so a stored pole would move
/// the corner every time the rate changed. Off by default, which is the
/// behaviour every existing caller already has.

#include <array>
#include <cmath>
#include <vector>

#include "effects/modulation/lfo.h"
#include "effects/modulation/mod_delay_line.h"
#include "rt/processor_base.h"
#include "util/constants.h"

namespace sonare::effects::modulation {

/// Shape the pre-filter section takes in front of a modulated delay.
enum class PreFilterMode {
  kOff,       ///< The delay reads the input as it arrives.
  kLowPass,   ///< One pole at the configured corner.
  kHighPass,  ///< That pole's complement: the same corner, read from the other side.
};

/// One-pole pre-filter, one instance per channel of one processor.
class PreFilter {
 public:
  /// Derives the pole from @p corner_hz. A corner that is not a usable frequency
  /// at this rate leaves the section out of the path, as kOff does.
  void prepare(PreFilterMode mode, float corner_hz, double sample_rate) noexcept {
    const double rate = sample_rate > 0.0 && std::isfinite(sample_rate) ? sample_rate : 48000.0;
    const bool usable =
        std::isfinite(corner_hz) && corner_hz > 0.0f && static_cast<double>(corner_hz) < rate * 0.5;
    mode_ = usable ? mode : PreFilterMode::kOff;
    // exp(-2*pi*fc/sr): evaluated here so the corner stays a frequency in hertz.
    pole_ = mode_ == PreFilterMode::kOff
                ? 0.0f
                : static_cast<float>(std::exp(-sonare::constants::kTwoPiD * corner_hz / rate));
    state_ = 0.0f;
  }

  void reset() noexcept { state_ = 0.0f; }

  float process(float input) noexcept {
    if (mode_ == PreFilterMode::kOff) return input;
    state_ = input + pole_ * (state_ - input);
    return mode_ == PreFilterMode::kLowPass ? state_ : input - state_;
  }

  float state() const noexcept { return state_; }
  void set_state(float value) noexcept { state_ = value; }

 private:
  PreFilterMode mode_ = PreFilterMode::kOff;
  float pole_ = 0.0f;
  float state_ = 0.0f;
};

struct ChorusConfig {
  float rate_hz = 0.8f;
  float depth_ms = 6.0f;
  float center_delay_ms = 14.0f;
  float dry_wet = 0.5f;
  /// Corner of the pre-filter section, in hertz. Read only when the mode below
  /// engages it; a non-positive corner is out of domain and leaves it off.
  float pre_filter_hz = 0.0f;
  PreFilterMode pre_filter_mode = PreFilterMode::kOff;
  /// Gain of the return from the delay's output, through a one-pole low-pass at
  /// 6 kHz, to its input. Clamped to +-0.95 in process(); zero leaves the path out.
  float feedback = 0.0f;
  /// Phase of the right LFO ahead of the left, in degrees, clamped to [0, 180].
  float phase_deg = 90.0f;
  /// How the delay lines read between samples; see DelayInterpolation.
  DelayInterpolation interpolation = DelayInterpolation::kLinear;
};

class Chorus : public rt::ProcessorBase {
 public:
  explicit Chorus(ChorusConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = rate_hz (clamped to >= 0; updates both LFOs in place)
  //   1 = depth_ms
  //   2 = center_delay_ms
  //   3 = dry_wet
  //   4 = pre_filter_hz (re-derives the pole in place; the filter keeps its state)
  //   5 = feedback (clamped to [-0.95, 0.95] in process(); the sign is carried)
  //   6 = phase_deg (clamped to [0, 180]; re-phases the right LFO in place)
  //   7 = pre_filter_mode (0 off, 1 low-pass, 2 high-pass; the filter keeps its state)
  //   8 = interpolation (0 linear, 1 Lagrange3)
  bool set_parameter_impl(unsigned int param_id, float value) override;
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override;
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  /// Returns the pre-filter sections and the lines they feed to rest once a
  /// non-finite value has reached a section, once per block.
  void discard_non_finite() noexcept;

  ChorusConfig config_{};
  double sample_rate_ = 48000.0;
  std::array<ModDelayLine, 2> delays_;
  std::array<Lfo, 2> lfos_;
  /// [L, R] pre-filter sections. One per channel, never shared with another
  /// instance: what the measurement calls one section is one design.
  std::array<PreFilter, 2> pre_filters_;
  /// [L, R] low-pass sections in the feedback return; their state is the return.
  std::array<PreFilter, 2> feedback_filters_;
};

}  // namespace sonare::effects::modulation
