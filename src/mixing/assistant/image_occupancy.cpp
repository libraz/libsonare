/// @file image_occupancy.cpp
/// @brief Stereo image occupancy and mono-fold risk implementation.

#include "mixing/assistant/image_occupancy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <utility>

#include "core/audio.h"
#include "core/spectrum.h"
#include "mastering/stereo/mono_compat_check.h"
#include "mixing/meter.h"
#include "util/constants.h"

namespace sonare::mixing::assistant {
namespace {

using sonare::constants::kEpsilon;
using sonare::mixing::kMaxMonoCompatWidth;

// A band counts as crowded once its energy is effectively concentrated into
// fewer than this many of the kPanBucketCount positions. `exp(entropy)` is the
// effective number of occupied buckets, so 1.7 sits between a single point
// source (1.0) and the least spread an intentional stereo placement produces, a
// band split evenly between two positions (2.0).
constexpr double kCrowdedEffectiveBuckets = 1.7;

// Correlation below which a mono fold is worth reporting. mono_compat_check
// defaults to 0, which only catches outright cancellation; the assistant flags
// earlier, at the point where folding an equal-level pair already costs it
// close to 1.9 dB against the fully correlated case (mid energy scales as
// (1 + r) / 2).
constexpr float kMonoRiskCorrelation = 0.30f;

// Width, sqrt(side / mid) over the orthonormal M/S transform, above which the
// side channel carries more energy than the mid and the fold therefore discards
// over half the track. Equivalent to correlation 0 for an equal-level pair, so
// this is the level-dependent counterpart of kMonoRiskCorrelation rather than a
// second opinion on the same number: correlation is blind to a level imbalance
// between the channels and width is not.
constexpr float kMonoRiskWidth = 1.0f;

// The sub and low bands of kBands, i.e. everything below 250 Hz. Conventional
// practice keeps this span near-mono, and it is where the complaint is raised.
constexpr int kLowEndBandCount = 2;
static_assert(kLowEndBandCount <= kBandCount, "low-end span must fit inside the band grid");

// The sub and low bands must hold at least this share of the track's band energy
// before their stereo spread is worth reporting. Below it the reading comes from
// a bright source's low-frequency skirt rather than from anything audible.
constexpr double kLowEndPresenceShare = 0.10;

// Side share -- side / (mid + side) -- across the sub and low bands above which
// the low end counts as wide. For an equal-level pair the share is (1 - r) / 2,
// so 0.25 is a low-end correlation of 0.5: clear of the near-mono low end
// conventional practice aims for, and far from the r around 0.9 a gently
// widened bass sits at.
constexpr double kLowEndSideShare = 0.25;

/// @brief Index of the @ref kBands band containing @p hz, or -1 when it falls
///        outside the grid.
int band_of_frequency(float hz) noexcept {
  for (int band = 0; band < kBandCount; ++band) {
    if (hz >= kBands[band].low_hz && hz < kBands[band].high_hz) return band;
  }
  return -1;
}

/// @brief The analysis geometry the profile was measured with, so a band energy
///        computed here lines up with the one cached in the profile.
sonare::StftConfig stft_config_for(const TrackProfile& profile) {
  const TrackProfileConfig defaults;
  const int n_fft = profile.bands.n_fft > 0 ? profile.bands.n_fft : defaults.n_fft;
  const int hop_length =
      profile.bands.hop_length > 0 ? profile.bands.hop_length : defaults.hop_length;
  return sonare::make_stft_config(n_fft, hop_length);
}

/// @brief Folds one track's channels into per-band linear power.
/// @details The two channel transforms are the expensive part of both image
///          passes, so this is called once per track by
///          @ref measure_track_channel_energy and never from a pass directly.
///          Degenerate input returns an entry with
///          @ref TrackChannelEnergy::valid false rather than throwing.
///
///          Both spectrograms are alive at once, and nothing else in the
///          assistant holds one: the mid/side pair follows from the two channel
///          transforms bin by bin, so neither can be released before the other
///          has been read. They are released together, before the next track,
///          which is what keeps the working set to one track's worth rather
///          than the session's.
TrackChannelEnergy measure_band_energy(const TrackInput& track, const TrackProfile& profile) {
  TrackChannelEnergy energy;
  if (track.left == nullptr || track.frame_count == 0 || track.sample_rate <= 0) return energy;

  const sonare::StftConfig config = stft_config_for(profile);
  const sonare::Spectrogram left_spec = sonare::Spectrogram::compute(
      sonare::Audio::from_buffer(track.left, track.frame_count, track.sample_rate), config);
  const int n_bins = left_spec.n_bins();
  const int n_frames = left_spec.n_frames();
  if (n_bins <= 0 || n_frames <= 0) return energy;

  sonare::Spectrogram right_spec;
  bool stereo = track.right != nullptr;
  if (stereo) {
    right_spec = sonare::Spectrogram::compute(
        sonare::Audio::from_buffer(track.right, track.frame_count, track.sample_rate), config);
    // Both channels come from one frame count, so a geometry mismatch can only
    // mean the second transform bailed out. Fall back to reading the track as
    // mono rather than indexing one grid with the other's stride.
    stereo = right_spec.n_bins() == n_bins && right_spec.n_frames() == n_frames;
  }

  const std::complex<float>* left_data = left_spec.complex_data();
  const std::complex<float>* right_data = stereo ? right_spec.complex_data() : nullptr;
  const double bin_hz = static_cast<double>(track.sample_rate) / static_cast<double>(config.n_fft);

  for (int bin = 0; bin < n_bins; ++bin) {
    const int band = band_of_frequency(static_cast<float>(static_cast<double>(bin) * bin_hz));
    if (band < 0) continue;
    const std::size_t base = static_cast<std::size_t>(bin) * static_cast<std::size_t>(n_frames);
    for (int frame = 0; frame < n_frames; ++frame) {
      const std::complex<float> left_bin = left_data[base + static_cast<std::size_t>(frame)];
      if (!stereo) {
        // A mono track is one source at centre: it has no side component and its
        // whole energy is mid.
        const double power = static_cast<double>(std::norm(left_bin));
        energy.left[band] += power;
        energy.mid[band] += power;
        continue;
      }
      const std::complex<float> right_bin = right_data[base + static_cast<std::size_t>(frame)];
      // The STFT is linear, so the mid/side pair follows from the two channel
      // transforms without a third and a fourth one, and without a separate
      // low-pass anywhere downstream. The 0.5 scaling matches the time-domain
      // convention mastering::stereo::mono_compat_check uses.
      const std::complex<float> mid_bin = (left_bin + right_bin) * 0.5f;
      const std::complex<float> side_bin = (left_bin - right_bin) * 0.5f;
      energy.left[band] += static_cast<double>(std::norm(left_bin));
      energy.right[band] += static_cast<double>(std::norm(right_bin));
      energy.mid[band] += static_cast<double>(std::norm(mid_bin));
      energy.side[band] += static_cast<double>(std::norm(side_bin));
    }
  }

  energy.valid = true;
  energy.stereo = stereo;
  return energy;
}

/// @brief Quantises a pan position in `[-1, 1]` onto the bucket grid.
/// @details The buckets tile the range at the uniform width
///          `2 / kPanBucketCount`, each half-open `[low, high)` so a value never
///          lands in two; the topmost is closed at `+1` so hard right has
///          somewhere to go. kPanBucketCount is odd, so bucket
///          `kPanBucketCount / 2` is centred on zero and holds `pan == 0`.
int pan_bucket(double pan) noexcept {
  const double clamped = std::clamp(pan, -1.0, 1.0);
  const double scaled = (clamped + 1.0) * 0.5 * static_cast<double>(kPanBucketCount);
  return std::clamp(static_cast<int>(std::floor(scaled)), 0, kPanBucketCount - 1);
}

/// @brief Crowding threshold expressed through the effective bucket count.
double crowded_threshold() noexcept {
  return 1.0 - std::log(kCrowdedEffectiveBuckets) / std::log(static_cast<double>(kPanBucketCount));
}

/// @brief True when the track carries meaningful stereo width below 250 Hz.
/// @details Measured on the same @ref kBands split the histogram uses, from the
///          mid/side pair the two channel transforms already give by linearity.
///          mono_compat_check_log_bands would supply a band-limited correlation
///          without a transform, but it imposes its own fractional-octave grid,
///          and a threshold chosen for the sub/low span must not be re-derived
///          on a different grid.
bool has_wide_low_end(const TrackChannelEnergy& energy) noexcept {
  if (!energy.valid || !energy.stereo) return false;

  double low_mid = 0.0;
  double low_side = 0.0;
  double total = 0.0;
  for (int band = 0; band < kBandCount; ++band) {
    total += energy.left[band] + energy.right[band];
    if (band < kLowEndBandCount) {
      low_mid += energy.mid[band];
      low_side += energy.side[band];
    }
  }

  const double low_total = low_mid + low_side;
  if (!(total > static_cast<double>(kEpsilon)) || !(low_total > 0.0)) return false;
  // Under the 0.5 M/S convention mid + side equals half of left + right, so the
  // doubled low total is on the same scale as `total`.
  if (2.0 * low_total < kLowEndPresenceShare * total) return false;
  return low_side / low_total >= kLowEndSideShare;
}

}  // namespace

std::vector<TrackChannelEnergy> measure_track_channel_energy(
    const std::vector<TrackInput>& tracks, const std::vector<TrackProfile>& profiles) {
  const std::size_t track_count = std::min(tracks.size(), profiles.size());
  std::vector<TrackChannelEnergy> energies(track_count);
  for (std::size_t index = 0; index < track_count; ++index) {
    // An excluded track contributes to neither pass, so it is never measured;
    // its entry stays invalid, which is what both passes already skip on.
    if (!profiles[index].usable) continue;
    energies[index] = measure_band_energy(tracks[index], profiles[index]);
  }
  return energies;
}

ImageOccupancy analyze_image_occupancy(const std::vector<TrackInput>& tracks,
                                       const std::vector<TrackProfile>& profiles) {
  return analyze_image_occupancy(measure_track_channel_energy(tracks, profiles));
}

ImageOccupancy analyze_image_occupancy(const std::vector<TrackChannelEnergy>& energies) {
  ImageOccupancy image;
  const std::size_t slot_count =
      static_cast<std::size_t>(kBandCount) * static_cast<std::size_t>(kPanBucketCount);
  image.histogram.assign(slot_count, 0.0f);
  image.crowding.assign(static_cast<std::size_t>(kBandCount), 0.0f);
  image.crowded.assign(static_cast<std::size_t>(kBandCount), false);

  // Accumulated in double: band energies from tracks of very different levels
  // are summed before they are ever normalized.
  std::vector<double> accumulator(slot_count, 0.0);
  for (const TrackChannelEnergy& energy : energies) {
    if (!energy.valid) continue;

    for (int band = 0; band < kBandCount; ++band) {
      const double band_total = energy.left[band] + energy.right[band];
      if (!(band_total > static_cast<double>(kEpsilon))) continue;
      // A mono track is one centred source; a stereo track is placed per band
      // from that band's own L/R balance, so a narrow low end and a wide top end
      // land in different buckets.
      const double pan =
          energy.stereo ? (energy.right[band] - energy.left[band]) / band_total : 0.0;
      const std::size_t slot =
          static_cast<std::size_t>(band) * static_cast<std::size_t>(kPanBucketCount) +
          static_cast<std::size_t>(pan_bucket(pan));
      accumulator[slot] += band_total;
    }
  }

  const double log_buckets = std::log(static_cast<double>(kPanBucketCount));
  const double threshold = crowded_threshold();
  for (int band = 0; band < kBandCount; ++band) {
    const std::size_t base =
        static_cast<std::size_t>(band) * static_cast<std::size_t>(kPanBucketCount);
    double band_total = 0.0;
    for (int bucket = 0; bucket < kPanBucketCount; ++bucket) {
      band_total += accumulator[base + static_cast<std::size_t>(bucket)];
    }
    // A band nothing reached keeps its all-zero row, a zero crowding and no
    // crowded flag: there is no distribution to describe, which is not the same
    // as an evenly spread one.
    if (!(band_total > 0.0)) continue;

    double entropy = 0.0;
    for (int bucket = 0; bucket < kPanBucketCount; ++bucket) {
      const std::size_t slot = base + static_cast<std::size_t>(bucket);
      const double share = accumulator[slot] / band_total;
      image.histogram[slot] = static_cast<float>(share);
      if (share > 0.0) entropy -= share * std::log(share);
    }
    const double crowding = std::clamp(1.0 - entropy / log_buckets, 0.0, 1.0);
    image.crowding[static_cast<std::size_t>(band)] = static_cast<float>(crowding);
    image.crowded[static_cast<std::size_t>(band)] = crowding > threshold;
  }

  return image;
}

std::vector<MonoRisk> analyze_mono_risks(const std::vector<TrackInput>& tracks,
                                         const std::vector<TrackProfile>& profiles) {
  return analyze_mono_risks(tracks, profiles, measure_track_channel_energy(tracks, profiles));
}

std::vector<MonoRisk> analyze_mono_risks(const std::vector<TrackInput>& tracks,
                                         const std::vector<TrackProfile>& profiles,
                                         const std::vector<TrackChannelEnergy>& energies) {
  std::vector<MonoRisk> risks;
  const std::size_t track_count = std::min(tracks.size(), profiles.size());
  for (std::size_t index = 0; index < track_count; ++index) {
    const TrackInput& track = tracks[index];
    const TrackProfile& profile = profiles[index];
    if (!profile.usable) continue;
    // A mono track has no stereo image, so the fold cannot take one away.
    if (track.left == nullptr || track.right == nullptr) continue;
    if (track.frame_count == 0 || track.sample_rate <= 0) continue;

    // Passing the assistant's own threshold in means likely_mono_compatible is
    // the correlation verdict, rather than the same comparison written twice.
    const mastering::stereo::MonoCompatResult compat = mastering::stereo::mono_compat_check(
        track.left, track.right, track.frame_count, kMonoRiskCorrelation);
    const bool wide_low_end = index < energies.size() && has_wide_low_end(energies[index]);
    const bool too_wide = compat.width > kMonoRiskWidth;
    if (compat.likely_mono_compatible && !too_wide && !wide_low_end) continue;

    MonoRisk risk;
    risk.track_index = static_cast<int>(index);
    risk.strip_id = profile.strip_id.empty() ? track.id : profile.strip_id;
    risk.correlation = compat.correlation;
    // stereo_width diverges to +inf once the mid collapses. Report the finite
    // sentinel the meter path already publishes so the value stays serializable.
    risk.width = std::min(compat.width, kMaxMonoCompatWidth);
    risk.wide_low_end = wide_low_end;
    risks.push_back(std::move(risk));
  }
  return risks;
}

}  // namespace sonare::mixing::assistant
