#pragma once

#include <cstddef>

#include "core/audio.h"

namespace sonare::mastering::repair {

/// @brief How a dehum pass removes the harmonic series.
enum class DehumMode {
  /// Tracks each harmonic's amplitude and phase and subtracts the tone those
  /// describe. What leaves the pass is the input minus a sinusoid, so material
  /// sitting at the same frequency but uncorrelated with the series survives.
  Subtract,
  /// Cascade of RBJ notches, one per harmonic. Removes everything inside each
  /// notch's bandwidth, hum or programme.
  Notch,
};

struct DehumConfig {
  float fundamental_hz = 50.0f;
  int harmonics = 4;
  /// Selectivity of the removal. In Notch mode the biquad Q; in Subtract mode
  /// the same quantity read as a rate, the cancellation converging over the
  /// time a notch of bandwidth fundamental_hz / q rings.
  float q = 20.0f;
  bool adaptive = false;
  float search_range_hz = 2.0f;
  float adaptation = 0.25f;
  int frame_size = 2048;
  /// PLL loop bandwidth as a fraction of the tracked frequency. Adaptive
  /// tracking only.
  float pll_bandwidth = 0.01f;
  DehumMode mode = DehumMode::Subtract;
};

/// @brief Longest harmonic series a dehum pass tracks.
/// @details Bounds the notch cascade and fixes the reported level array's size.
/// 16 harmonics reach 800 Hz from a 50 Hz mains fundamental, past where mains
/// hum carries energy worth notching.
inline constexpr int kDehumMaxHarmonics = 16;

/// @brief Largest accepted @c DehumConfig::fundamental_hz.
/// @details Detection runs at fundamentals up to 5 kHz; mains hum sits at 50 or 60 Hz.
inline constexpr float kDehumMaxFundamentalHz = 5000.0f;

/// @brief Largest accepted @c DehumConfig::q; the corpus runs use at most 48.
inline constexpr float kDehumMaxQ = 100.0f;

/// @brief Largest accepted @c DehumConfig::search_range_hz; the adaptive runs use at most 35.
inline constexpr float kDehumMaxSearchRangeHz = 100.0f;

/// @brief Largest accepted @c DehumConfig::frame_size, in samples (8 times the default).
/// @details The frame is both the adaptive analysis block and its look-ahead.
inline constexpr int kDehumMaxFrameSize = 16384;

/// @brief Largest accepted @c DehumConfig::pll_bandwidth, a fraction of the tracked frequency.
inline constexpr float kDehumMaxPllBandwidth = 1.0f;

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
  int notched_harmonics = 0;            ///< Harmonics the pass reached, in either mode.
                                        ///  Fewer than config.harmonics once k*f0 hits
                                        ///  Nyquist.
  float applied_fundamental_hz = 0.0f;  ///< Frequency the last coefficient refresh used.
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
/// @details The adaptive search sums each channel's projected energy, so phase
///   cancellation between channels cannot hide the hum. One original channel
///   with the strongest hum-band projection drives the shared PLL for the whole
///   pass. Only the frequency is shared: each channel keeps its own filter
///   state, so neither channel's transient rings through the other.
DehumStereoResult dehum_stereo(const Audio& left, const Audio& right,
                               const DehumConfig& config = {});

}  // namespace sonare::mastering::repair
