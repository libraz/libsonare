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
