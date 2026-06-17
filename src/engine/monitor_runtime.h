#pragma once

/// @file monitor_runtime.h
/// @brief Engine-side solo/mute and PFL/AFL monitoring for ChannelStrip tracks.

#include <array>
#include <atomic>
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

  /// Snaps every active strip's mute-gain smoother (and the strip's own gain
  /// stages) to their steady-state targets so the next render block opens
  /// without a ramp-in, keeping an offline bounce deterministic.
  void settle() noexcept;

  size_t size() const noexcept { return size_.load(std::memory_order_acquire); }
  bool muted(size_t index) const noexcept;
  bool soloed(size_t index) const noexcept;
  bool solo_safe(size_t index) const noexcept;
  bool implied_mute(size_t index) const noexcept;
  MonitorMode monitor_mode(size_t index) const noexcept;

 private:
  // Solo/mute flags and monitor mode are written by the control thread
  // (set_mute / set_solo / ... / recompute_solo_mutes) and read by the audio
  // thread in process_strip(), so they are atomic to avoid torn reads. Writes
  // use release ordering and reads use acquire ordering. The atomics make the
  // struct non-copyable, so add_strip / remove_strip move the plain fields and
  // copy the atomics through load/store via the helpers below.
  struct StripState {
    mixing::ChannelStrip* strip = nullptr;
    rt::ParamSmoother mute_gain;
    std::atomic<bool> muted{false};
    std::atomic<bool> soloed{false};
    std::atomic<bool> solo_safe{false};
    std::atomic<bool> implied_mute{false};
    std::atomic<MonitorMode> monitor{MonitorMode::kOff};

    StripState() = default;

    // Snapshots all atomic flags with relaxed ordering; used only on the
    // control thread when compacting the array (remove_strip) or clearing a
    // slot (add_strip / remove_strip), never concurrently with the audio
    // thread's acquire reads.
    StripState& operator=(const StripState& other) noexcept {
      strip = other.strip;
      mute_gain = other.mute_gain;
      muted.store(other.muted.load(std::memory_order_relaxed), std::memory_order_relaxed);
      soloed.store(other.soloed.load(std::memory_order_relaxed), std::memory_order_relaxed);
      solo_safe.store(other.solo_safe.load(std::memory_order_relaxed), std::memory_order_relaxed);
      implied_mute.store(other.implied_mute.load(std::memory_order_relaxed),
                         std::memory_order_relaxed);
      monitor.store(other.monitor.load(std::memory_order_relaxed), std::memory_order_relaxed);
      return *this;
    }
  };

  void recompute_solo_mutes() noexcept;
  void update_target(StripState& state) noexcept;
  bool valid_index(size_t index) const noexcept {
    return index < size_.load(std::memory_order_acquire);
  }

  std::array<StripState, kMaxStrips> strips_{};
  // Number of active strips. Written only by the control thread (add_strip /
  // remove_strip) and read by the audio thread via size() / valid_index() in
  // process_strip(). The control thread publishes a newly written slot with a
  // release store and the audio thread observes it with an acquire load, so a
  // process_strip() that sees the new count is guaranteed to see the fully
  // initialized slot. Consistent with the StripState flag atomics above.
  std::atomic<size_t> size_{0};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  float smoothing_ms_ = 5.0f;
};

}  // namespace sonare::engine
