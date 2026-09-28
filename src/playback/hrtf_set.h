#pragma once

/// @file hrtf_set.h
/// @brief An HRTF set on a regular azimuth x elevation grid, with bilinear
///        time-domain interpolation of the minimum-phase HRIRs and the ITD.
///
/// Azimuth wraps around; elevation outside the grid is clamped to the nearest
/// row. The set is immutable once built, so one set may back several renderers.

#include <cstddef>
#include <cstdint>

#include "playback/shrf_format.h"

namespace sonare::playback {

class HrtfSet {
 public:
  /// Parses SHRF v1 bytes (copied).
  /// @throws SonareException(InvalidParameter) on malformed data.
  static HrtfSet from_memory(const uint8_t* data, size_t size);
#ifndef __EMSCRIPTEN__
  /// The embedded default set (native builds only).
  static HrtfSet builtin_default();
#endif

  /// Copy at @p sample_rate: HRIRs resampled, ITDs scaled by the rate ratio.
  HrtfSet resampled(int sample_rate) const;

  int sample_rate() const noexcept;
  int taps() const noexcept;
  int azimuth_count() const noexcept;
  int elevation_count() const noexcept;
  float azimuth_step_deg() const noexcept;
  float elevation_min_deg() const noexcept;
  float elevation_step_deg() const noexcept;

  /// Grid HRIR of one ear (0 left, 1 right), `taps()` samples.
  const float* hrir(int elevation_index, int azimuth_index, int ear) const noexcept;
  /// Grid ITD in samples (positive: the left ear lags).
  float itd_samples(int elevation_index, int azimuth_index) const noexcept;

  /// Bilinear interpolation at a head-relative direction. Writes `taps()`
  /// samples to each of @p left and @p right. Allocation-free.
  void interpolate(float azimuth_deg, float elevation_deg, float* left, float* right,
                   float* itd_samples) const noexcept;

 private:
  explicit HrtfSet(ShrfData data);

  ShrfData data_;
};

}  // namespace sonare::playback
