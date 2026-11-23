#pragma once

/// @file silence.h
/// @brief librosa.effects.trim / split — silence boundary detection.
/// @details Distinct from sonare::trim_absolute(const Audio&, ...) in
///          src/effects/normalize.h, which operates on Audio. These functions
///          take raw float buffers and return sample-index ranges.

#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

#include "util/db.h"
#include "util/parameter_checks.h"

namespace sonare {

/// Silence threshold, in dB under the peak RMS, when a caller gives none (librosa's trim/split).
inline constexpr float kDefaultSilenceTopDb = 60.0f;
/// RMS frame and hop of the silence detectors when a caller gives none (librosa's trim/split).
inline constexpr int kDefaultSilenceFrameLength = 2048;
inline constexpr int kDefaultSilenceHopLength = 512;

/// @brief What the RMS frame length of the silence detectors accepts.
inline constexpr util::check::NumericDomain kSilenceFrameLengthDomain =
    util::check::integer_domain_of<int>(1);
/// @brief What the RMS hop length of the silence detectors accepts.
inline constexpr util::check::NumericDomain kSilenceHopLengthDomain =
    util::check::integer_domain_of<int>(1);
/// @brief What a buffer length accepts so its sample indices fit the int interval arithmetic.
inline constexpr util::check::NumericDomain kSilenceIndexableLengthDomain =
    util::check::integer_domain(0, std::numeric_limits<int>::max());
/// @brief What the signal count of a common split accepts.
inline constexpr util::check::NumericDomain kSilenceSignalCountDomain =
    util::check::integer_domain_of<long long>(1);

/// @brief Result of trim().
struct TrimResult {
  std::vector<float> audio;  ///< Trimmed audio (between start_sample and end_sample)
  int start_sample;          ///< First non-silent sample index in the original signal
  int end_sample;            ///< One past the last non-silent sample (exclusive)
};

/// @brief Trim leading and trailing silence from a mono signal.
/// @param x Input signal
/// @param n Length
/// @param top_db Signal below `top_db` below its peak RMS is considered silent.
/// @param frame_length Frame length for RMS computation
/// @param hop_length Hop length for RMS computation
/// @return TrimResult with the trimmed audio and the original-sample range.
///         An entirely silent input yields `{{}, 0, 0}` (empty audio, zero range).
/// @details Computes RMS per frame, finds the peak, and treats frames whose
///          RMS is below peak_dB - top_db as silent. Mirrors librosa.effects.trim.
/// @throw sonare::SonareException on empty input (`n == 0`), null input,
///        non-positive frame/hop, or a non-finite/non-positive `top_db`.
/// @note Empty input is rejected instead of returning `{{}, 0, 0}`, which is
///       reserved for the all-silent result; sharing one value between the two
///       would leave a caller unable to tell them apart.
TrimResult trim(const float* x, std::size_t n, float top_db = kDefaultSilenceTopDb,
                int frame_length = kDefaultSilenceFrameLength,
                int hop_length = kDefaultSilenceHopLength);
TrimResult trim(const std::vector<float>& x, float top_db = kDefaultSilenceTopDb,
                int frame_length = kDefaultSilenceFrameLength,
                int hop_length = kDefaultSilenceHopLength);

/// @brief Split a signal into non-silent intervals.
/// @return Vector of (start_sample, end_sample) pairs (end exclusive). Empty
///         if the entire signal is silent.
/// @details Same RMS-based silence detection as trim().
/// @throw sonare::SonareException on empty input (`n == 0`), null input,
///        non-positive frame/hop, or a non-finite/non-positive `top_db`.
/// @note Empty input is rejected for the same reason as in trim(): an empty
///       interval list already means "the whole signal is silent".
std::vector<std::pair<int, int>> split(const float* x, std::size_t n,
                                       float top_db = kDefaultSilenceTopDb,
                                       int frame_length = kDefaultSilenceFrameLength,
                                       int hop_length = kDefaultSilenceHopLength);
std::vector<std::pair<int, int>> split(const std::vector<float>& x,
                                       float top_db = kDefaultSilenceTopDb,
                                       int frame_length = kDefaultSilenceFrameLength,
                                       int hop_length = kDefaultSilenceHopLength);

/// @brief split() plus the threshold headroom the same pass already measured.
struct SplitReport {
  std::vector<std::pair<int, int>> intervals;  ///< Exactly what split() returns.
  /// @brief The largest @p top_db at which this signal still shows silence.
  /// @details The detector calls a frame silent when it sits at least @p top_db
  ///   under the signal's peak RMS, so the deepest dip decides whether ANY
  ///   threshold can find silence here. Above this figure the signal reads as
  ///   continuously sounding, which is why a caller holding one interval cannot
  ///   otherwise tell "no quiet moment exists" from "the threshold was set too
  ///   loose to see one". +inf when a frame is exactly 0 under a nonzero peak,
  ///   which every threshold calls silent; NaN (not measured) when the peak is 0.
  float silence_ceiling_db = kNotMeasured;
};

/// @brief split() with its report; split() is this without the second field.
/// @throw Identical to split() — the checks and their messages are shared.
SplitReport split_with_report(const float* x, std::size_t n, float top_db = kDefaultSilenceTopDb,
                              int frame_length = kDefaultSilenceFrameLength,
                              int hop_length = kDefaultSilenceHopLength);

/// @brief The union of several takes' sounding intervals, plus its report.
struct CommonSplitReport {
  /// @brief Intervals where ANY signal sounds, so every gap is silent in all.
  /// @details Touching counts as overlapping: two takes whose intervals meet
  ///   exactly leave no silent sample between them, so a cut there is mid-phrase.
  std::vector<std::pair<int, int>> intervals;
  /// @brief The smallest @ref SplitReport::silence_ceiling_db across the signals.
  /// @details The union needs every signal quiet at the same place, so the signal
  ///   with the least headroom is the binding constraint. A signal with no peak is
  ///   silent everywhere and constrains nothing; NaN when every signal is silent.
  float silence_ceiling_db = kNotMeasured;
  int max_signal_intervals = 0;  ///< Intervals the most fragmented signal produced alone.
  int min_signal_intervals = 0;  ///< Intervals the least fragmented signal produced alone.
};

/// @brief Runs split_with_report() per signal and merges the results.
/// @details Lives here rather than in each caller because the C ABI and the
///   WebAssembly binding both need the rule and had a copy each; a merge rule
///   held in two places is one place for them to disagree.
/// @param signals @p signal_count buffers, none null and none empty.
/// @throw sonare::SonareException for @p signal_count == 0, a null array, or
///        whatever split_with_report() refuses in any one signal.
CommonSplitReport split_common_with_report(const float* const* signals, const std::size_t* lengths,
                                           std::size_t signal_count,
                                           float top_db = kDefaultSilenceTopDb,
                                           int frame_length = kDefaultSilenceFrameLength,
                                           int hop_length = kDefaultSilenceHopLength);

}  // namespace sonare
