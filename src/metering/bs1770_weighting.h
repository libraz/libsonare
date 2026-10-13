#pragma once

#include "core/channel_layout.h"

namespace sonare::metering {

// ITU-R BS.1770-4 Table 3 position-dependent channel weight G_i = 1.41, applied
// to the mean-square energy (power domain, equivalent to +1.5 dB):
// 10^(1.5/10) = 1.4125375446227544. Spec-mandated bit-exact value; do not round.
inline constexpr double kBs1770SurroundWeight = 1.4125375446227544;

/// @brief ITU-R BS.1770-4 Table 3 weight for an ear-level loudspeaker at @p azimuth_deg.
/// @details 1.41 for 60 <= |azimuth| <= 120 degrees, 1.0 elsewhere. The table
///          also requires an elevation below 30 degrees, which every bed this
///          library models satisfies (none has a height layer).
constexpr double bs1770_position_weight(double azimuth_deg) noexcept {
  const double magnitude = azimuth_deg < 0.0 ? -azimuth_deg : azimuth_deg;
  return magnitude >= 60.0 && magnitude <= 120.0 ? kBs1770SurroundWeight : 1.0;
}

/// Returns the ITU-R BS.1770-4 per-channel weight for the weighted loudness sum.
///
/// The weight follows the loudspeaker's position (bs1770_position_weight), and
/// the LFE channel is excluded entirely (weight 0). Layouts are keyed off the
/// canonical WAVE_FORMAT_EXTENSIBLE order for each channel count, with the
/// azimuths of core/channel_layout.h: 5.0/5.1 surrounds at +/-110 degrees weigh
/// 1.41; 7.1 side surrounds (Lss/Rss, +/-90) weigh 1.41 and its back surrounds
/// (Ls/Rs, +/-135) 1.0; quad back surrounds (+/-135) weigh 1.0.
///
/// @param channel  Zero-based channel index within the interleaved layout.
/// @param channels Total channel count (selects the layout).
/// @return Linear (power-domain) weight applied to the channel's mean-square.
inline double bs1770_channel_weight(int channel, int channels) noexcept {
  switch (channels) {
    case 4: {
      // Quad: L, R, Ls, Rs at +/-45 and +/-135 degrees. No center, no LFE.
      constexpr double kQuadAzimuthDeg[] = {-45.0, 45.0, -135.0, 135.0};
      return bs1770_position_weight(kQuadAzimuthDeg[channel]);
    }
    case 5: {
      // 5.0: the 5.1 bed without its LFE plane.
      constexpr SpeakerRole kFiveZero[] = {SpeakerRole::L, SpeakerRole::R, SpeakerRole::C,
                                           SpeakerRole::Ls, SpeakerRole::Rs};
      return bs1770_position_weight(
          speaker_azimuth_deg(kFiveZero[channel], ChannelLayout::FivePointOne));
    }
    case 6:
    case 8: {
      const ChannelLayout layout = layout_from_channel_count(channels);
      const SpeakerRole role = speaker_roles(layout)[channel];
      if (role == SpeakerRole::LFE) return 0.0;
      return bs1770_position_weight(speaker_azimuth_deg(role, layout));
    }
    default:
      // Mono, stereo, and unspecified layouts: treat every channel as unity.
      return 1.0;
  }
}

}  // namespace sonare::metering
