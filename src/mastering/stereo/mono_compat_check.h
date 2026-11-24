#pragma once

/// @file mono_compat_check.h
/// @brief Stereo mono compatibility analysis helper.

#include <cstddef>
#include <optional>
#include <vector>

#include "util/db.h"
#include "util/parameter_checks.h"

namespace sonare::mastering::stereo {

struct MonoCompatResult {
  float correlation = sonare::kNotMeasured;  ///< NaN when either channel is silent.
  float width = 0.0f;
  float mono_peak = 0.0f;
  float side_rms = 0.0f;
  bool likely_mono_compatible = true;
};

/// @brief One log-spaced band's stereo readout.
///
/// `correlation` and `side_rms` are measured OVER the interval
/// [low_hz, high_hz), not at a single representative frequency inside it, so two
/// components sharing a band both contribute: an in-phase and an anti-phase
/// partial of equal energy in one band read as an uncorrelated band, which is
/// what they sum to in mono.
struct MonoCompatBandResult {
  float low_hz = 0.0f;
  float high_hz = 0.0f;
  float correlation = sonare::kNotMeasured;  ///< NaN when either channel is silent in the band.
  float side_rms = 0.0f;
};

/// @brief Domain of the log-band count per octave, stated once for the core check.
inline constexpr util::check::NumericDomain kMonoCompatBandsPerOctaveDomain =
    util::check::integer_domain_of<int>(1);

/// @brief Refuses a log-band count outside @c kMonoCompatBandsPerOctaveDomain under @p subject.
void check_mono_compat_bands_per_octave(int bands_per_octave, const char* subject);

MonoCompatResult mono_compat_check(const float* left, const float* right, size_t length,
                                   float correlation_threshold = 0.0f);
/// @brief Per-band stereo readout over log-spaced bands from @p low_hz to @p high_hz.
/// @param high_hz Upper edge; absent reads as 20 kHz or Nyquist, whichever is lower. A given
///        edge above Nyquist is refused.
std::vector<MonoCompatBandResult> mono_compat_check_log_bands(
    const float* left, const float* right, size_t length, double sample_rate,
    int bands_per_octave = 3, float low_hz = 20.0f, std::optional<float> high_hz = std::nullopt);

}  // namespace sonare::mastering::stereo
