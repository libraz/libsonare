#pragma once

/// @file renderer.h
/// @brief The playback renderer: decoded mono / stereo / 5.1 / 7.1 PCM to
///        headphones or to stereo / 5.1 / 7.1 speakers.
///
/// Front ends (stages [1] to [6], one per input layout, four under
/// `input.layout = "auto"`) write the output bus; the back end (speaker stage
/// or binaural stage, then the output limiter) reads it. Latency depends only
/// on the target, the sample rate and distance compensation: at 48 kHz without
/// distance compensation it is 288 frames for stereo speakers and 1312 for
/// every other target.
///
/// Threading: process_*, reset() and destruction run on one thread at a time.
/// set_config() runs on one control thread, concurrently with process_*; its
/// realtime values are adopted at the next block boundary. set_head_orientation()
/// runs on any single thread concurrently with process_*.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/channel_layout.h"
#include "playback/config.h"
#include "playback/hrtf_set.h"

namespace sonare::playback {

/// Output-limiter look-ahead.
inline constexpr float kOutputLimiterLookaheadMs = 1.0f;
/// Block length of the offline one-shot render.
inline constexpr int kOfflineRenderBlockFrames = 4096;

/// Snapshot of the renderer's diagnostics.
struct RendererDiagnostics {
  ChannelLayout active_input_layout = ChannelLayout::Stereo;
  uint64_t layout_switches = 0;
  uint64_t truncated_drains = 0;
  uint32_t inactive_stages = 0;  ///< bit `1u << Stage`
  std::array<int, kStageCount> stage_latency_q8{};
  int latency_samples = 0;
  float loudness_gain_db = 0.0f;
  bool loudness_gain_clamped = false;
  bool hrtf_ignored = false;  ///< an HRTF set was supplied to a speakers target
  /// Deepest last-block main/LFE limiter reduction (dB, <= 0).
  float limiter_gain_reduction_db = 0.0f;
  uint32_t non_finite_discards = 0;
};

/// Diagnostics document (`active_input_layout`, `layout_switches`,
/// `truncated_drains`, `inactive_stages`, `latency.stages`, clamps).
std::string diagnostics_to_json(const RendererDiagnostics& diagnostics);

class PlaybackRenderer {
 public:
  /// Control thread. @p hrtf is copied (resampled if needed) for a headphones
  /// target; null selects the built-in set on native builds.
  /// @throws SonareException(InvalidParameter) on an invalid configuration,
  ///         sample rate or block size, or a missing HRTF set on WASM.
  PlaybackRenderer(const RendererConfig& config, const HrtfSet* hrtf, int sample_rate,
                   int max_block_size);
  ~PlaybackRenderer();
  PlaybackRenderer(const PlaybackRenderer&) = delete;
  PlaybackRenderer& operator=(const PlaybackRenderer&) = delete;

  /// Control thread. Publishes the realtime half of a complete configuration.
  /// @throws SonareException(InvalidParameter) "requires a new renderer: <key>"
  ///         when a prepare key differs.
  void set_config(const RendererConfig& config);
  /// Control thread. The configuration last applied.
  RendererConfig config() const;
  void set_head_orientation(float yaw_deg, float pitch_deg, float roll_deg) noexcept;

  /// Renders one planar block. Returns false (and advances nothing) for a
  /// rejected shape: wrong channel counts, frames beyond the max block size,
  /// null buffers. frames == 0 returns true and never switches layouts.
  bool process_planar(const float* const* in, int in_channels, float* const* out, int out_channels,
                      int frames) noexcept;
  /// Interleaved variant; @p in and @p out must not alias.
  bool process_interleaved(const float* in, int in_channels, float* out, int out_channels,
                           int frames) noexcept;
  /// Clears all DSP state, including pending drains; keeps the configuration,
  /// the head pose and the active input layout.
  void reset() noexcept;

  int latency_samples() const noexcept;
  int input_channel_count() const noexcept;
  int output_channel_count() const noexcept;
  int sample_rate() const noexcept;
  int max_block_size() const noexcept;
  RendererDiagnostics diagnostics() const;
  uint32_t non_finite_discard_count() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Renders a whole interleaved buffer offline, latency removed: the result has
/// @p frames frames of `*out_channels` channels.
/// @throws SonareException(InvalidParameter) if @p frames is 0, @p in is null,
///         @p in_channels is not positive, or @p in contains a non-finite
///         sample.
std::vector<float> render_interleaved(const float* in, size_t frames, int in_channels,
                                      int sample_rate, const RendererConfig& config,
                                      const HrtfSet* hrtf, int* out_channels);

}  // namespace sonare::playback
