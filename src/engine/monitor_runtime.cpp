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
  if (!strip || size_ >= strips_.size()) return false;
  for (size_t i = 0; i < size_; ++i) {
    if (strips_[i].strip == strip) return false;
  }
  StripState& state = strips_[size_++];
  state = {};
  state.strip = strip;
  state.mute_gain.prepare(sample_rate_, smoothing_ms_);
  state.mute_gain.reset(1.0f);
  recompute_solo_mutes();
  return true;
}

bool MonitorRuntime::remove_strip(mixing::ChannelStrip* strip) noexcept {
  for (size_t i = 0; i < size_; ++i) {
    if (strips_[i].strip != strip) continue;
    for (size_t j = i + 1; j < size_; ++j) {
      strips_[j - 1] = strips_[j];
    }
    strips_[--size_] = {};
    recompute_solo_mutes();
    return true;
  }
  return false;
}

void MonitorRuntime::set_mute(size_t index, bool muted) noexcept {
  if (!valid_index(index)) return;
  strips_[index].muted = muted;
  update_target(strips_[index]);
}

void MonitorRuntime::set_solo(size_t index, bool soloed) noexcept {
  if (!valid_index(index)) return;
  strips_[index].soloed = soloed;
  recompute_solo_mutes();
}

void MonitorRuntime::set_exclusive_solo(size_t index, bool soloed) noexcept {
  if (!valid_index(index)) return;
  for (size_t i = 0; i < size_; ++i) {
    strips_[i].soloed = (i == index) && soloed;
  }
  recompute_solo_mutes();
}

void MonitorRuntime::set_solo_safe(size_t index, bool solo_safe) noexcept {
  if (!valid_index(index)) return;
  strips_[index].solo_safe = solo_safe;
  recompute_solo_mutes();
}

void MonitorRuntime::set_monitor_mode(size_t index, MonitorMode mode) noexcept {
  if (!valid_index(index)) return;
  strips_[index].monitor = mode;
}

void MonitorRuntime::process_strip(size_t index, float* const* channels, int num_channels,
                                   int num_samples, int64_t timeline_sample,
                                   float* const* monitor_out) noexcept {
  if (!valid_index(index) || !channels || num_channels <= 0 || num_samples <= 0) return;
  StripState& state = strips_[index];

  if (monitor_out && state.monitor == MonitorMode::kPfl) {
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

  if (monitor_out && state.monitor == MonitorMode::kAfl) {
    for (int ch = 0; ch < num_channels; ++ch) {
      if (!channels[ch] || !monitor_out[ch]) continue;
      for (int i = 0; i < num_samples; ++i) {
        monitor_out[ch][i] += channels[ch][i];
      }
    }
  }
}

bool MonitorRuntime::muted(size_t index) const noexcept {
  return valid_index(index) && strips_[index].muted;
}

bool MonitorRuntime::soloed(size_t index) const noexcept {
  return valid_index(index) && strips_[index].soloed;
}

bool MonitorRuntime::solo_safe(size_t index) const noexcept {
  return valid_index(index) && strips_[index].solo_safe;
}

bool MonitorRuntime::implied_mute(size_t index) const noexcept {
  return valid_index(index) && strips_[index].implied_mute;
}

MonitorMode MonitorRuntime::monitor_mode(size_t index) const noexcept {
  return valid_index(index) ? strips_[index].monitor : MonitorMode::kOff;
}

void MonitorRuntime::recompute_solo_mutes() noexcept {
  bool any_solo = false;
  for (size_t i = 0; i < size_; ++i) {
    any_solo = any_solo || strips_[i].soloed;
  }
  for (size_t i = 0; i < size_; ++i) {
    strips_[i].implied_mute = any_solo && !strips_[i].soloed && !strips_[i].solo_safe;
    update_target(strips_[i]);
  }
}

void MonitorRuntime::update_target(StripState& state) noexcept {
  const bool effectively_muted = state.muted || state.implied_mute;
  state.mute_gain.set_target(effectively_muted ? 0.0f : 1.0f);
}

}  // namespace sonare::engine
