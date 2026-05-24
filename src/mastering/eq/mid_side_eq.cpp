#include "mastering/eq/mid_side_eq.h"

#include <stdexcept>

#include "mastering/common/scoped_no_denormals.h"

namespace sonare::mastering::eq {

void MidSideEq::prepare(double sample_rate, int max_block_size) {
  mid_eq_.prepare(sample_rate, max_block_size);
  side_eq_.prepare(sample_rate, max_block_size);
  if (max_block_size > 0) {
    mid_buffer_.assign(static_cast<size_t>(max_block_size), 0.0f);
    side_buffer_.assign(static_cast<size_t>(max_block_size), 0.0f);
  }
}

void MidSideEq::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (num_channels != 2) {
    throw std::invalid_argument("MidSideEq requires exactly two channels");
  }
  if (channels == nullptr || channels[0] == nullptr || channels[1] == nullptr) {
    throw std::invalid_argument("channels must not be null");
  }

  ensure_buffers(num_samples);
  float* const mid = mid_buffer_.data();
  float* const side = side_buffer_.data();
  for (int i = 0; i < num_samples; ++i) {
    const float left = channels[0][i];
    const float right = channels[1][i];
    mid[i] = (left + right) * 0.5f;
    side[i] = (left - right) * 0.5f;
  }

  float* mid_channels[] = {mid};
  float* side_channels[] = {side};
  mid_eq_.process(mid_channels, 1, num_samples);
  side_eq_.process(side_channels, 1, num_samples);

  for (int i = 0; i < num_samples; ++i) {
    channels[0][i] = mid[i] + side[i];
    channels[1][i] = mid[i] - side[i];
  }
}

void MidSideEq::reset() {
  mid_eq_.reset();
  side_eq_.reset();
}

void MidSideEq::set_mid_band(size_t index, const EqBand& band) { mid_eq_.set_band(index, band); }

void MidSideEq::set_side_band(size_t index, const EqBand& band) { side_eq_.set_band(index, band); }

void MidSideEq::clear_mid_band(size_t index) { mid_eq_.clear_band(index); }

void MidSideEq::clear_side_band(size_t index) { side_eq_.clear_band(index); }

void MidSideEq::clear() {
  mid_eq_.clear();
  side_eq_.clear();
}

const EqBand& MidSideEq::mid_band(size_t index) const { return mid_eq_.band(index); }

const EqBand& MidSideEq::side_band(size_t index) const { return side_eq_.band(index); }

void MidSideEq::ensure_buffers(int num_samples) {
  if (mid_buffer_.size() < static_cast<size_t>(num_samples)) {
    mid_buffer_.assign(static_cast<size_t>(num_samples), 0.0f);
    side_buffer_.assign(static_cast<size_t>(num_samples), 0.0f);
  }
}

}  // namespace sonare::mastering::eq
