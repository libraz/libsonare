#pragma once

/// @file fractional_delay.h
/// @brief Shared 3rd-order Lagrange fractional-sample delay kernel.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace sonare::rt {

/// @brief Writes @p input into a circular buffer and reads back a fractionally
///        delayed sample using a 3rd-order Lagrange FIR interpolator.
///
/// The delay is expressed in Q8.8 fixed-point samples (256 == one sample). For
/// the normal case (integer part @c base >= 1) a CENTRED stencil
/// {base-1, base, base+1, base+2} is used — this is the most accurate 4-point
/// Lagrange and matches the long-standing behaviour. The only degenerate case
/// is a sub-sample delay (base == 0): the centred stencil's `base-1` tap would
/// read delay -1 (a future sample), which clamps to delay 0 and collapses onto
/// the `base` tap (y0 == y1), corrupting the interpolation. For base == 0 we
/// therefore fall back to the strictly causal stencil {0, 1, 2, 3}, which reads
/// no future/clamped tap and stays well-formed across the whole sub-sample
/// range. Both branches interpolate at the same physical delay (base + mu).
///
/// @param buffer          Circular delay buffer (must be non-null).
/// @param size            Buffer length in samples (must be > 0).
/// @param write_index     Current write position; advanced by one on return.
/// @param delay_samples_q8 Requested delay in Q8.8 samples (negatives clamp to 0).
/// @param input           New input sample to push into the buffer.
/// @return The fractionally delayed output sample.
inline float lagrange3_fractional_delay(float* buffer, size_t size, size_t& write_index,
                                        int delay_samples_q8, float input) noexcept {
  auto sample_at_delay = [&](int delay) {
    delay = std::max(0, delay);
    const size_t index = (write_index + size - (static_cast<size_t>(delay) % size)) % size;
    return buffer[index];
  };

  buffer[write_index] = input;
  const float delay = static_cast<float>(std::max(0, delay_samples_q8)) / 256.0f;
  const int base = static_cast<int>(std::floor(delay));
  const float mu = delay - static_cast<float>(base);

  float y0, y1, y2, y3;
  float c0, c1, c2, c3;
  if (base >= 1) {
    // Centred stencil over delays {base-1, base, base+1, base+2}; Lagrange nodes
    // {-1, 0, 1, 2} evaluated at fractional position mu (physical delay base+mu).
    y0 = sample_at_delay(base - 1);
    y1 = sample_at_delay(base);
    y2 = sample_at_delay(base + 1);
    y3 = sample_at_delay(base + 2);
    c0 = -mu * (mu - 1.0f) * (mu - 2.0f) / 6.0f;
    c1 = (mu + 1.0f) * (mu - 1.0f) * (mu - 2.0f) / 2.0f;
    c2 = -(mu + 1.0f) * mu * (mu - 2.0f) / 2.0f;
    c3 = (mu + 1.0f) * mu * (mu - 1.0f) / 6.0f;
  } else {
    // Sub-sample delay: causal stencil over delays {0, 1, 2, 3}; Lagrange nodes
    // {0, 1, 2, 3} at mu, with no future/clamped tap so y0 and y1 stay distinct.
    y0 = sample_at_delay(0);
    y1 = sample_at_delay(1);
    y2 = sample_at_delay(2);
    y3 = sample_at_delay(3);
    c0 = -(mu - 1.0f) * (mu - 2.0f) * (mu - 3.0f) / 6.0f;
    c1 = mu * (mu - 2.0f) * (mu - 3.0f) / 2.0f;
    c2 = -mu * (mu - 1.0f) * (mu - 3.0f) / 2.0f;
    c3 = mu * (mu - 1.0f) * (mu - 2.0f) / 6.0f;
  }

  write_index = (write_index + 1) % size;
  return c0 * y0 + c1 * y1 + c2 * y2 + c3 * y3;
}

/// @brief Vector convenience overload (the long-standing call form).
inline float lagrange3_fractional_delay(std::vector<float>& buffer, size_t& write_index,
                                        int delay_samples_q8, float input) noexcept {
  return lagrange3_fractional_delay(buffer.data(), buffer.size(), write_index, delay_samples_q8,
                                    input);
}

}  // namespace sonare::rt
