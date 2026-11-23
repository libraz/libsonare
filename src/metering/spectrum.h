#pragma once

/// @file spectrum.h
/// @brief Meter-oriented spectrum views.

#include <vector>

#include "core/audio.h"
#include "util/constants.h"
#include "util/parameter_checks.h"
#include "util/resource_limits.h"

namespace sonare::metering {

/// @brief What the spectrum meter's FFT size accepts, as a power of two; the ceiling is the
///        meter's FFT resource limit.
inline constexpr util::check::NumericDomain kSpectrumNFftDomain = util::check::integer_domain(
    2, static_cast<long long>(resource::kDefaultSpectrumResourceLimits.max_fft_size));

struct SpectrumConfig {
  int n_fft = 2048;
  bool apply_octave_smoothing = false;
  int octave_fraction = 3;
  float db_ref = 1.0f;
  /// Clamp on the magnitude before the dB conversion, max(magnitude, db_amin); silence reads
  /// 20·log10(db_amin / db_ref) under it.
  float db_amin = constants::kEpsilon;
};

struct SpectrumResult {
  std::vector<float> frequencies;
  std::vector<float> magnitude;
  std::vector<float> power;
  std::vector<float> db;
  int n_fft = 0;
  int sample_rate = 0;
};

/// @brief Welch-averaged spectrum over the whole buffer (time-averaged; NOT a
///        single-frame snapshot).
SpectrumResult spectrum(const Audio& audio, const SpectrumConfig& config = {});

/// @brief True single-frame spectrum: one Hann-windowed @c n_fft-length FFT
///        starting at @p frame_offset, zero-padded past the end of the buffer.
///        Unlike @ref spectrum, the result is a single moment and is not
///        time-averaged.
SpectrumResult spectrum_frame(const Audio& audio, size_t frame_offset,
                              const SpectrumConfig& config = {});

std::vector<float> smooth_fractional_octave(const std::vector<float>& values,
                                            const std::vector<float>& frequencies,
                                            int octave_fraction = 3);

}  // namespace sonare::metering
