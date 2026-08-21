#pragma once

/// @file mix_profile.h
/// @brief Cross-track measurements for the mixing assistant.
///
/// @details **Offline / control thread only.** The pairwise passes are
///          quadratic in track count and allocate freely. Never call any of
///          this from `process()`.
///
/// @details @ref MixProfile holds only what cannot be measured from one track
///          in isolation. Anything single-track lives in @ref TrackProfile.

#include <cstddef>
#include <string>
#include <vector>

#include "mixing/assistant/track_profile.h"

namespace sonare::mixing::assistant {

/// @brief How strongly one track dominates another within a band.
/// @details **This is an energy ratio, not a loudness difference.** The value is
///          `E_masker / (E_masker + E_maskee)` averaged over the frames where
///          both tracks are actually sounding: 0.5 means the two carry equal
///          band energy, 1 means the masker owns the band outright. No loudness
///          model, no auditory filterbank and no excitation pattern is involved
///          anywhere in its derivation, and it is deliberately *not* the drop in
///          a track's loudness caused by the presence of the others.
///
///          The measure is asymmetric on purpose: "A dominates B" and "B
///          dominates A" are separate entries, and which of the two is larger is
///          exactly what decides who gets carved.
struct BandDominance {
  /// @brief Mean energy share in `[0, 1]`, or 0 when @ref valid_frames is 0.
  float ratio = 0.0f;
  /// @brief Frames where both tracks exceeded the band energy floor.
  /// @details Frames where either track is silent are excluded rather than
  ///          averaged in as zero: the ratio is meaningless there and would only
  ///          drag the mean down. Zero valid frames means "these two never
  ///          overlap in this band", which is an absence of interference, not a
  ///          division by zero.
  int valid_frames = 0;
};

/// @brief Time and polarity relationship between two tracks.
struct PairAlignment {
  /// @brief Index of the reference track. Always the smaller of the two.
  int reference_index = 0;
  /// @brief Index of the track measured against the reference.
  int target_index = 0;
  /// @brief Lag in samples that best aligns the target onto the reference.
  /// @details Positive means the target arrives *later* than the reference and
  ///          the reference needs delaying by this many samples to match it.
  ///          The reference is always the lower index, so the sign has one
  ///          meaning across the whole matrix.
  int lag_samples = 0;
  /// @brief Signed normalized correlation at @ref lag_samples, in `[-1, 1]`.
  float correlation = 0.0f;
  /// @brief True when the best `|r|` peak is negative, i.e. the pair is
  ///        polarity-opposed.
  bool polarity_opposed = false;
  /// @brief False when the pair is too weakly correlated to be worth touching.
  /// @details Aligning two unrelated tracks does nothing useful and can do harm.
  bool related = false;
};

/// @brief Number of pan buckets the stereo image histogram is quantised into.
/// @details Odd so that one bucket is exactly centre. Panning is a coarse,
///          perceptual quantity here; a finer grid would imply a precision the
///          measurement does not have.
inline constexpr int kPanBucketCount = 9;

/// @brief Energy distribution across the stereo image, per band.
struct ImageOccupancy {
  /// @brief `[band * kPanBucketCount + bucket]`, normalized so each band sums
  ///        to 1 (or to 0 for a band with no energy).
  std::vector<float> histogram;
  /// @brief Per-band crowding in `[0, 1]`: 1 when all of a band's energy sits in
  ///        one bucket, 0 when it is spread evenly.
  std::vector<float> crowding;
  /// @brief True when the band's energy is concentrated enough to be worth
  ///        spreading.
  std::vector<bool> crowded;

  float at(int band, int bucket) const noexcept {
    if (band < 0 || band >= kBandCount || bucket < 0 || bucket >= kPanBucketCount) return 0.0f;
    return histogram[static_cast<std::size_t>(band) * kPanBucketCount +
                     static_cast<std::size_t>(bucket)];
  }
};

/// @brief A track whose own stereo image collapses when summed to mono.
struct MonoRisk {
  int track_index = 0;
  std::string strip_id;
  float correlation = 0.0f;
  float width = 0.0f;
  /// @brief True when the track carries meaningful low-frequency stereo width,
  ///        which is the most common practical complaint.
  bool wide_low_end = false;
};

/// @brief Everything measured across tracks rather than within one.
struct MixProfile {
  int track_count = 0;

  /// @brief Pairwise band dominance, `[(masker * track_count + maskee) *
  ///        kBandCount + band]`.
  /// @details Both `(i, j)` and `(j, i)` are filled. The diagonal is left at its
  ///          default; a track does not mask itself.
  std::vector<BandDominance> dominance;

  /// @brief One entry per unordered track pair, reference index first.
  std::vector<PairAlignment> alignment;

  /// @brief Stereo image histogram over the summed mix.
  ImageOccupancy image;

  /// @brief Tracks whose stereo image is at risk under a mono fold.
  std::vector<MonoRisk> mono_risks;

  /// @brief Band-major dominance accessor. Returns a default entry when out of
  ///        range or on the diagonal.
  BandDominance dominance_at(int masker, int maskee, int band) const noexcept {
    if (masker < 0 || maskee < 0 || masker >= track_count || maskee >= track_count ||
        masker == maskee || band < 0 || band >= kBandCount) {
      return BandDominance{};
    }
    const std::size_t index =
        (static_cast<std::size_t>(masker) * static_cast<std::size_t>(track_count) +
         static_cast<std::size_t>(maskee)) *
            kBandCount +
        static_cast<std::size_t>(band);
    if (index >= dominance.size()) return BandDominance{};
    return dominance[index];
  }
};

}  // namespace sonare::mixing::assistant
