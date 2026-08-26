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
///   @c bias is the half-width of the crossover dead zone (how far into class B
///   the pair sits) and @c knee is how abruptly each half turns on. It takes
///   both to get the kink at the handover: a wide dead zone with a soft knee is
///   only a gentle expander.
///
///   The composite is normalized by its SATURATED output rather than by its
///   origin slope, because the supply rails do not move when an amp's bias is
///   readjusted — so the ceiling stays fixed and the small-signal slope falls.
///   The other way round would make a colder bias louder and cleaner.
///
///   At @c bias == 0 and @c knee == 1 this collapses onto TanhNonlinearity
///   bit-exactly, so class A is the same arithmetic with no branch.
///
///   PITFALL when both fields come from one control: the origin slope is
///   @c knee*sech^2(knee*bias) ~ @c knee*(1-(knee*bias)^2), so ramping @c knee
///   linearly from @c bias == 0 makes the slope RISE at first — the sharper knee
///   reads as gain before the dead zone catches up. For a monotonic fall,
///   @c knee's excess over 1 must scale with @c bias SQUARED, which cancels the
///   first-order term; @c saturation::AmpSim's @c crossover has the derivation.
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
