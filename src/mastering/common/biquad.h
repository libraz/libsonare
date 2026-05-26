#pragma once

namespace sonare::mastering::common {

struct Biquad {
  float b0 = 1.0f;
  float b1 = 0.0f;
  float b2 = 0.0f;
  float a1 = 0.0f;
  float a2 = 0.0f;
  float z1 = 0.0f;
  float z2 = 0.0f;

  float process(float x) noexcept {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }

  void reset() noexcept {
    z1 = 0.0f;
    z2 = 0.0f;
  }
};

}  // namespace sonare::mastering::common
