#pragma once

/// @file surround_panner.h
/// @brief Constant-power surround panner for >2-channel destination buses.
///
/// The stereo @ref sonare::mixing::PannerProcessor is unchanged and remains the
/// path for stereo buses; the mixer selects this panner only when the
/// destination layout has more than two planes (5.1 / 7.1). The two are chosen
/// by dispatch, so "stereo pan-law behaviour is unchanged" holds by construction.

#include <algorithm>
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

/// @brief The parameters a surround writer will actually store for @p params.
/// @details Placement is computed from the clamped values, so every writer that
///          also caches what it was handed - ChannelStrip, the C ABI setter, the
///          scene walker - resolves through here and a read-back reports what
///          the panner is using. elevation and distance are reserved and pass
///          through unclamped, matching the render path.
inline SurroundPanParams clamp_surround_pan_params(const SurroundPanParams& params) noexcept {
  SurroundPanParams out = params;
  out.azimuth = std::clamp(params.azimuth, -180.0f, 180.0f);
  out.divergence = std::clamp(params.divergence, 0.0f, 1.0f);
  out.lfe = std::clamp(params.lfe, 0.0f, 1.0f);
  return out;
}

/// Per-plane linear gain vector in canonical plane order. The non-LFE planes are
/// unit-power normalized; the LFE plane carries the raw `lfe` scalar (it bypasses
/// the loudness contribution intentionally).
struct SurroundPanGains {
  std::array<float, kMaxSurroundPlanes> gain{};
  int count = 0;  ///< active planes = channel_count(layout)
};

/// Role azimuth (degrees, 0 = front, +right) shared by the 5.1 and 7.1 rings;
/// 7.1 overrides Ls/Rs below. LFE has no position.
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

/// Azimuth of @p role on @p layout's horizontal ring; 7.1 places Ls/Rs at the rear (+/-135).
constexpr float speaker_azimuth_deg(SpeakerRole role, ChannelLayout layout) noexcept {
  if (layout == ChannelLayout::SevenPointOne) {
    switch (role) {
      case SpeakerRole::Ls:
        return -135.0f;
      case SpeakerRole::Rs:
        return 135.0f;
      default:
        break;
    }
  }
  return speaker_azimuth_deg(role);
}

/// Pure gain computation: pairwise constant-power panning between the two
/// adjacent speakers on the layout's horizontal ring selected by `azimuth`, then
/// a unit-power renormalized blend toward an equal-front-spread vector by
/// `divergence`, plus the raw `lfe` scalar in the LFE plane.
/// @throws SonareException(InvalidParameter) for non-surround layouts (the mixer
///         dispatches mono/stereo to the stereo panner).
SurroundPanGains compute_surround_pan_gains(const SurroundPanParams& params, ChannelLayout layout);
/// No-throw realtime variant. Returns false for a non-surround/unsupported layout.
bool try_compute_surround_pan_gains(const SurroundPanParams& params, ChannelLayout layout,
                                    SurroundPanGains* out) noexcept;

/// @brief Glides a surround placement by smoothing its parameters, not its gains.
/// @details azimuth, divergence and lfe each follow a one-pole and the gains are
///          evaluated from the smoothed values every sample, so every instant of a
///          move is a placement the law describes (unit non-LFE power included) and
///          the result does not depend on block partitioning. Azimuth takes the
///          shorter arc; an exactly opposite move passes behind the listener.
class SurroundPanGlide {
 public:
  void prepare(double sample_rate, float time_ms) noexcept;
  /// Opens at @p params on @p layout with no glide.
  void snap(const SurroundPanParams& params, ChannelLayout layout) noexcept;
  /// Glides toward @p params on the layout the last snap() named.
  void set_target(const SurroundPanParams& params) noexcept;
  /// Jumps to the current target.
  void settle() noexcept;
  /// Advances one sample and returns its gains (all zero on a non-surround layout).
  const SurroundPanGains& next() noexcept;

 private:
  void evaluate(float azimuth, float divergence, float lfe) noexcept;

  ChannelLayout layout_ = ChannelLayout::FivePointOne;
  rt::ParamSmoother azimuth_;  // unwrapped; wrapped when evaluated
  rt::ParamSmoother divergence_;
  rt::ParamSmoother lfe_;
  float elevation_ = 0.0f;
  float distance_ = 1.0f;
  // The smoothed values gains_ was evaluated from; a settled glide reuses it.
  std::array<float, 3> evaluated_{};
  bool evaluated_valid_ = false;
  SurroundPanGains gains_{};
};

/// Realtime surround panner: glides the placement with a @ref SurroundPanGlide and
/// scatters a (mono-summed) lane signal additively across the destination planes.
///
/// Numerically the same scatter as TrackMixerRuntime's lane path: a 0.5(L+R) point
/// source, a 5 ms parameter glide, and a snap to the target on the first block after
/// prepare()/reset() and on a layout change. The standalone mixer graph scatters
/// strips with it.
class SurroundPannerProcessor {
 public:
  explicit SurroundPannerProcessor(ChannelLayout layout = ChannelLayout::FivePointOne,
                                   SurroundPanParams params = {}, float smoothing_ms = 5.0f);

  void prepare(double sample_rate, int max_block_size);
  /// Makes the next process_add() open at its target gains rather than glide.
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
  double sample_rate_ = 48000.0;
  float smoothing_ms_ = 5.0f;
  SurroundPanGlide glide_;
  // The layout the glide was last snapped on, or kUnprimed after prepare()/reset().
  // A placement on one layout is not a position on another, so a layout change
  // snaps instead of gliding. Audio thread only.
  static constexpr uint8_t kUnprimed = 0xFF;
  uint8_t rendered_layout_{kUnprimed};
  std::atomic<uint8_t> layout_{static_cast<uint8_t>(ChannelLayout::FivePointOne)};
  std::atomic<float> azimuth_{0.0f};
  std::atomic<float> elevation_{0.0f};
  std::atomic<float> divergence_{0.0f};
  std::atomic<float> lfe_{0.0f};
  std::atomic<float> distance_{1.0f};
};

}  // namespace sonare::mixing
