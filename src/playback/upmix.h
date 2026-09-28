#pragma once

/// @file upmix.h
/// @brief Stereo to 5.1 / 7.1 upmix (stage [3]): coherence-based direct /
///        ambience split, anti-phase direct energy to the surrounds, a
///        panning-index centre window applied to the L/R bins, and a fixed
///        all-pass decorrelator on the rear ambience.
///
/// STFT with a periodic Hann window, N = 2^round(log2(21.3 ms * fs)), hop N/4,
/// WOLA-normalized. Input and output FIFOs decouple it from the host block
/// length, so the latency is always exactly N frames, and the bypass path (L/R
/// only, delayed by N) keeps that latency when the stage is switched off.

#include <memory>

#include "core/channel_layout.h"

namespace sonare::playback {

/// STFT frame length in seconds that sets N.
inline constexpr float kUpmixFrameSeconds = 0.0213f;

/// Upmix latency in frames at @p sample_rate: the STFT length N (1024 at 44.1
/// and 48 kHz, 2048 at 96 kHz). The single source for both the bypass delay and
/// the reported latency.
int upmix_latency_frames(double sample_rate) noexcept;

/// Realtime parameters (`upmix.*`).
struct UpmixParams {
  bool enabled = true;
  float center_width = 0.2f;
  float front_ambience = 0.5f;
  bool lfe_from_upmix = false;
};

/// Stereo in, 5.1 or 7.1 bed out.
class Upmixer {
 public:
  Upmixer();
  ~Upmixer();
  Upmixer(const Upmixer&) = delete;
  Upmixer& operator=(const Upmixer&) = delete;

  /// Control thread. @p output is FivePointOne or SevenPointOne. Converts the
  /// millisecond delays, the 100 ms statistics time constant and the 120 Hz
  /// LFE filter to @p sample_rate.
  void prepare(double sample_rate, int max_block_size, ChannelLayout output);
  /// Realtime. An on/off change cross-fades over 20 ms unless @p immediate.
  void set_params(const UpmixParams& params, bool immediate) noexcept;
  /// Writes `channel_count(output)` planes (overwrites).
  void process(const float* left, const float* right, float* const* out, int frames) noexcept;
  void reset() noexcept;
  /// Always `upmix_latency_frames(sample_rate)`.
  int latency_samples() const noexcept;
  /// Frames for the ambience all-pass chain, its delay and the LFE filter to
  /// decay to -60 dB once the input is silent (excluding the latency).
  int decay_frames() const noexcept;
  ChannelLayout output_layout() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sonare::playback
