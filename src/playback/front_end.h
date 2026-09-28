#pragma once

/// @file front_end.h
/// @brief The source-dependent half of the renderer, stages [1] to [6]:
///        reorder, dialogue level, upmix, loudness alignment, night-mode DRC
///        and layout conversion onto the output bus.
///
/// Every front end of one renderer reports the same latency: the upmix length
/// N whenever the bus is not stereo speakers (a front end without an upmix
/// delays by N instead) plus the DRC look-ahead. On an input-layout switch the
/// old front end is fed silence for `drain_frames()` and its output is summed
/// with the new one on the bus, so nothing is dropped or shifted in time.
/// prepare() allocates; nothing else does.

#include <array>
#include <cstdint>
#include <memory>

#include "core/channel_layout.h"
#include "playback/config.h"
#include "playback/layout_convert.h"
#include "playback/night_mode_drc.h"

namespace sonare::playback {

/// Loudness-alignment gain limits (`target - program`).
inline constexpr float kLoudnessMaxGainDb = 12.0f;
inline constexpr float kLoudnessMinGainDb = -40.0f;
/// Fade applied to a drain cut short by a second switch.
inline constexpr float kTruncatedDrainFadeMs = 2.0f;

/// Static loudness-alignment gain after clamping.
struct LoudnessGain {
  float gain_db = 0.0f;
  bool clamped = false;
};

/// `target - program` clamped to [-40, +12] dB; 0 when program is null.
LoudnessGain compute_loudness_gain(const RealtimeConfig& config) noexcept;

class FrontEnd {
 public:
  FrontEnd();
  ~FrontEnd();
  FrontEnd(const FrontEnd&) = delete;
  FrontEnd& operator=(const FrontEnd&) = delete;

  /// Control thread. @p input is this front end's fixed source layout; the
  /// channel map, if any, is taken from @p config.
  void prepare(double sample_rate, int max_block_size, ChannelLayout input,
               const PrepareConfig& config, const OutputBus& bus);
  /// Realtime values for the next block. @p immediate skips the cross-fades
  /// (a dormant front end, which is not producing output).
  void apply_realtime(const RealtimeConfig& config, bool immediate) noexcept;
  /// Adds this block's output to the bus planes. @p in holds
  /// `input_channel_count()` planes in input order, or is null for silence.
  void process(const float* const* in, float* const* bus, int frames) noexcept;
  /// Clears all state without allocating.
  void reset() noexcept;

  /// Latency in frames; equal across the front ends of one renderer.
  int latency() const noexcept;
  /// Frames of silent input after which the output has decayed below -60 dB:
  /// latency, plus N when an upmix is present, plus the slowest stateful
  /// stage's decay.
  int drain_frames() const noexcept;
  ChannelLayout input_layout() const noexcept;
  int input_channel_count() const noexcept;

  /// Draining: DRC gain held, detection off; the STFT is skipped once its
  /// FIFOs hold only silence.
  void set_draining(bool draining) noexcept;
  /// Linear fade to silence over @p frames of output (a truncated drain).
  void start_fade_out(int frames) noexcept;
  /// True once a started fade-out has reached silence.
  bool fade_out_done() const noexcept;

  NightModeDrcHandover handover() const noexcept;
  void accept_handover(const NightModeDrcHandover& state) noexcept;

  /// Bit `1u << Stage` set for each front-end stage that does nothing under
  /// @p config for this input layout.
  uint32_t inactive_stages(const RealtimeConfig& config) const noexcept;
  /// Front-end stage latencies in Q8 frames, indexed by Stage (other entries 0).
  std::array<int, kStageCount> stage_latency_q8() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sonare::playback
