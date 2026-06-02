#include "effects/common/dc_blocker.h"

#include <algorithm>
#include <cmath>

#include "mastering/dynamics/channel_limits.h"
#include "util/constants.h"

namespace sonare::effects::common {

using sonare::constants::kTwoPi;
using sonare::mastering::dynamics::kRealtimePreparedChannels;

DcBlocker::DcBlocker(float pole) { set_pole(pole); }

void DcBlocker::prepare(double sample_rate, int) {
  if (sample_rate > 0.0) {
    const float pole =
        std::exp(-sonare::constants::kTwoPi * cutoff_hz_ / static_cast<float>(sample_rate));
    set_pole(pole);
  }
  // Preallocate per-channel state up front so the audio-thread process() path
  // never resizes (which would malloc). Blocks wider than the prepared state are
  // clamped in process() and never trigger an allocation. Mirrors the dynamics
  // processors (limiter.cpp).
  x1_.assign(kRealtimePreparedChannels, 0.0f);
  y1_.assign(kRealtimePreparedChannels, 0.0f);
  reset();
}

void DcBlocker::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }
  // Lazily preallocate when process() is reached without a prior prepare() (e.g.
  // a default-constructed instance used directly). This single allocation never
  // recurs on the audio thread because subsequent blocks are clamped to the
  // prepared width.
  if (x1_.empty()) {
    x1_.assign(kRealtimePreparedChannels, 0.0f);
    y1_.assign(kRealtimePreparedChannels, 0.0f);
  }
  // Never allocate on the audio thread: clamp to the preallocated state width
  // rather than resizing for wider blocks.
  const int channel_count = std::min(num_channels, static_cast<int>(x1_.size()));
  for (int ch = 0; ch < channel_count; ++ch) {
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
