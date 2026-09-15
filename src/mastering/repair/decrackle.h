#pragma once

#include <cstddef>

#include "core/audio.h"

namespace sonare::mastering::repair {

enum class DecrackleMode {
  Median,
  WaveletShrinkage,
};

struct DecrackleConfig {
  float threshold = 0.4f;
  DecrackleMode mode = DecrackleMode::Median;
  int levels = 4;
};

/// Validates every public DecrackleConfig field. The mono, stereo and detection
/// entrypoints share this oracle so range and enum handling cannot drift
/// between them.
void validate_config(const DecrackleConfig& config);

/// @brief What a decrackle analysis found.
/// @details Crackle is measured by the median criterion regardless of the
///   configured DecrackleMode. Wavelet shrinkage is a removal method, not a
///   detection method -- it shrinks coefficients without ever deciding that a
///   sample is crackle -- so the median deviation is this module's only
///   definition of the defect. A caller therefore gets the same answer before
///   choosing a mode, which is the order the repair assistant asks in.
struct CrackleDetection {
  size_t sample_count = 0;  ///< Samples deviating from the local median by
                            ///  more than threshold.
  float sample_fraction = 0.0f;
  float per_second = 0.0f;
};

/// @brief Measures crackle without repairing.
CrackleDetection detect_crackle(const float* samples, size_t size, int sample_rate,
                                const DecrackleConfig& config = {});

/// @brief What a decrackle pass found in one channel and what it did to it.
/// @details The two modes remove crackle by different means and report through
///   different fields. A field belonging to the other mode reads zero because
///   that mode did not run, which the caller knows from the config it passed --
///   it is not an unfilled value.
struct DecrackleReport {
  CrackleDetection detected;       ///< This channel's own analysis of the input.
  size_t replaced_samples = 0;     ///< Median mode: samples the filter overwrote.
                                   ///  Equal to detected.sample_count, since the
                                   ///  detector and the repair share a criterion.
  size_t detail_coefficients = 0;  ///< Wavelet mode: detail coefficients examined.
  size_t shrunk_coefficients = 0;  ///< Wavelet mode: of those, driven to zero.
  float noise_sigma = 0.0f;        ///< Wavelet mode: the MAD noise estimate that
                                   ///  set every level's threshold. The configured
                                   ///  threshold is only a cap on it.
};

Audio decrackle(const Audio& audio, const DecrackleConfig& config = {});

/// @brief Decrackles @p audio and reports what the pass found and did.
Audio decrackle(const Audio& audio, const DecrackleConfig& config, DecrackleReport* report);

/// @brief A decrackled stereo pair and what each channel's pass did.
struct DecrackleStereoResult {
  Audio left;
  Audio right;
  DecrackleReport left_report;
  DecrackleReport right_report;
};

/// @brief Decrackles a stereo pair, each channel on its own.
/// @details Crackle is surface damage: the two channels carry different scratches
///   at different instants, so there is no common event for a shared decision to
///   agree about. Both modes are memoryless across channels, so the pair is
///   processed independently and this entrypoint exists to keep the reports and
///   the channel-length contract in one place.
DecrackleStereoResult decrackle_stereo(const Audio& left, const Audio& right,
                                       const DecrackleConfig& config = {});

}  // namespace sonare::mastering::repair
