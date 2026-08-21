/// @file masking.cpp
/// @brief Pairwise band dominance for the mixing assistant.

#include "mixing/assistant/masking.h"

#include <algorithm>
#include <cstddef>
#include <vector>

#include "util/db.h"

namespace sonare::mixing::assistant {
namespace {

// A frame counts towards a band's dominance only while the track's energy in
// that band stays within this much of the loudest frame the same track reaches
// in the same band. The floor is relative rather than absolute because an
// absolute one follows the material's level: the identical part rendered 20 dB
// quieter would silently lose most of its frames and report a different figure.
// It is referenced to the band's own peak rather than to the track's overall
// peak so that a part is judged on how it behaves in that band, not on how loud
// the rest of its spectrum happens to be. 60 dB below a peak is the convention
// for where a decaying tail stops contributing, which keeps note tails and the
// quiet side of a dynamic part in while dropping the between-note gaps, whose
// ratio carries no information. The value is power dB, matching the linear
// power stored in BandEnergyEnvelope.
constexpr float kBandFloorBelowPeakDb = -60.0f;

// A second floor, referenced to the loudest band the track reaches anywhere.
// The per-band floor above answers "is this part sounding in this band right
// now", and on its own it answers yes even in a band the part does not occupy
// at all: window leakage and a transient's high-frequency skirt sit comfortably
// within 60 dB of their own band's peak, so a bass line reads as present up to
// Nyquist and lands in dominance rows for bands it has no business in. A
// downstream stage acting on those rows carves a track where it carries nothing.
// Measured against synthetic parts with known band placement, the bands a part
// genuinely occupies sit within roughly 30 dB of its loudest band while the ones
// it does not fall away to 38 dB and below, so the line is drawn between them.
// Power dB, matching the linear power in BandEnergyEnvelope.
constexpr float kBandFloorBelowTrackPeakDb = -35.0f;

// Upper bound on the number of frames one pair is evaluated over; past it the
// frame grid is walked with a whole-number stride. The result is a mean over
// frames, and a few thousand samples pin a mean far tighter than the underlying
// measurement's own accuracy, while the cost is quadratic in track count and so
// a full-resolution walk of a long arrangement buys nothing. At a 512-sample
// hop and 48 kHz this covers roughly 44 s before any decimation begins, so
// ordinary material is measured frame by frame.
constexpr int kMaxAnalyzedFrames = 4096;

/// @brief Flat index into the dominance matrix, matching MixProfile::dominance_at.
std::size_t dominance_index(std::size_t masker, std::size_t maskee, std::size_t track_count,
                            int band) noexcept {
  return (masker * track_count + maskee) * static_cast<std::size_t>(kBandCount) +
         static_cast<std::size_t>(band);
}

/// @brief Flat index into the per (track, band) floor table.
std::size_t floor_index(std::size_t track, int band) noexcept {
  return track * static_cast<std::size_t>(kBandCount) + static_cast<std::size_t>(band);
}

// True when the envelope's storage really holds every band it claims to.
// BandEnergyEnvelope::at() range-checks the frame index but trusts the vector's
// size, so a short buffer would be read past its end.
bool envelope_is_well_formed(const BandEnergyEnvelope& bands) noexcept {
  if (bands.n_frames <= 0) return false;
  const std::size_t required =
      static_cast<std::size_t>(kBandCount) * static_cast<std::size_t>(bands.n_frames);
  return bands.energy.size() >= required;
}

}  // namespace

std::vector<BandDominance> analyze_band_dominance(const std::vector<TrackProfile>& profiles) {
  const std::size_t track_count = profiles.size();
  std::vector<BandDominance> dominance(track_count * track_count *
                                       static_cast<std::size_t>(kBandCount));
  if (track_count < 2) return dominance;

  // Which tracks take part, and the longest envelope among them. An unusable
  // track keeps its whole row and column default-constructed.
  std::vector<bool> measurable(track_count, false);
  int max_frames = 0;
  for (std::size_t track = 0; track < track_count; ++track) {
    const TrackProfile& profile = profiles[track];
    if (!profile.usable || !envelope_is_well_formed(profile.bands)) continue;
    measurable[track] = true;
    max_frames = std::max(max_frames, profile.bands.n_frames);
  }
  if (max_frames <= 0) return dominance;

  // One stride shared by every pair, so the whole matrix is sampled on the same
  // frame grid and its entries stay comparable with each other.
  const int frame_stride = 1 + (max_frames - 1) / kMaxAnalyzedFrames;

  // Per (track, band) energy floor, taken from that band's loudest frame on the
  // same grid the dominance sweep will visit.
  const float floor_scale = db_to_power_scalar(kBandFloorBelowPeakDb);
  const float track_floor_scale = db_to_power_scalar(kBandFloorBelowTrackPeakDb);
  std::vector<float> floors(track_count * static_cast<std::size_t>(kBandCount), 0.0f);
  for (std::size_t track = 0; track < track_count; ++track) {
    if (!measurable[track]) continue;
    const BandEnergyEnvelope& bands = profiles[track].bands;
    std::vector<float> band_peaks(static_cast<std::size_t>(kBandCount), 0.0f);
    float track_peak = 0.0f;
    for (int band = 0; band < kBandCount; ++band) {
      float peak = 0.0f;
      for (int frame = 0; frame < bands.n_frames; frame += frame_stride) {
        peak = std::max(peak, bands.at(band, frame));
      }
      band_peaks[static_cast<std::size_t>(band)] = peak;
      track_peak = std::max(track_peak, peak);
    }
    // The higher of the two floors wins: a frame has to be loud for its own band
    // and the band has to be one the track actually occupies.
    const float occupancy_floor = track_peak * track_floor_scale;
    for (int band = 0; band < kBandCount; ++band) {
      floors[floor_index(track, band)] =
          std::max(band_peaks[static_cast<std::size_t>(band)] * floor_scale, occupancy_floor);
    }
  }

  // Band, then pair, then frame. Each track's band row is contiguous inside its
  // envelope, so holding one band while sweeping a pair's frames keeps both
  // rows resident; the envelopes are already folded to [band][frame], so an
  // outer pair loop re-reads no spectrogram either way.
  for (int band = 0; band < kBandCount; ++band) {
    for (std::size_t first = 0; first < track_count; ++first) {
      if (!measurable[first]) continue;
      const BandEnergyEnvelope& first_bands = profiles[first].bands;
      const float first_floor = floors[floor_index(first, band)];
      if (first_floor <= 0.0f) continue;  // the band is silent for this track

      for (std::size_t second = first + 1; second < track_count; ++second) {
        if (!measurable[second]) continue;
        const BandEnergyEnvelope& second_bands = profiles[second].bands;
        const float second_floor = floors[floor_index(second, band)];
        if (second_floor <= 0.0f) continue;

        // Both directions accumulate in the same sweep, which is why they share
        // one set of valid frames by construction. Filling one and transposing
        // it afterwards would be a second chance to disagree.
        //
        // The share is evaluated frame by frame and only then averaged: two
        // parts that occupy the same band but never sound at the same moment do
        // not interfere, and a mean spectrum cannot tell that case apart from a
        // genuine collision.
        double first_sum = 0.0;
        double second_sum = 0.0;
        int valid_frames = 0;
        for (int frame = 0; frame < max_frames; frame += frame_stride) {
          const float first_energy = first_bands.at(band, frame);
          if (first_energy <= first_floor) continue;
          const float second_energy = second_bands.at(band, frame);
          if (second_energy <= second_floor) continue;
          const double total = static_cast<double>(first_energy) + second_energy;
          first_sum += first_energy / total;
          second_sum += second_energy / total;
          ++valid_frames;
        }
        // Never sounding together in this band is an absence of interference,
        // so the pair keeps its default entry rather than a fabricated ratio.
        if (valid_frames == 0) continue;

        const double inverse = 1.0 / static_cast<double>(valid_frames);
        BandDominance& first_masks_second =
            dominance[dominance_index(first, second, track_count, band)];
        first_masks_second.ratio = static_cast<float>(first_sum * inverse);
        first_masks_second.valid_frames = valid_frames;

        BandDominance& second_masks_first =
            dominance[dominance_index(second, first, track_count, band)];
        second_masks_first.ratio = static_cast<float>(second_sum * inverse);
        second_masks_first.valid_frames = valid_frames;
      }
    }
  }

  return dominance;
}

}  // namespace sonare::mixing::assistant
