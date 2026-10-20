#pragma once

/// @file loop_phase_servo.h
/// @brief Loop phase servo for a self-oscillating jet pipe: once a period, the
///        f0 lead the whole injection holds over the returning bore pressure,
///        read as the angle of a smoothed complex cross-spectrum.
///
/// A jet pipe's bore line is cut to the period less the loop's known delays, but
/// the nonlinear source (the clipped jet, the rectifier image, a jet ratio off the
/// half period) leads or lags by an amount no closed form gives, and the pitch
/// follows it. The servo measures that lead and the caller shortens the bore by
/// it. Shared by the flute's cubic source and every pipe-organ rank; what the
/// bore is re-cut to from the lead stays at the call site.

#include <algorithm>
#include <cmath>

#include "util/constants.h"

namespace sonare::midi::synth {

/// Time constant (ms) of the servo's cross-spectrum average: tens of periods, so one period's
/// harmonic leakage averages out while the onset still lands in tune. An estimate.
constexpr float kLoopPhaseServoMs = 40.0f;

struct LoopPhaseServo {
  /// The measured lead of the injection over the returning pressure at f0, in samples.
  float lead = 0.0f;
  float smooth = 0.0f;
  int len = 0;
  int count = 0;
  float cos_ = 1.0f;
  float sin_ = 0.0f;
  float rot_cos = 1.0f;
  float rot_sin = 0.0f;
  float ret_re = 0.0f;
  float ret_im = 0.0f;
  float inj_re = 0.0f;
  float inj_im = 0.0f;
  float x_re = 0.0f;
  float x_im = 0.0f;

  /// Arms the servo for a note whose loop period is @p period_samples and whose f0 phasor
  /// advances @p omega radians a sample at @p sample_rate.
  void start(float period_samples, float omega, float sample_rate) noexcept {
    lead = 0.0f;
    len = std::max(1, static_cast<int>(std::lround(period_samples)));
    count = 0;
    smooth = 1.0f - std::exp(-period_samples * 1000.0f / (kLoopPhaseServoMs * sample_rate));
    cos_ = 1.0f;
    sin_ = 0.0f;
    rot_cos = std::cos(omega);
    rot_sin = std::sin(omega);
    ret_re = ret_im = inj_re = inj_im = 0.0f;
    x_re = x_im = 0.0f;
  }

  /// Adds one sample of @p returning pressure and @p injected drive to the period's coherent
  /// sums. At the period's end re-estimates `lead` at @p omega (the pitch the loop is running at),
  /// arms the next period of @p period_samples at that pitch and returns true.
  bool step(float returning, float injected, float omega, float period_samples) noexcept {
    ret_re += returning * cos_;
    ret_im -= returning * sin_;
    inj_re += injected * cos_;
    inj_im -= injected * sin_;
    const float c = cos_ * rot_cos - sin_ * rot_sin;
    sin_ = sin_ * rot_cos + cos_ * rot_sin;
    cos_ = c;
    if (++count < len) return false;
    // Averaging the complex cross-spectrum weights each period by its power; its angle is the lead.
    const float cx_re = inj_re * ret_re + inj_im * ret_im;
    const float cx_im = inj_im * ret_re - inj_re * ret_im;
    x_re += smooth * (cx_re - x_re);
    x_im += smooth * (cx_im - x_im);
    lead = std::atan2(x_im, x_re) / omega;
    len = std::max(1, static_cast<int>(std::lround(period_samples)));
    count = 0;
    rot_cos = std::cos(omega);
    rot_sin = std::sin(omega);
    cos_ = 1.0f;
    sin_ = 0.0f;
    ret_re = ret_im = inj_re = inj_im = 0.0f;
    return true;
  }
};

}  // namespace sonare::midi::synth
