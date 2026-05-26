#pragma once

/// @file monitor_runtime.h
/// @brief Engine-side solo/mute and PFL/AFL monitoring for ChannelStrip tracks.

#include <array>
#include <cstddef>
#include <cstdint>

#include "mixing/channel_strip.h"
#include "rt/param_smoother.h"

namespace sonare::engine {

enum class MonitorMode {
  kOff,
  kPfl,
  kAfl,
};

class MonitorRuntime {
 public:
  static constexpr size_t kMaxStrips = 16;

  void prepare(double sample_rate, int max_block_size, float smoothing_ms = 5.0f) noexcept;
  bool add_strip(mixing::ChannelStrip* strip) noexcept;
  bool remove_strip(mixing::ChannelStrip* strip) noexcept;
  bool contains(mixing::ChannelStrip* strip) const noexcept;

  void set_mute(size_t index, bool muted) noexcept;
  void set_solo(size_t index, bool soloed) noexcept;
  void set_exclusive_solo(size_t index, bool soloed) noexcept;
  void set_solo_safe(size_t index, bool solo_safe) noexcept;
  void set_monitor_mode(size_t index, MonitorMode mode) noexcept;

  void process_strip(size_t index, float* const* channels, int num_channels, int num_samples,
                     int64_t timeline_sample, float* const* monitor_out = nullptr) noexcept;

  size_t size() const noexcept { return size_; }
  bool muted(size_t index) const noexcept;
  bool soloed(size_t index) const noexcept;
  bool solo_safe(size_t index) const noexcept;
  bool implied_mute(size_t index) const noexcept;
  MonitorMode monitor_mode(size_t index) const noexcept;

 private:
  struct StripState {
    mixing::ChannelStrip* strip = nullptr;
    rt::ParamSmoother mute_gain;
    bool muted = false;
    bool soloed = false;
    bool solo_safe = false;
    bool implied_mute = false;
    MonitorMode monitor = MonitorMode::kOff;
  };

  void recompute_solo_mutes() noexcept;
  void update_target(StripState& state) noexcept;
  bool valid_index(size_t index) const noexcept { return index < size_; }

  std::array<StripState, kMaxStrips> strips_{};
  size_t size_ = 0;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  float smoothing_ms_ = 5.0f;
};

}  // namespace sonare::engine
