#pragma once

#include <cstddef>

#include "core/audio.h"

namespace sonare::mastering::repair {

struct DehumConfig {
  float fundamental_hz = 50.0f;
  int harmonics = 4;
  float q = 20.0f;
  bool adaptive = false;
  float search_range_hz = 2.0f;
  float adaptation = 0.25f;
  int frame_size = 2048;
  float pll_bandwidth = 0.01f;
};

/// @brief Longest harmonic series a dehum pass tracks.
/// @details Bounds the notch cascade and fixes the reported level array's size.
/// 16 harmonics reach 800 Hz from a 50 Hz mains fundamental, past where mains
/// hum carries energy worth notching.
inline constexpr int kDehumMaxHarmonics = 16;

/// Validates every public DehumConfig field. The mono, stereo and detection
/// entrypoints share this oracle so range handling cannot drift between them.
void validate_config(const DehumConfig& config);

/// @brief What a dehum analysis found.
struct HumDetection {
  float fundamental_hz = 0.0f;  ///< Tracked fundamental. The configured
                                ///  value when adaptive tracking is off.
  /// How far the winning candidate stood above the rest of the search: the
  /// ratio of its projected energy to the median of the candidates. 1.0 means
  /// the search found no peak at all. Not a lock flag.
  float fundamental_prominence = 1.0f;
  int harmonics = 0;  ///< Harmonics found above the floor. Not necessarily a
                      ///  contiguous run from the first.
  /// Input level at each k*f0, k ascending, floored at the library dB floor.
  /// Measured for every k the sample rate can carry, not only the ones the
  /// cascade notches; a k*f0 at or past Nyquist reads the floor because nothing
  /// is there to measure.
  float harmonic_dbfs[kDehumMaxHarmonics] = {};
};

/// @brief Measures hum without filtering.
/// @details Always runs the estimation path, whatever @c config.adaptive says:
///   the default path notches the configured frequency without ever looking for
///   hum, so a detector following the flag would hand back its own input.
HumDetection detect_hum(const float* samples, size_t size, int sample_rate,
                        const DehumConfig& config = {});

/// @brief What a dehum pass found in one channel and what it did to it.
struct DehumReport {
  HumDetection detected;                ///< Analysis of the input, before filtering.
  int notched_harmonics = 0;            ///< Harmonics the cascade reached. Fewer than
                                        ///  config.harmonics once k*f0 hits Nyquist.
  float applied_fundamental_hz = 0.0f;  ///< Frequency the last notch refresh used.
  float fundamental_drift_hz = 0.0f;    ///< Largest excursion of the tracked
                                        ///  frequency from the configured one.
                                        ///  Zero without adaptive tracking, which
                                        ///  is the measurement, not an unset field.
};

Audio dehum(const Audio& audio, const DehumConfig& config = {});

/// @brief Dehums @p audio and reports what the pass found and did.
Audio dehum(const Audio& audio, const DehumConfig& config, DehumReport* report);

/// @brief A dehummed stereo pair and what each channel's pass did.
struct DehumStereoResult {
  Audio left;
  Audio right;
  DehumReport left_report;
  DehumReport right_report;
};

/// @brief Dehums a stereo pair on one shared tracked fundamental.
/// @details Mains hum is one physical source, so the two channels carry the same
///   frequency and tracking them apart puts the notches at two frequencies that
///   differ by whatever each channel's programme material pulled its own search
///   to -- an image shift the hum itself never had. The tracker therefore reads
///   the channel mean and both cascades follow it. Only the frequency is shared:
///   each channel keeps its own filter state, so neither channel's transient
///   rings through the other.
DehumStereoResult dehum_stereo(const Audio& left, const Audio& right,
                               const DehumConfig& config = {});

}  // namespace sonare::mastering::repair
