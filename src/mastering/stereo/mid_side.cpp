#include "mastering/stereo/mid_side.h"

#include <stdexcept>

namespace sonare::mastering::stereo {

namespace {

void validate_buffers(const float* a, const float* b, float* c, float* d, size_t length) {
  if (length == 0) {
    return;
  }
  if (a == nullptr || b == nullptr || c == nullptr || d == nullptr) {
    throw std::invalid_argument("mid/side buffers must not be null");
  }
}

}  // namespace

MidSideSample encode_sample(float left, float right) {
  return {(left + right) * 0.5f, (left - right) * 0.5f};
}

MidSideSample decode_sample(float mid, float side) { return {mid + side, mid - side}; }

void encode_buffer(const float* left, const float* right, float* mid, float* side, size_t length) {
  validate_buffers(left, right, mid, side, length);
  for (size_t i = 0; i < length; ++i) {
    const auto encoded = encode_sample(left[i], right[i]);
    mid[i] = encoded.mid;
    side[i] = encoded.side;
  }
}

void decode_buffer(const float* mid, const float* side, float* left, float* right, size_t length) {
  validate_buffers(mid, side, left, right, length);
  for (size_t i = 0; i < length; ++i) {
    const auto decoded = decode_sample(mid[i], side[i]);
    left[i] = decoded.mid;
    right[i] = decoded.side;
  }
}

}  // namespace sonare::mastering::stereo
