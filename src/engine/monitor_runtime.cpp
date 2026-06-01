#include "engine/monitor_runtime.h"

#include <algorithm>

namespace sonare::engine {

void MonitorRuntime::prepare(double sample_rate, int max_block_size, float smoothing_ms) noexcept {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  max_block_size_ = std::max(max_block_size, 1);
  smoothing_ms_ = std::max(smoothing_ms, 0.0f);
  for (StripState& state : strips_) {
    state.mute_gain.prepare(sample_rate_, smoothing_ms_);
    state.mute_gain.reset(1.0f);
  }
}

bool MonitorRuntime::add_strip(mixing::ChannelStrip* strip) noexcept {
  // Control thread is the sole writer of size_, so relaxed loads suffice for
  // reading the current count here; the new count is published with a release
  // store below.
  const size_t count = size_.load(std::memory_order_relaxed);
  if (!strip || count >= strips_.size()) return false;
  for (size_t i = 0; i < count; ++i) {
    if (strips_[i].strip == strip) return false;
  }
  StripState& state = strips_[count];
  state = {};
  state.strip = strip;
  state.mute_gain.prepare(sample_rate_, smoothing_ms_);
  state.mute_gain.reset(1.0f);
  // Publish the fully initialized slot before the audio thread can observe the
  // incremented count via an acquire load.
  size_.store(count + 1, std::memory_order_release);
  recompute_solo_mutes();
  return true;
}

bool MonitorRuntime::remove_strip(mixing::ChannelStrip* strip) noexcept {
  const size_t count = size_.load(std::memory_order_relaxed);
  for (size_t i = 0; i < count; ++i) {
    if (strips_[i].strip != strip) continue;
    for (size_t j = i + 1; j < count; ++j) {
      strips_[j - 1] = strips_[j];
    }
    // Shrink the published count first so the audio thread stops touching the
    // tail slot, then clear it.
    size_.store(count - 1, std::memory_order_release);
    strips_[count - 1] = {};
    recompute_solo_mutes();
    return true;
  }
  return false;
}

bool MonitorRuntime::contains(mixing::ChannelStrip* strip) const noexcept {
  if (!strip) return false;
  const size_t count = size_.load(std::memory_order_acquire);
  for (size_t i = 0; i < count; ++i) {
    if (strips_[i].strip == strip) return true;
  }
  return false;
}

void MonitorRuntime::set_mute(size_t index, bool muted) noexcept {
  if (!valid_index(index)) return;
  strips_[index].muted.store(muted, std::memory_order_release);
  update_target(strips_[index]);
}

void MonitorRuntime::set_solo(size_t index, bool soloed) noexcept {
  if (!valid_index(index)) return;
  strips_[index].soloed.store(soloed, std::memory_order_release);
  recompute_solo_mutes();
}

void MonitorRuntime::set_exclusive_solo(size_t index, bool soloed) noexcept {
  if (!valid_index(index)) return;
  const size_t count = size_.load(std::memory_order_relaxed);
  for (size_t i = 0; i < count; ++i) {
    strips_[i].soloed.store((i == index) && soloed, std::memory_order_release);
  }
  recompute_solo_mutes();
}

void MonitorRuntime::set_solo_safe(size_t index, bool solo_safe) noexcept {
  if (!valid_index(index)) return;
  strips_[index].solo_safe.store(solo_safe, std::memory_order_release);
  recompute_solo_mutes();
}

void MonitorRuntime::set_monitor_mode(size_t index, MonitorMode mode) noexcept {
  if (!valid_index(index)) return;
  strips_[index].monitor.store(mode, std::memory_order_release);
}

void MonitorRuntime::process_strip(size_t index, float* const* channels, int num_channels,
                                   int num_samples, int64_t timeline_sample,
                                   float* const* monitor_out) noexcept {
  if (!valid_index(index) || !channels || num_channels <= 0 || num_samples <= 0) return;
  StripState& state = strips_[index];

  const MonitorMode monitor = state.monitor.load(std::memory_order_acquire);
  if (monitor_out && monitor == MonitorMode::kPfl) {
    for (int ch = 0; ch < num_channels; ++ch) {
      if (!channels[ch] || !monitor_out[ch]) continue;
      for (int i = 0; i < num_samples; ++i) {
        monitor_out[ch][i] += channels[ch][i];
      }
    }
  }

  if (state.strip) {
    state.strip->process_at(channels, num_channels, num_samples, timeline_sample);
  }

  for (int i = 0; i < num_samples; ++i) {
    const float gain = state.mute_gain.process();
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch]) channels[ch][i] *= gain;
    }
  }

  if (monitor_out && monitor == MonitorMode::kAfl) {
    for (int ch = 0; ch < num_channels; ++ch) {
      if (!channels[ch] || !monitor_out[ch]) continue;
      for (int i = 0; i < num_samples; ++i) {
        monitor_out[ch][i] += channels[ch][i];
      }
    }
  }
}

bool MonitorRuntime::muted(size_t index) const noexcept {
  return valid_index(index) && strips_[index].muted.load(std::memory_order_acquire);
}

bool MonitorRuntime::soloed(size_t index) const noexcept {
  return valid_index(index) && strips_[index].soloed.load(std::memory_order_acquire);
}

bool MonitorRuntime::solo_safe(size_t index) const noexcept {
  return valid_index(index) && strips_[index].solo_safe.load(std::memory_order_acquire);
}

bool MonitorRuntime::implied_mute(size_t index) const noexcept {
  return valid_index(index) && strips_[index].implied_mute.load(std::memory_order_acquire);
}

MonitorMode MonitorRuntime::monitor_mode(size_t index) const noexcept {
  return valid_index(index) ? strips_[index].monitor.load(std::memory_order_acquire)
                            : MonitorMode::kOff;
}

void MonitorRuntime::recompute_solo_mutes() noexcept {
  const size_t count = size_.load(std::memory_order_relaxed);
  bool any_solo = false;
  for (size_t i = 0; i < count; ++i) {
    any_solo = any_solo || strips_[i].soloed.load(std::memory_order_relaxed);
  }
  for (size_t i = 0; i < count; ++i) {
    const bool implied = any_solo && !strips_[i].soloed.load(std::memory_order_relaxed) &&
                         !strips_[i].solo_safe.load(std::memory_order_relaxed);
    strips_[i].implied_mute.store(implied, std::memory_order_release);
    update_target(strips_[i]);
  }
}

void MonitorRuntime::update_target(StripState& state) noexcept {
  const bool effectively_muted = state.muted.load(std::memory_order_relaxed) ||
                                 state.implied_mute.load(std::memory_order_relaxed);
  state.mute_gain.set_target(effectively_muted ? 0.0f : 1.0f);
}

}  // namespace sonare::engine
