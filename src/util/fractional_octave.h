#pragma once

/// @file fractional_octave.h
/// @brief Fractional-octave spectrum smoothing utility.
/// @details Lives in `util/` because both `metering/` (spectrum meters) and
///          `mastering/match/` (reference-spectrum matching) need it. Keeping
///          the helper layer-neutral avoids forcing `mastering/` to reach into
///          `metering/` for what is fundamentally a generic numeric utility.

#include <vector>

namespace sonare::util {

/// @brief Smooths a per-bin value vector using a fractional-octave moving average.
/// @details For each bin i with center frequency frequencies[i], averages all
///          bins whose frequency falls inside [center/ratio, center*ratio] where
///          ratio = 2^(1/(2*octave_fraction)). The DC bin (index 0) is passed
///          through unchanged.
/// @param values Per-bin values (magnitude, power, dB, ...). Size == frequencies.size().
/// @param frequencies Per-bin center frequencies (Hz), monotonically increasing.
/// @param octave_fraction Bandwidth divisor (e.g. 3 = 1/3 octave). Must be > 0.
/// @return Smoothed values, same length as input.
/// @throws SonareException(InvalidParameter) for non-positive octave_fraction or
///         mismatched input lengths.
std::vector<float> smooth_fractional_octave(const std::vector<float>& values,
                                            const std::vector<float>& frequencies,
                                            int octave_fraction = 3);

}  // namespace sonare::util
