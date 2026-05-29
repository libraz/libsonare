#include "mastering/eq/tilt.h"

#include <algorithm>
#include <stdexcept>

#include "util/constants.h"

namespace sonare::mastering::eq {

void TiltEq::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }

  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  prepared_ = true;
  eq_.prepare(sample_rate_, max_block_size_);
  update_bands();
}

void TiltEq::process(float* const* channels, int num_channels, int num_samples) {
  ensure_prepared(prepared_, "TiltEq");
  eq_.process(channels, num_channels, num_samples);
}

void TiltEq::reset() { eq_.reset(); }

void TiltEq::set_tilt_db(float tilt_db) {
  tilt_db_ = tilt_db;
  if (prepared_) {
    update_bands();
  }
}

void TiltEq::set_pivot_hz(float pivot_hz) {
  if (!(pivot_hz > 0.0f)) {
    throw std::invalid_argument("pivot_hz must be positive");
  }
  pivot_hz_ = pivot_hz;
  if (prepared_) {
    update_bands();
  }
}

bool TiltEq::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      tilt_db_ = value;
      break;
    case 1:
      // Clamp to (0 Hz, Nyquist) so coefficient design never throws on the
      // audio thread.
      pivot_hz_ = std::clamp(value, 1.0e-3f, static_cast<float>(sample_rate_ * 0.5) - 1.0e-3f);
      break;
    default:
      return false;
  }
  if (prepared_) {
    update_bands();
  }
  return true;
}

void TiltEq::update_bands() {
  const bool enabled = tilt_db_ != 0.0f;
  const float half_tilt = tilt_db_ * 0.5f;
  constexpr float q = sonare::constants::kButterworthQ;

  eq_.set_band(0, {EqBandType::LowShelf, pivot_hz_, -half_tilt, q, enabled});
  eq_.set_band(1, {EqBandType::HighShelf, pivot_hz_, half_tilt, q, enabled});
  for (size_t i = 2; i < ParametricEq::kMaxBands; ++i) {
    eq_.clear_band(i);
  }
}

}  // namespace sonare::mastering::eq
