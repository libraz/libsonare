#pragma once

/// @file speaker_stage.h
/// @brief Speaker output stage (stage [7a]): per-speaker trim, distance
///        compensation and bass management on the output bus.
///
/// Distance delay = (max distance - distance) / speed of sound, as an integer
/// delay line plus a Lagrange-3 fractional part. Bass management splits small
/// speakers with an LR4 crossover; the low band plus LFE * `lfe_gain_db` feeds
/// the LFE plane (sub-feed rule), or, without a subwoofer, the low band plus
/// the LFE fold-down go to the large L/R pair.

#include <array>
#include <memory>

#include "core/channel_layout.h"
#include "playback/config.h"

namespace sonare::playback {

class SpeakerStage {
 public:
  SpeakerStage();
  ~SpeakerStage();
  SpeakerStage(const SpeakerStage&) = delete;
  SpeakerStage& operator=(const SpeakerStage&) = delete;

  /// Control thread. @p speakers and @p bass are the prepare keys; the
  /// crossover and fold-down filters are built at @p sample_rate.
  void prepare(double sample_rate, int max_block_size, ChannelLayout layout,
               const std::array<SpeakerPrepare, kSpeakerRoleCount>& speakers,
               const BassManagementConfig& bass);
  /// Realtime: per-role trims (indexed by SpeakerRole), LFE sub-feed gain and
  /// LFE fold-down level.
  void set_levels(const std::array<float, kSpeakerRoleCount>& trim_db, float lfe_gain_db,
                  float lfe_mix_db) noexcept;
  /// In place over `channel_count(layout)` planes.
  void process(float* const* planes, int frames) noexcept;
  void reset() noexcept;
  /// Largest distance delay rounded to the nearest frame.
  int latency_samples() const noexcept;
  /// Largest distance delay in Q8 frames.
  int latency_samples_q8() const noexcept;
  /// Distance delay of one plane in Q8 frames.
  int plane_delay_q8(int plane) const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sonare::playback
