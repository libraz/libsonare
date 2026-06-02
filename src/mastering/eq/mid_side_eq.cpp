#include "mastering/eq/mid_side_eq.h"

#include <algorithm>

#include "rt/scoped_no_denormals.h"
#include "util/exception.h"

namespace sonare::mastering::eq {

void MidSideEq::prepare(double sample_rate, int max_block_size) {
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }
  mid_eq_.prepare(sample_rate, max_block_size);
  side_eq_.prepare(sample_rate, max_block_size);
  max_block_size_ = max_block_size;
  if (max_block_size > 0) {
    mid_buffer_.assign(static_cast<size_t>(max_block_size), 0.0f);
    side_buffer_.assign(static_cast<size_t>(max_block_size), 0.0f);
  }
}

void MidSideEq::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  if (num_channels < 0 || num_samples < 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (num_channels != 2) {
    throw SonareException(ErrorCode::InvalidParameter, "MidSideEq requires exactly two channels");
  }
  if (channels == nullptr || channels[0] == nullptr || channels[1] == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  }

  // Process in chunks bounded by the prepared scratch so the audio thread never
  // reallocates the mid/side buffers, even if a caller passes a block larger
  // than the prepared max_block_size. Biquad state persists across chunks, so
  // the result is identical to processing the whole block at once. When prepared
  // offline (max_block_size_ == 0) we size the scratch once to the full block.
  const int chunk = max_block_size_ > 0 ? max_block_size_ : num_samples;
  ensure_buffers(chunk);
  float* const mid = mid_buffer_.data();
  float* const side = side_buffer_.data();

  for (int start = 0; start < num_samples; start += chunk) {
    const int n = std::min(chunk, num_samples - start);
    for (int i = 0; i < n; ++i) {
      const float left = channels[0][start + i];
      const float right = channels[1][start + i];
      mid[i] = (left + right) * 0.5f;
      side[i] = (left - right) * 0.5f;
    }

    float* mid_channels[] = {mid};
    float* side_channels[] = {side};
    mid_eq_.process(mid_channels, 1, n);
    side_eq_.process(side_channels, 1, n);

    for (int i = 0; i < n; ++i) {
      channels[0][start + i] = mid[i] + side[i];
      channels[1][start + i] = mid[i] - side[i];
    }
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
