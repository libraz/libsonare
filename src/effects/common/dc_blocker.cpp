#include "effects/common/dc_blocker.h"

#include <algorithm>

#include "util/constants.h"

namespace sonare::effects::common {

using sonare::constants::kTwoPi;

DcBlocker::DcBlocker(float pole) { set_pole(pole); }

void DcBlocker::prepare(double sample_rate, int) {
  // Derive the pole from the desired corner frequency so the HPF stays at a
  // fixed cutoff (~20 Hz) regardless of sample rate. For a one-pole high pass,
  // pole = 1 - 2*pi*fc/sr (valid while fc << sr).
  if (sample_rate > 0.0) {
    const float sr = static_cast<float>(sample_rate);
    const float pole = 1.0f - kTwoPi * cutoff_hz_ / sr;
    set_pole(pole);
  }
  reset();
}

void DcBlocker::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }
  if (x1_.size() != static_cast<size_t>(num_channels)) {
    x1_.assign(static_cast<size_t>(num_channels), 0.0f);
    y1_.assign(static_cast<size_t>(num_channels), 0.0f);
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      continue;
    }
    float x1 = x1_[static_cast<size_t>(ch)];
    float y1 = y1_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      const float x = channels[ch][i];
      const float y = x - x1 + pole_ * y1;
      channels[ch][i] = y;
      x1 = x;
      y1 = y;
    }
    x1_[static_cast<size_t>(ch)] = x1;
    y1_[static_cast<size_t>(ch)] = y1;
  }
}

void DcBlocker::reset() {
  std::fill(x1_.begin(), x1_.end(), 0.0f);
  std::fill(y1_.begin(), y1_.end(), 0.0f);
}

void DcBlocker::set_pole(float pole) noexcept { pole_ = std::clamp(pole, 0.0f, 0.9999f); }

void DcBlocker::set_cutoff_hz(float cutoff_hz) noexcept { cutoff_hz_ = std::max(0.0f, cutoff_hz); }

}  // namespace sonare::effects::common
