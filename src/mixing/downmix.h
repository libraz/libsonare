#pragma once

/// @file downmix.h
/// @brief Deterministic surround -> narrower-layout downmix (ITU-R BS.775).
///
/// Pure, stateless coefficient mixing. The inverse direction (upmix to a wider
/// bed) is the panner's job (see surround design §3/§6), not a matrix — this
/// header only narrows.

#include <cstddef>

#include "core/channel_layout.h"

namespace sonare::mixing {

/// BS.775 downmix coefficients, exposed as named constants for the unit tests.
namespace downmix_coeff {
/// -3 dB attenuation (1/sqrt(2)) applied to the center and surround feeds when
/// folding them into the front L/R pair.
inline constexpr float kMinus3dB = 0.70710678118654752f;
}  // namespace downmix_coeff

/// Downmix options. Defaults match the BS.775 stereo deliverable: LFE dropped,
/// no normalization (the caller is responsible for headroom, as in the engine's
/// existing stereo path).
struct DownmixOptions {
  /// Fold the LFE plane into L/R at -3 dB instead of dropping it (BS.775 omits
  /// LFE by default).
  bool include_lfe = false;
  /// Scale the output so the worst-case (all inputs at full scale, in phase)
  /// sum cannot exceed unity. Off by default to keep level unchanged.
  bool normalize = false;
};

/// Downmix planar `in` (`channel_count(from)` planes) into planar `out`
/// (`channel_count(to)` planes), `n` frames each.
///
/// Supported conversions narrow the bed: 7.1->5.1, 5.1/7.1->stereo,
/// stereo/5.1/7.1->mono, plus the identity copy. Upmix or any other pair throws
/// `SonareException(InvalidParameter)`.
///
/// @param from   Source layout (describes the `in` planes).
/// @param to     Destination layout (describes the `out` planes).
/// @param in     `channel_count(from)` input plane pointers, each `n` samples.
/// @param out    `channel_count(to)` output plane pointers, each `n` samples.
/// @param n      Frame count per plane.
/// @param options Downmix behavior (LFE, normalization).
void downmix(ChannelLayout from, ChannelLayout to, const float* const* in, float* const* out,
             size_t n, const DownmixOptions& options = {});

}  // namespace sonare::mixing
