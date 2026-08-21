#pragma once

/// @file image_occupancy.h
/// @brief Stereo image occupancy and mono-fold risk for the mixing assistant.
///
/// @details **Offline / control thread only.** Both entry points run an STFT per
///          channel per track and allocate freely. Never call either of them
///          from `process()`.
///
/// @details Both measure the material *as the caller handed it over*. Neither
///          reads, proposes nor applies a pan value: the histogram describes
///          where the tracks already sit, which is the input a panning decision
///          is made from and not its result.

#include <vector>

#include "mixing/assistant/mix_profile.h"
#include "mixing/assistant/track_profile.h"

namespace sonare::mixing::assistant {

/// @brief Builds the band-by-pan-position energy histogram of the tracks as they
///        currently sit, before any panning decision has been made.
///
/// @details Each track is placed from its own channel energies rather than from
///          a pan setting:
///          - A mono track (@ref TrackInput::right null) is one source at
///            centre, so its whole band energy lands in the middle bucket.
///          - A stereo track is placed **per band** from that band's L/R energy
///            balance, `pan = (E_R - E_L) / (E_R + E_L)` in `[-1, 1]`. A track
///            with a narrow low end and a wide top end therefore occupies
///            several buckets, which is the shape real material has.
///
/// @details Band edges are @ref kBands verbatim; this module never introduces a
///          second band grid. Pan buckets tile `[-1, 1]` at the uniform width
///          `2 / kPanBucketCount`, each half-open `[low, high)` so a value can
///          never land in two, except the topmost, which is closed at `+1` so
///          hard right has somewhere to go. @ref kPanBucketCount is odd, so the
///          middle bucket is centred on zero and contains `pan == 0` exactly.
///
/// @details @ref ImageOccupancy::crowding is the complement of the band's
///          normalized pan entropy, `1 - H / log(kPanBucketCount)`. It reads the
///          whole distribution rather than only its peak, so energy moving off
///          the peak into neighbouring buckets registers, and it reaches the
///          documented endpoints by construction: exactly 1 for a single
///          occupied bucket and exactly 0 for a uniform spread.
///
/// @details Tracks with @ref TrackProfile::usable false contribute nothing, as
///          do tracks with no samples or no buffer.
///
/// @param tracks Tracks in the mix.
/// @param profiles Their profiles, in the same order. Entries past the shorter
///        of the two lists are ignored.
/// @return A histogram of `kBandCount * kPanBucketCount` entries. Degenerate
///         input never throws: no tracks, or every track excluded, yields a
///         correctly sized all-zero histogram with no crowded band.
ImageOccupancy analyze_image_occupancy(const std::vector<TrackInput>& tracks,
                                       const std::vector<TrackProfile>& profiles);

/// @brief Finds tracks whose stereo image is at risk of collapsing under a mono
///        fold.
///
/// @details The correlation and width judgements come from
///          @ref mastering::stereo::mono_compat_check; nothing about them is
///          recomputed here. A track is reported when its correlation falls
///          below the assistant's threshold, when its width rises above it, or
///          when its low end alone is wide.
///
/// @details The low end is judged separately because it is the complaint that
///          comes up most in practice: it is measured over the sub and low
///          @ref kBands only, and is reported through
///          @ref MonoRisk::wide_low_end.
///
/// @details Mono tracks are never reported. A track with a single channel has no
///          stereo image for the fold to take away. Tracks with
///          @ref TrackProfile::usable false are skipped, as are tracks with no
///          samples or no buffer.
///
/// @note A track with one silent channel (a hard-panned source) reads as
///       correlation 0 and is reported. That is deliberate: its level relative
///       to the rest of the mix moves on fold-down even though nothing cancels.
///
/// @param tracks Tracks in the mix.
/// @param profiles Their profiles, in the same order. Entries past the shorter
///        of the two lists are ignored.
/// @return One entry per at-risk track, in input order. Empty when nothing is at
///         risk. @ref MonoRisk::width is clamped to a finite sentinel so an
///         anti-phase pair stays serializable.
std::vector<MonoRisk> analyze_mono_risks(const std::vector<TrackInput>& tracks,
                                         const std::vector<TrackProfile>& profiles);

}  // namespace sonare::mixing::assistant
