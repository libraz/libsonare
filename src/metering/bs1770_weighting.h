#pragma once

namespace sonare::metering {

// ITU-R BS.1770-4 §2.4 — surround channel weighting for L_s / R_s when
// computing the weighted loudness sum. Applied to the mean-square energy (power
// domain, equivalent to +1.5 dB): 10^(1.5/10) = 1.4125375446227544.
// Spec-mandated bit-exact value; do not round.
inline constexpr double kBs1770SurroundWeight = 1.4125375446227544;

/// Returns the ITU-R BS.1770-4 per-channel weight for the weighted loudness sum.
///
/// Front channels (L/R/C) carry unity weight, surround channels (Ls/Rs) are
/// weighted +1.5 dB (kBs1770SurroundWeight), and the LFE channel is excluded
/// entirely (weight 0). BS.1770-4 normatively specifies layouts only up to 5.1;
/// the 7.1 rear-surround pair (Lb/Rb) weighting is a NON-NORMATIVE extrapolation
/// (rear surrounds treated the same +1.5 dB as the side surrounds), not part of
/// the standard. Layouts are keyed off the canonical SMPTE/WAV interleaving
/// order for each channel count (see core/channel_layout.h).
///
/// @param channel  Zero-based channel index within the interleaved layout.
/// @param channels Total channel count (selects the layout).
/// @return Linear (power-domain) weight applied to the channel's mean-square.
inline double bs1770_channel_weight(int channel, int channels) noexcept {
  switch (channels) {
    case 4:
      // Quad: L, R, Ls(2), Rs(3). No center, no LFE.
      if (channel == 2 || channel == 3) return kBs1770SurroundWeight;
      return 1.0;
    case 5:
      // 5.0: L, R, C, Ls(3), Rs(4).
      if (channel == 3 || channel == 4) return kBs1770SurroundWeight;
      return 1.0;
    case 6:
      // 5.1: L, R, C, LFE(3), Ls(4), Rs(5).
      if (channel == 3) return 0.0;  // LFE excluded.
      if (channel == 4 || channel == 5) return kBs1770SurroundWeight;
      return 1.0;
    case 8:
      // 7.1: L, R, C, LFE(3), Lss(4), Rss(5), Ls(6), Rs(7) (canonical order, see
      // core/channel_layout.h). NON-NORMATIVE: BS.1770-4 does not define a 7.1
      // weighting; the rear/back pair (Ls/Rs) is treated like the side surrounds
      // (Lss/Rss, +1.5 dB) as a reasonable extension rather than a standard value.
      if (channel == 3) return 0.0;                    // LFE excluded.
      if (channel >= 4) return kBs1770SurroundWeight;  // side + rear surround.
      return 1.0;
    default:
      // Mono, stereo, and unspecified layouts: treat every channel as unity.
      return 1.0;
  }
}

}  // namespace sonare::metering
