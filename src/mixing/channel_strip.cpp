#include "mixing/channel_strip.h"

#include <algorithm>

namespace sonare::mixing {

ChannelStrip::ChannelStrip(ChannelStripConfig config)
    : fader_({config.fader_db, config.smoothing_ms}),
      panner_({config.pan, config.pan_law, config.smoothing_ms}) {}

void ChannelStrip::prepare(double sample_rate, int max_block_size) {
  fader_.prepare(sample_rate, max_block_size);
  panner_.prepare(sample_rate, max_block_size);
}

void ChannelStrip::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }

  if (effectively_muted()) {
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] != nullptr) {
        std::fill(channels[ch], channels[ch] + num_samples, 0.0f);
      }
    }
    return;
  }

  fader_.process(channels, num_channels, num_samples);
  panner_.process(channels, num_channels, num_samples);
}

void ChannelStrip::reset() {
  fader_.reset();
  panner_.reset();
}

void ChannelStrip::set_muted(bool muted) noexcept {
  muted_.store(muted, std::memory_order_relaxed);
}

bool ChannelStrip::muted() const noexcept { return muted_.load(std::memory_order_relaxed); }

bool ChannelStrip::effectively_muted() const noexcept {
  return muted() || (implied_mute() && !solo_safe());
}

void ChannelStrip::set_soloed(bool soloed) noexcept {
  soloed_.store(soloed, std::memory_order_relaxed);
}

bool ChannelStrip::soloed() const noexcept { return soloed_.load(std::memory_order_relaxed); }

void ChannelStrip::set_solo_safe(bool solo_safe) noexcept {
  solo_safe_.store(solo_safe, std::memory_order_relaxed);
}

bool ChannelStrip::solo_safe() const noexcept { return solo_safe_.load(std::memory_order_relaxed); }

void ChannelStrip::set_implied_mute(bool implied_mute) noexcept {
  implied_mute_.store(implied_mute, std::memory_order_relaxed);
}

bool ChannelStrip::implied_mute() const noexcept {
  return implied_mute_.load(std::memory_order_relaxed);
}

}  // namespace sonare::mixing
