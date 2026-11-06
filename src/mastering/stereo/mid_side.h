#pragma once

/// @file mid_side.h
/// @brief Mid/side encode and decode helpers (energy-preserving, AES convention).
/// @details Encode and decode both apply the symmetric 1/sqrt(2) factor, so
///          M^2 + S^2 == L^2 + R^2 (per sample). This matches the canonical
///          AES/EBU mid-side definition and lets downstream processors apply
///          gain in the M/S domain without changing total loudness.
///          encode -> decode is bit-exact within floating-point round-off
///          (1/sqrt(2) * 1/sqrt(2) == 1/2 exactly in IEEE-754 double, with
///          the float variant losing only a few ULPs).

#include <cstddef>

namespace sonare::mastering::stereo {

struct MidSideSample {
  float mid = 0.0f;
  float side = 0.0f;
};

/// @brief Encode L/R to M/S using the energy-preserving 1/sqrt(2) factor.
/// @return {mid = (L+R)/sqrt(2), side = (L-R)/sqrt(2)}.
MidSideSample encode_sample(float left, float right);

/// @brief Decode M/S back to L/R using the energy-preserving 1/sqrt(2) factor.
/// @return {left = (M+S)/sqrt(2), right = (M-S)/sqrt(2)}.
MidSideSample decode_sample(float mid, float side);

void encode_buffer(const float* left, const float* right, float* mid, float* side, size_t length);
void decode_buffer(const float* mid, const float* side, float* left, float* right, size_t length);

/// @brief Encode L/R to M/S with the half-scale convention: mid = (L+R)/2, side = (L-R)/2.
/// @details Not energy-preserving; pairs with decode_buffer_half for an exact round trip.
inline void encode_buffer_half(const float* left, const float* right, float* mid, float* side,
                               size_t length) {
  for (size_t i = 0; i < length; ++i) {
    mid[i] = (left[i] + right[i]) * 0.5f;
    side[i] = (left[i] - right[i]) * 0.5f;
  }
}

/// @brief Decode half-scale M/S back to L/R: left = M+S, right = M-S.
inline void decode_buffer_half(const float* mid, const float* side, float* left, float* right,
                               size_t length) {
  for (size_t i = 0; i < length; ++i) {
    const float m = mid[i];
    const float s = side[i];
    left[i] = m + s;
    right[i] = m - s;
  }
}

}  // namespace sonare::mastering::stereo
