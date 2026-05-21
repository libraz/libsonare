#include "mastering/common/oversampler.h"

#include <algorithm>

#include "util/exception.h"

namespace sonare::mastering::common {
namespace {

bool is_supported_factor(int factor) { return factor == 2 || factor == 4 || factor == 8; }

}  // namespace

Oversampler::Oversampler(int factor) { set_factor(factor); }

void Oversampler::set_factor(int factor) {
  SONARE_CHECK(is_supported_factor(factor), ErrorCode::InvalidParameter);
  factor_ = factor;
}

std::vector<float> Oversampler::upsample(const float* input, size_t size) const {
  if (size == 0) return {};
  SONARE_CHECK(input != nullptr, ErrorCode::InvalidParameter);

  std::vector<float> output(size * static_cast<size_t>(factor_));
  if (size == 1) {
    std::fill(output.begin(), output.end(), input[0]);
    return output;
  }

  for (size_t i = 0; i < size - 1; ++i) {
    const float a = input[i];
    const float b = input[i + 1];
    for (int phase = 0; phase < factor_; ++phase) {
      const float t = static_cast<float>(phase) / static_cast<float>(factor_);
      output[i * static_cast<size_t>(factor_) + static_cast<size_t>(phase)] = a + (b - a) * t;
    }
  }

  const size_t last_base = (size - 1) * static_cast<size_t>(factor_);
  std::fill(output.begin() + static_cast<long>(last_base), output.end(), input[size - 1]);
  return output;
}

std::vector<float> Oversampler::upsample(const std::vector<float>& input) const {
  return upsample(input.data(), input.size());
}

std::vector<float> Oversampler::downsample(const float* input, size_t size) const {
  if (size == 0) return {};
  SONARE_CHECK(input != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(size % static_cast<size_t>(factor_) == 0, ErrorCode::InvalidParameter);

  const size_t out_size = size / static_cast<size_t>(factor_);
  std::vector<float> output(out_size);
  for (size_t i = 0; i < out_size; ++i) {
    output[i] = input[i * static_cast<size_t>(factor_)];
  }
  return output;
}

std::vector<float> Oversampler::downsample(const std::vector<float>& input) const {
  return downsample(input.data(), input.size());
}

}  // namespace sonare::mastering::common
