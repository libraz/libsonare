#pragma once

/// @file image_occupancy.h
/// @brief Stereo image occupancy and mono-fold risk for the mixing assistant.
///
/// @details **Offline / control thread only.** Measuring runs an STFT per
///          channel per track and allocates freely. Never call any of this from
///          `process()`.
///
/// @details Both measure the material *as the caller handed it over*. Neither
///          reads, proposes nor applies a pan value: the histogram describes
///          where the tracks already sit, which is the input a panning decision
///          is made from and not its result.
///
/// @details The two questions — where a track sits in the image, and what it
///          loses under a mono fold — are answered from one measurement.
///          @ref measure_track_channel_energy takes it once per track and each
///          entry point reads it, because the transform behind it is the
///          expensive part and running it twice over the same track measures
///          the same thing twice.

#include <array>
#include <vector>

#include "mixing/assistant/mix_profile.h"
#include "mixing/assistant/track_profile.h"

namespace sonare::mixing::assistant {

/// @brief One track's per-band linear power, split by channel and by mid/side.
/// @details @ref TrackProfile::bands is a single per-track envelope with no
///          channel split, so it cannot answer where in the image a band sits;
///          this is the split the image passes need and the profile does not
///          carry. It keeps seven numbers per plane and no time axis, which is
///          why it can be held for every track at once when the spectrogram it
///          came from cannot.
struct TrackChannelEnergy {
  std::array<double, kBandCount> left{};
  std::array<double, kBandCount> right{};
  std::array<double, kBandCount> mid{};
  std::array<double, kBandCount> side{};
  /// @brief False when the track could not be measured at all.
  bool valid = false;
  /// @brief True only when both channels were transformed.
  bool stereo = false;
};

/// @brief Measures every usable track's per-band channel energy, once.
/// @details A track with no usable profile, no buffer, no samples or a
///          non-positive sample rate comes back with @ref
///          TrackChannelEnergy::valid false rather than throwing, so the vector
///          always has one entry per track and indices line up with @p tracks.
///
///          The analysis geometry is the one each profile was measured with, so
///          a band energy computed here lines up with the one the profile
///          already caches.
/// @param tracks Tracks in the mix.
/// @param profiles Their profiles, in the same order. Entries past the shorter
///        of the two lists are ignored.
/// @return One entry per track, up to the shorter of the two lists.
std::vector<TrackChannelEnergy> measure_track_channel_energy(
    const std::vector<TrackInput>& tracks, const std::vector<TrackProfile>& profiles);

/// @brief Builds the band-by-pan-position energy histogram of the tracks as they
///        currently sit, before any panning decision has been made.
///
/// @details Each track is placed from its own channel energies, not from a pan
///          setting: a mono track is one source in the middle bucket, and a
///          stereo track is placed **per band** from that band's balance,
///          `pan = (E_R - E_L) / (E_R + E_L)`. A narrow low end under a wide top
///          therefore occupies several buckets, which is the shape real material
///          has.
///
/// @details Band edges are @ref kBands verbatim — this module never introduces a
///          second band grid. Pan buckets tile `[-1, 1]` uniformly, half-open so
///          a value cannot land in two, except the topmost, closed at `+1` so
///          hard right has somewhere to go. @ref kPanBucketCount is odd, so the
///          middle bucket contains `pan == 0` exactly.
///
/// @details @ref ImageOccupancy::crowding is `1 - H / log(kPanBucketCount)`, the
///          complement of normalized pan entropy, so it reads the whole
///          distribution rather than its peak and hits exactly 1 for a single
///          occupied bucket and exactly 0 for a uniform spread.
///
/// @details Tracks that are unusable, empty or unbuffered contribute nothing.
///
/// @param tracks Tracks in the mix.
/// @param profiles Their profiles, in the same order. Entries past the shorter
///        of the two lists are ignored.
/// @return A histogram of `kBandCount * kPanBucketCount` entries. Degenerate
///         input never throws: no tracks, or every track excluded, yields a
///         correctly sized all-zero histogram with no crowded band.
ImageOccupancy analyze_image_occupancy(const std::vector<TrackInput>& tracks,
                                       const std::vector<TrackProfile>& profiles);

/// @brief As above, from an energy measurement already taken.
/// @param energies Per-track energies from @ref measure_track_channel_energy.
ImageOccupancy analyze_image_occupancy(const std::vector<TrackChannelEnergy>& energies);

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

/// @brief As above, from an energy measurement already taken.
/// @details Only the low-end width verdict reads @p energies; the correlation
///          and width judgements are time-domain and are taken here either way.
/// @param tracks Tracks in the mix.
/// @param profiles Their profiles, in the same order.
/// @param energies Per-track energies from @ref measure_track_channel_energy,
///        in the same order. A track past the end of this list is measured as
///        if its low end could not be read, which is the same answer an
///        unmeasurable track gets.
std::vector<MonoRisk> analyze_mono_risks(const std::vector<TrackInput>& tracks,
                                         const std::vector<TrackProfile>& profiles,
                                         const std::vector<TrackChannelEnergy>& energies);

}  // namespace sonare::mixing::assistant
