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

}  // namespace sonare::mastering::stereo
