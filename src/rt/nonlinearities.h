#pragma once

/// @file nonlinearities.h
/// @brief Nonlinear transfer functions and antiderivatives for ADAA.

#include <algorithm>
#include <cmath>

namespace sonare::rt {

struct TanhNonlinearity {
  float apply(float x) const noexcept { return std::tanh(x); }

  float antiderivative(float x) const noexcept {
    const float ax = std::abs(x);
    return ax + std::log1p(std::exp(-2.0f * ax)) - std::log(2.0f);
  }
};

/// @brief Push-pull output pair: two one-sided devices driven in antiphase and
///        summed by the output transformer.
/// @details Each half sees the signal offset by @c bias, so the composite
///   @c tanh(knee*(x+bias)) + tanh(knee*(x-bias)) is odd by construction —
///   which is why a push-pull stage cancels even harmonics here rather than
///   being asserted to.
///
///   @c bias is how far into class B the pair is biased: it is the half-width
///   of the region around the zero crossing where neither half is conducting
///   properly, i.e. the crossover dead zone. @c knee is how abruptly each half
///   turns on relative to the signal; a wide dead zone with a soft knee is only
///   a gentle expander, and it takes both to get the kink at the handover that
///   makes crossover distortion sound the way it does.
///
///   The composite is normalized by its own SATURATED output, not by its slope
///   at the origin: the supply rails do not move when an amp's bias is
///   readjusted, so the ceiling is what has to stay fixed, and the small-signal
///   slope is what falls. Normalizing the other way round would make a colder
///   bias louder and cleaner, which is backwards on both counts.
///
///   At @c bias == 0 and @c knee == 1 the whole function collapses onto
///   TanhNonlinearity bit-exactly: @c 1.0f*(x+-0.0f) is exact, and
///   @c f(x)+f(x) scaled by @c 0.5f only moves a binary exponent. Class A is
///   therefore the same arithmetic as before, with no branch.
///
///   PITFALL for callers driving both fields from one control: the origin
///   slope is @c knee*sech^2(knee*bias), and to second order in @c bias that
///   is @c knee*(1-(knee*bias)^2). Ramping @c knee linearly in the control
///   while @c bias starts at 0 makes the slope RISE at first — a sharper knee
///   reads as gain before a wide-enough dead zone has caught up to cut it back
///   down. A caller wanting the slope to fall monotonically as the dead zone
///   opens needs @c knee's excess over 1 to scale with @c bias SQUARED (not
///   with the control directly), which cancels the first-order term instead of
///   fighting it — see @c saturation::AmpSim's @c crossover parameter for the
///   worked derivation and its bound on the scaling coefficient.
struct PushPullNonlinearity {
  float bias = 0.0f;
  float knee = 1.0f;

  /// The 0.5f is the SATURATED-output normalizer and must not carry @c knee:
  /// each half tops out at 1, so the pair tops out at 2 whatever the knee is.
  /// Folding the knee in here would shrink the ceiling as the turn-on sharpens,
  /// which reads as "a colder bias compresses harder" instead of "a colder bias
  /// has a dead zone" — the two are easy to confuse in a level measurement and
  /// they are not the same effect.
  float apply(float x) const noexcept {
    const TanhNonlinearity half;
    return (half.apply(knee * (x + bias)) + half.apply(knee * (x - bias))) * 0.5f;
  }

  /// The antiderivative DOES carry it, from the chain rule on @c knee*x.
  float antiderivative(float x) const noexcept {
    const TanhNonlinearity half;
    return (half.antiderivative(knee * (x + bias)) + half.antiderivative(knee * (x - bias))) *
           (0.5f / knee);
  }
};

struct ArctanNonlinearity {
  float apply(float x) const noexcept { return std::atan(x); }

  float antiderivative(float x) const noexcept {
    return x * std::atan(x) - 0.5f * std::log1p(x * x);
  }

  float second_antiderivative(float x) const noexcept {
    const float x2 = x * x;
    return 0.5f * (x2 - 1.0f) * std::atan(x) + 0.5f * x - 0.5f * x * std::log1p(x2);
  }
};

struct HardClipNonlinearity {
  float limit = 1.0f;

  float apply(float x) const noexcept { return std::clamp(x, -limit, limit); }

  float antiderivative(float x) const noexcept {
    const float ax = std::abs(x);
    if (ax <= limit) {
      return 0.5f * x * x;
    }
    return limit * ax - 0.5f * limit * limit;
  }

  float second_antiderivative(float x) const noexcept {
    const float ax = std::abs(x);
    if (ax <= limit) {
      return x * x * x / 6.0f;
    }
    if (x > 0.0f) {
      return limit * x * x * 0.5f - limit * limit * x * 0.5f + limit * limit * limit / 6.0f;
    }
    return -limit * x * x * 0.5f - limit * limit * x * 0.5f - limit * limit * limit / 6.0f;
  }
};

struct CubicSoftClipNonlinearity {
  float apply(float x) const noexcept {
    const float clipped = std::clamp(x, -1.0f, 1.0f);
    return clipped - (clipped * clipped * clipped) / 3.0f;
  }

  float antiderivative(float x) const noexcept {
    const float clipped = std::clamp(x, -1.0f, 1.0f);
    const float inside = 0.5f * clipped * clipped - (clipped * clipped * clipped * clipped) / 12.0f;
    if (x > 1.0f) {
      return (5.0f / 12.0f) + (2.0f / 3.0f) * (x - 1.0f);
    }
    if (x < -1.0f) {
      return (5.0f / 12.0f) - (2.0f / 3.0f) * (x + 1.0f);
    }
    return inside;
  }

  float second_antiderivative(float x) const noexcept {
    if (x > 1.0f) {
      return x * x / 3.0f - x * 0.25f + 1.0f / 15.0f;
    }
    if (x < -1.0f) {
      return -x * x / 3.0f - x * 0.25f - 1.0f / 15.0f;
    }
    return x * x * x / 6.0f - x * x * x * x * x / 60.0f;
  }
};

}  // namespace sonare::rt
