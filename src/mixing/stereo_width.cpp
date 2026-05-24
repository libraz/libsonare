#include "mixing/stereo_width.h"

#include <algorithm>

namespace sonare::mixing {

StereoWidthProcessor::StereoWidthProcessor(float width) : width_(std::clamp(width, 0.0f, 2.0f)) {}

void StereoWidthProcessor::prepare(double, int) {}

void StereoWidthProcessor::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels < 2 || num_samples <= 0 || channels[0] == nullptr ||
      channels[1] == nullptr) {
    return;
  }
  for (int i = 0; i < num_samples; ++i) {
    const float mid = 0.5f * (channels[0][i] + channels[1][i]);
    const float side = 0.5f * (channels[0][i] - channels[1][i]) * width_;
    channels[0][i] = mid + side;
    channels[1][i] = mid - side;
  }
}

void StereoWidthProcessor::set_width(float width) noexcept {
  width_ = std::clamp(width, 0.0f, 2.0f);
}

}  // namespace sonare::mixing
