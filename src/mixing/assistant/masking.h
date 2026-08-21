#pragma once

/// @file masking.h
/// @brief Pairwise band dominance for the mixing assistant.
///
/// @details **Offline / control thread only.** The pass is quadratic in track
///          count, allocates its result, and walks every analysis frame of
///          every track pair. Never call it from `process()`.
///
/// @details The measure is a **band energy ratio, not a loudness difference.**
///          For band `b` and frame `t`, with `E_i(b, t)` the masker's band
///          energy and `E_j(b, t)` the maskee's, the per-frame share is
///          `E_i / (E_i + E_j)`, and the reported figure is that share averaged
///          over the frames where both tracks clear the band's energy floor.
///          The energies are sums of STFT power over a band's bins, taken
///          straight from @ref BandEnergyEnvelope. No loudness model, no
///          auditory filterbank and no excitation pattern takes part, and the
///          figure is deliberately *not* the drop in one track's loudness
///          caused by the presence of the others.

#include <vector>

#include "mixing/assistant/mix_profile.h"
#include "mixing/assistant/track_profile.h"

namespace sonare::mixing::assistant {

/// @brief Measures how strongly each track dominates each other track, per band.
/// @details Both directions of a pair are filled independently; which of the
///          two is larger is what decides who gets carved. The diagonal is left
///          default-constructed, since a track does not mask itself, and so are
///          the whole row and column of a track whose
///          @ref TrackProfile::usable is false.
///
///          Frames where either track's band energy sits below that track's own
///          per-band floor are dropped rather than averaged in as zero, and the
///          surviving count is reported in @ref BandDominance::valid_frames. A
///          pair that never sounds together in a band therefore reads as
///          `ratio = 0, valid_frames = 0` — an absence of interference, not a
///          division by zero. Averaging happens per frame and only then over
///          time: two parts that share a band but never sound at the same
///          moment do not interfere, and a mean spectrum cannot tell the two
///          cases apart.
///
///          Tracks need not be the same length. A frame past a track's end
///          reads as silence and simply fails the floor test.
/// @param profiles Per-track profiles from @ref analyze_track_profiles.
/// @return A flat matrix of `profiles.size() * profiles.size() * kBandCount`
///         entries, indexed `(masker * n + maskee) * kBandCount + band`, which
///         is exactly what @ref MixProfile::dominance_at reads. Empty for an
///         empty input.
std::vector<BandDominance> analyze_band_dominance(const std::vector<TrackProfile>& profiles);

}  // namespace sonare::mixing::assistant
