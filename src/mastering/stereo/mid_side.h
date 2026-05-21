#pragma once

/// @file mid_side.h
/// @brief Mid/side encode and decode helpers.

#include <cstddef>

namespace sonare::mastering::stereo {

struct MidSideSample {
  float mid = 0.0f;
  float side = 0.0f;
};

MidSideSample encode_sample(float left, float right);
MidSideSample decode_sample(float mid, float side);

void encode_buffer(const float* left, const float* right, float* mid, float* side, size_t length);
void decode_buffer(const float* mid, const float* side, float* left, float* right, size_t length);

}  // namespace sonare::mastering::stereo
