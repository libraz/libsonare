#pragma once

/// @file fade_curve.h
/// @brief Fade-curve law and the clip-fade-envelope math shared by every clip
///        player (audio and MIDI) so the two agree bit-for-bit.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "util/constants.h"

namespace sonare {

/// Fade-curve law applied to a clip fade-in / fade-out region.
enum class FadeCurve {
  /// Linear-amplitude ramp (default; preserves existing golden output). Each
  /// gain is 0.5 (-6 dB) at the midpoint of a symmetric crossfade: coherent
  /// material sums to unity there, while uncorrelated equal-power material dips
  /// ~-3 dB in power.
  Linear,
  /// Equal-power ramp using a sine/cosine law: the squared gains sum to 1, so
  /// each gain is ~0.707 (-3 dB) at the midpoint. Uncorrelated equal-power
  /// material holds its power; coherent material rises ~+3 dB there.
  EqualPower,
  /// Slow start, fast finish (x^2).
  Exponential,
  /// Fast start, slow finish (sqrt(x)).
  Logarithmic,
};

/// Evaluates one fade curve at a linear-progress fraction in [0, 1] (clamped).
inline float fade_curve_gain(FadeCurve curve, float fraction) noexcept {
  fraction = std::clamp(fraction, 0.0f, 1.0f);
  switch (curve) {
    case FadeCurve::EqualPower:
      return std::sin(constants::kHalfPi * fraction);
    case FadeCurve::Exponential:
      return fraction * fraction;
    case FadeCurve::Logarithmic:
      return std::sqrt(fraction);
    case FadeCurve::Linear:
    default:
      return fraction;
  }
}

/// Fade-in/fade-out gain at `position` samples into a clip of `length` samples.
///
/// `length <= 0` is the open-ended case (a clip with no known end yet, e.g. a
/// MIDI clip still being recorded): fade-out is impossible without an end, so
/// it is skipped entirely, and fade-in runs its own requested duration rather
/// than being clamped against a length that does not exist. For a bounded
/// clip (`length > 0`) both fades are clamped to `length`, matching the
/// existing ClipPlayer::fade_gain contract exactly.
inline float clip_fade_gain(int64_t position, int64_t length, int64_t fade_in, int64_t fade_out,
                            FadeCurve in_curve, FadeCurve out_curve) noexcept {
  float gain = 1.0f;
  const bool open_ended = length <= 0;
  const int64_t fade_in_samples =
      open_ended ? std::max<int64_t>(0, fade_in) : std::min(std::max<int64_t>(0, fade_in), length);
  if (fade_in_samples > 0 && position < fade_in_samples) {
    const float fraction = static_cast<float>(position) / static_cast<float>(fade_in_samples);
    gain *= fade_curve_gain(in_curve, fraction);
  }
  if (!open_ended) {
    const int64_t fade_out_samples = std::min(std::max<int64_t>(0, fade_out), length);
    if (fade_out_samples > 0) {
      const int64_t fade_start = length - fade_out_samples;
      if (position >= fade_start) {
        const float fraction = static_cast<float>(std::max<int64_t>(0, length - position)) /
                               static_cast<float>(fade_out_samples);
        gain *= fade_curve_gain(out_curve, fraction);
      }
    }
  }
  return std::clamp(gain, 0.0f, 1.0f);
}

}  // namespace sonare
