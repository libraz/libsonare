#pragma once

/// @file night_mode_drc.h
/// @brief Night-mode dynamic range control (stage [5]): BS.1770-weighted
///        detection over all source planes, a static two-sided curve relative
///        to the target loudness, and one gain applied to every plane.
///
/// Detection: per-plane K-weighting, channel-weighted power sum, 50 ms moving
/// average, dB. Curve (x = detected - target, y = output - target): above +3 dB
/// compressed at 1 + 3*amount : 1; between -12 and -40 dB lifted upward at
/// 1 + amount : 1 (at most 12*amount dB), fading to no lift at -40 dB; 1:1 in
/// between; 6 dB soft knees. The signal path is delayed 5 ms (look-ahead) in
/// every state, including amount = 0, where the curve is not evaluated.

#include <memory>

#include "core/channel_layout.h"

namespace sonare::playback {

inline constexpr float kNightModeLookaheadMs = 5.0f;
inline constexpr float kNightModeAttackMs = 10.0f;
inline constexpr float kNightModeReleaseMs = 300.0f;
inline constexpr float kNightModeDetectorWindowMs = 50.0f;

/// Detector and gain state carried from one front end to the next on an
/// input-layout switch.
struct NightModeDrcHandover {
  double mean_power = 0.0;  ///< detector moving-average power (linear)
  float gain_db = 0.0f;     ///< smoothed gain
};

/// Static curve output level relative to target for input @p x_db relative to
/// target (amount = 1: +15 -> +6.0, -25 -> -18.5).
float night_mode_curve_db(float x_db, float amount) noexcept;

class NightModeDrc {
 public:
  NightModeDrc();
  ~NightModeDrc();
  NightModeDrc(const NightModeDrc&) = delete;
  NightModeDrc& operator=(const NightModeDrc&) = delete;

  /// Control thread. @p layout selects the BS.1770 channel weights.
  void prepare(double sample_rate, int max_block_size, ChannelLayout layout);
  /// Realtime: `night_mode.amount` in [0, 1].
  void set_amount(float amount) noexcept;
  /// Realtime: `loudness.target_lufs`.
  void set_target_lufs(float target_lufs) noexcept;
  /// Holds the current gain and skips detection (a draining front end).
  void set_frozen(bool frozen) noexcept;
  /// In place over `channel_count(layout)` planes.
  void process(float* const* planes, int frames) noexcept;
  void reset() noexcept;
  /// Look-ahead in frames (5 ms rounded).
  int latency_samples() const noexcept;
  NightModeDrcHandover handover() const noexcept;
  /// Fills the detector window with `mean_power` and sets the smoothed gain.
  void accept_handover(const NightModeDrcHandover& state) noexcept;
  /// Gain currently applied, in dB.
  float current_gain_db() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sonare::playback
