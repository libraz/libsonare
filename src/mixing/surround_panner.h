#pragma once

/// @file surround_panner.h
/// @brief Constant-power surround panner for >2-channel destination buses.
///
/// The stereo @ref sonare::mixing::PannerProcessor is unchanged and remains the
/// path for stereo buses; the mixer selects this panner only when the
/// destination layout has more than two planes (5.1 / 7.1). The two are chosen
/// by dispatch, so "stereo pan-law behaviour is unchanged" holds by construction.

#include <array>
#include <atomic>

#include "core/channel_layout.h"
#include "rt/param_smoother.h"

namespace sonare::mixing {

/// Maximum number of output planes any phase-1 layout can have (7.1).
inline constexpr int kMaxSurroundPlanes = 8;

/// Surround pan parameters. Phase 1 honors azimuth, divergence and LFE;
/// `elevation`/`distance` are reserved (no height beds / focus yet).
struct SurroundPanParams {
  float azimuth = 0.0f;     ///< -180..180 deg, 0 = front-center, +right
  float elevation = 0.0f;   ///< reserved, 0 in phase 1 (no height beds)
  float divergence = 0.0f;  ///< 0 = point source, 1 = spread across the front
  float lfe = 0.0f;         ///< 0..1 scalar send into the LFE plane
  float distance = 1.0f;    ///< reserved (focus/spread), 1 in phase 1
};

/// Per-plane linear gain vector in canonical plane order. The non-LFE planes are
/// unit-power normalized; the LFE plane carries the raw `lfe` scalar (it bypasses
/// the loudness contribution intentionally).
struct SurroundPanGains {
  std::array<float, kMaxSurroundPlanes> gain{};
  int count = 0;  ///< active planes = channel_count(layout)
};

/// Canonical azimuth (degrees, 0 = front, +right) assigned to each speaker role
/// for the horizontal panning ring. LFE has no position.
constexpr float speaker_azimuth_deg(SpeakerRole role) noexcept {
  switch (role) {
    case SpeakerRole::C:
      return 0.0f;
    case SpeakerRole::L:
      return -30.0f;
    case SpeakerRole::R:
      return 30.0f;
    case SpeakerRole::Lss:
      return -90.0f;
    case SpeakerRole::Rss:
      return 90.0f;
    case SpeakerRole::Ls:
      return -110.0f;
    case SpeakerRole::Rs:
      return 110.0f;
    case SpeakerRole::LFE:
      return 0.0f;
  }
  return 0.0f;
}

/// Pure gain computation: pairwise constant-power panning between the two
/// adjacent speakers on the layout's horizontal ring selected by `azimuth`, then
/// a unit-power renormalized blend toward an equal-front-spread vector by
/// `divergence`, plus the raw `lfe` scalar in the LFE plane.
/// @throws SonareException(InvalidParameter) for non-surround layouts (the mixer
///         dispatches mono/stereo to the stereo panner).
SurroundPanGains compute_surround_pan_gains(const SurroundPanParams& params, ChannelLayout layout);

/// Realtime surround panner: smooths each output-plane gain with a one-pole and
/// scatters a (mono-summed) lane signal additively across the destination planes.
///
/// Both this processor and TrackMixerRuntime's inline lane scatter compute their
/// placement from the shared @ref compute_surround_pan_gains; they differ only in
/// per-plane gain smoothing (this uses a one-pole, the lane scatter a per-block
/// linear ramp). The live mixer path uses the lane scatter; this processor is a
/// standalone building block. The smoothing styles are intentionally not unified
/// (no audible difference, and they have separate call sites).
class SurroundPannerProcessor {
 public:
  explicit SurroundPannerProcessor(ChannelLayout layout = ChannelLayout::FivePointOne,
                                   SurroundPanParams params = {}, float smoothing_ms = 5.0f);

  void prepare(double sample_rate, int max_block_size);
  void reset();

  void set_params(const SurroundPanParams& params) noexcept;
  SurroundPanParams params() const noexcept;

  void set_layout(ChannelLayout layout) noexcept;
  ChannelLayout layout() const noexcept {
    return static_cast<ChannelLayout>(layout_.load(std::memory_order_relaxed));
  }

  /// Pans the input signal and adds it into `out` planes.
  /// @param in Input planes (mono = 1 plane, stereo = 2 planes summed to a point
  ///           source); only the first min(num_in_channels, 2) planes are read.
  /// @param num_in_channels Number of valid input planes (1 or 2).
  /// @param out Destination bus planes (accumulated into, not overwritten).
  /// @param num_out_planes Number of destination planes; must be >= the layout's
  ///        channel_count for the panned planes to land.
  /// @param num_samples Samples per plane.
  void process_add(const float* const* in, int num_in_channels, float* const* out,
                   int num_out_planes, int num_samples);

 private:
  void load_target_gains(SurroundPanGains& out) const;

  double sample_rate_ = 48000.0;
  float smoothing_ms_ = 5.0f;
  std::array<rt::ParamSmoother, kMaxSurroundPlanes> smoothers_{};
  std::atomic<uint8_t> layout_{static_cast<uint8_t>(ChannelLayout::FivePointOne)};
  std::atomic<float> azimuth_{0.0f};
  std::atomic<float> elevation_{0.0f};
  std::atomic<float> divergence_{0.0f};
  std::atomic<float> lfe_{0.0f};
  std::atomic<float> distance_{1.0f};
};

}  // namespace sonare::mixing
