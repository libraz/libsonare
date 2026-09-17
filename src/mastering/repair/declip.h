#pragma once

#include <cstddef>

#include "core/audio.h"

namespace sonare::mastering::repair {

/// @brief Longest clipped run reconstructed with the LPC (Janssen) solver.
/// @details The solver builds dense matrices of (context x gap) and (gap x gap),
/// so an uncapped run makes both peak memory and compute unbounded in the input
/// length. Runs longer than this are filled with the cubic / linear interpolation
/// fallback instead. An AR(p) model only carries information about p samples past
/// each known edge, so beyond this length the LPC estimate has already decayed
/// into the regularized baseline. 512 samples is ~10.7 ms at 48 kHz, which covers
/// hard clipping down to the deepest bass fundamentals.
inline constexpr size_t kDeclipMaxLpcGapSamples = 512;

/// @brief Longest one-sided context window handed to the LPC solver.
/// @details Bounds the context even when @c DeclipConfig::lpc_order is large;
/// without it the order-derived term could pull the whole input into the solver.
/// Never binds for a gap-derived context, which is already at most
/// 8 * @ref kDeclipMaxLpcGapSamples.
inline constexpr size_t kDeclipMaxLpcContextRadius = 8 * kDeclipMaxLpcGapSamples;

/// @brief Upper bound on the LPC solver's dense working set, in bytes.
/// @details Derived from the two caps above: the (context x gap) prediction-error
/// matrix plus the (gap x gap) normal-equation matrix and its factorization. This
/// is the whole point of the caps — the bound is a constant, independent of the
/// input length and of the clip pattern.
inline constexpr size_t kDeclipMaxLpcWorkingSetBytes =
    sizeof(float) * kDeclipMaxLpcGapSamples *
    (2 * kDeclipMaxLpcContextRadius + 3 * kDeclipMaxLpcGapSamples);

/// @brief Shortest run of bit-identical samples counted as a flat top.
/// @details Two is reachable by rounding alone. The shallowest clipped run in the
/// evaluation corpus is four samples, so three separates with margin either way.
inline constexpr size_t kDeclipMinFlatRunSamples = 3;

/// @brief How far under the peak a flat run may sit and still count, in dB.
/// @details A clipper pins samples at one ceiling, so the runs that matter sit at
/// the peak and this window only admits the rounding spread around it. Corpus
/// separation is unchanged anywhere between 0.1 and 6 dB.
inline constexpr float kDeclipFlatRunPeakWindowDb = 1.0f;

struct DeclipConfig {
  float clip_threshold = 0.98f;
  int lpc_order = 36;
  int iterations = 2;
  // Blend weight for the LPC prediction; the interpolation fallback gets (1 - lpc_blend).
  float lpc_blend = 0.65f;
};

/// Validates every public DeclipConfig field. The mono, stereo and detection
/// entrypoints share this oracle so range handling cannot drift between them.
void validate_config(const DeclipConfig& config);

/// @brief What a declip analysis found.
struct ClipDetection {
  size_t sample_count = 0;       ///< Samples at or past clip_threshold.
  float sample_fraction = 0.0f;  ///< sample_count / size.
  size_t run_count = 0;
  size_t longest_run_samples = 0;  ///< Compare against kDeclipMaxLpcGapSamples:
                                   ///  a longer run takes the interpolation
                                   ///  fallback rather than the LPC solver.

  /// @name Flat-top analysis
  /// Runs of bit-identical samples sitting at the signal's peak. The fields above
  /// count what is at or past clip_threshold now, so they see only clipping that
  /// still reaches that ceiling; a flat top survives a later gain change and so
  /// reports material clipped before it was attenuated. Neither drives the
  /// repair, which reconstructs what crosses clip_threshold. A waveform that is
  /// genuinely flat on top -- a square or pulse train, a fully limited master --
  /// counts here and cannot be told from clipping in the time domain. The reverse
  /// error is the one to state: anything that moves samples independently erases
  /// a real flat top, so zero is not proof the material was never clipped.
  /// Resampling and lossy coding do it, and so does a stereo downmix -- measured,
  /// a shallow clipped tone reports 440 runs per channel and none at all after
  /// the two are averaged.
  /// @{
  size_t flat_run_count = 0;
  size_t longest_flat_run_samples = 0;
  size_t flat_sample_count = 0;
  float flat_level = 0.0f;  ///< Magnitude the counted runs sit at; 0 when none.
  /// @}
};

ClipDetection detect_clipping(const float* samples, size_t size, int sample_rate,
                              const DeclipConfig& config = {});

/// @brief What a declip pass found in one channel and what it did to it.
struct DeclipReport {
  ClipDetection detected;             ///< This channel's own analysis of the input.
  size_t lpc_reconstructed_runs = 0;  ///< Runs the Janssen solver filled.
  size_t interpolated_runs = 0;       ///< Runs past the LPC gap cap, filled by
                                      ///  interpolation instead: for these,
                                      ///  lpc_order / iterations / lpc_blend had
                                      ///  no effect.
  size_t repaired_samples = 0;        ///< Samples overwritten by either fill.
  size_t linked_runs = 0;             ///< Of the repaired runs, those reaching
                                      ///  past this channel's own clipped
                                      ///  samples because the other channel's
                                      ///  run was wider. Always 0 from the mono
                                      ///  entrypoint.
};

Audio declip(const Audio& audio, const DeclipConfig& config = {});

/// @brief Declips @p audio and reports what the pass found and did.
Audio declip(const Audio& audio, const DeclipConfig& config, DeclipReport* report);

/// @brief A declipped stereo pair and what each channel's pass did.
struct DeclipStereoResult {
  Audio left;
  Audio right;
  DeclipReport left_report;
  DeclipReport right_report;
};

/// @brief Declips a stereo pair, taking the reconstructed regions from both
///        channels.
/// @details One clipped plateau rarely ends on the same sample in both
///   channels, and a run that a single sample splits in one channel but not the
///   other reconstructs differently on the two sides, which moves the image.
///   The union of the two channels' clipped runs is therefore the region set,
///   and each channel reconstructs the whole of every union run it has at least
///   one clipped sample in. A channel with none is left untouched there:
///   reconstructing unclipped audio to match the other side would replace real
///   samples with an estimate.
DeclipStereoResult declip_stereo(const Audio& left, const Audio& right,
                                 const DeclipConfig& config = {});

}  // namespace sonare::mastering::repair
