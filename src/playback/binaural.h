#pragma once

/// @file binaural.h
/// @brief Headphone output stage (stage [7b]): per-slot HRIR convolution with
///        head tracking, an 8-direction room-fixed reflection ring rendered
///        through the same HRIR path, and a two-ear late reverberation.
///
/// Each virtual speaker is convolved with the HRIR interpolated at its
/// head-relative direction; the ITD is applied to the far ear only, as a
/// fractional delay, so the near ear has zero latency. When a slot's filter
/// changes between blocks, the old and new filter outputs cross-fade linearly
/// over the block. The direct-ear bus planes (the LFE fold-down) bypass the
/// HRIR, reflection and reverberation paths.

#include <memory>

#include "playback/config.h"
#include "playback/hrtf_set.h"
#include "playback/speaker_geometry.h"

namespace sonare::playback {

/// Listener head orientation in degrees. Right-handed: positive yaw turns
/// right, positive pitch looks up, positive roll lowers the right ear. Applied
/// intrinsically in yaw, pitch, roll order.
struct HeadPose {
  float yaw_deg = 0.0f;
  float pitch_deg = 0.0f;
  float roll_deg = 0.0f;
};

/// Head-relative direction of a room-fixed direction under @p pose.
SpeakerDirection head_relative_direction(SpeakerDirection world, const HeadPose& pose) noexcept;

/// Realtime parameters (`room.*`, `head_tracking.enabled`).
struct BinauralParams {
  bool room_enabled = true;
  float room_mix_db = -6.0f;
  bool head_tracking_enabled = true;
};

class BinauralRenderer {
 public:
  BinauralRenderer();
  ~BinauralRenderer();
  BinauralRenderer(const BinauralRenderer&) = delete;
  BinauralRenderer& operator=(const BinauralRenderer&) = delete;

  /// Control thread. Resamples @p hrtf to @p sample_rate when the rates
  /// differ, builds the reflection ring from the image sources of @p room and
  /// synthesizes the late-reverberation IRs. @p slots is the bus slot set.
  void prepare(double sample_rate, int max_block_size, const HrtfSet& hrtf,
               const HeadphoneSlotSet& slots, RoomPreset room);
  /// Realtime.
  void set_params(const BinauralParams& params) noexcept;
  /// Realtime; adopted by the next process() call.
  void set_head_pose(const HeadPose& pose) noexcept;
  /// @p bus holds `slots.count` slot planes then the direct left and right
  /// planes. Writes both ears (overwrites).
  void process(const float* const* bus, float* left, float* right, int frames) noexcept;
  void reset() noexcept;
  /// Near-ear latency (0).
  int latency_samples() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sonare::playback
