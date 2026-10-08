#pragma once

#include <cstddef>
#include <vector>

#include "core/audio.h"

namespace sonare::mastering::repair {

/// @brief Largest accepted @c DeclickConfig::max_click_samples.
/// @details The longest gap the AR solver reconstructs; a longer run takes the linear fill, so
/// a larger cap only widens the runs repaired by interpolation. The shipped presets use at most 16.
inline constexpr size_t kDeclickMaxClickSamples = 512;

/// @brief Largest accepted @c DeclickConfig::threshold, a sample magnitude.
/// @details Full scale is 1; float audio can carry overs, and the request-object tests run it at 3.
inline constexpr float kDeclickMaxThreshold = 10.0f;

/// @brief Largest accepted @c DeclickConfig::neighbor_ratio; the corpus sweeps run 2 to 50.
inline constexpr float kDeclickMaxNeighborRatio = 100.0f;

/// @brief Largest accepted @c DeclickConfig::residual_ratio.
/// @details Above this the residual criterion selects nothing and only the threshold mask remains.
inline constexpr float kDeclickMaxResidualRatio = 1000.0f;

/// @brief Longest one-sided context window handed to the AR click fill.
/// @details Bounds the context even when @c DeclickConfig::lpc_order is large.
inline constexpr size_t kDeclickMaxArContextRadius = 8 * kDeclickMaxClickSamples;

/// @brief Largest accepted @c DeclickConfig::lpc_order.
/// @details The AR fill lowers the order to a quarter of its context, which spans at most two
/// capped radii plus the gap, so this is the largest order it runs as requested at every gap.
inline constexpr int kDeclickMaxLpcOrder = static_cast<int>(2 * kDeclickMaxArContextRadius / 4);

struct DeclickConfig {
  float threshold = 0.8f;
  float neighbor_ratio = 4.0f;
  size_t max_click_samples = 8;
  int lpc_order = 20;
  float residual_ratio = 8.0f;
};

/// Validates every public DeclickConfig field. The mono, stereo and detection
/// entrypoints share this oracle so range handling cannot drift between them.
void validate_config(const DeclickConfig& config);

/// @brief What a declick analysis found. Counts runs, not samples.
struct ClickDetection {
  size_t count = 0;                ///< Runs meeting the repair criteria.
  size_t rejected = 0;             ///< Outlier runs the criteria excluded. A
                                   ///  large value says max_click_samples or
                                   ///  neighbor_ratio is too tight for this
                                   ///  material, not that the material is clean.
  size_t longest_run_samples = 0;  ///< Over the counted runs.
  float per_second = 0.0f;         ///< count divided by the input duration.
};

/// @brief Measures clicks without repairing. Runs the same LPC analysis the
///        repair runs: a cheaper threshold-only detector would report runs the
///        repair does not act on.
ClickDetection detect_clicks(const float* samples, size_t size, int sample_rate,
                             const DeclickConfig& config = {});

/// @brief What a declick pass found in one channel and what it did to it.
struct DeclickReport {
  ClickDetection detected;      ///< This channel's own analysis of the input.
  size_t repaired_runs = 0;     ///< Runs interpolated. Larger than detected.count
                                ///  only under linked stereo detection.
  size_t repaired_samples = 0;  ///< Samples overwritten by interpolation.
  size_t linked_runs = 0;       ///< Of repaired_runs, those whose extent this
                                ///  channel's own detection did not produce.
                                ///  Always 0 from the mono entrypoint.
  bool lpc_model_used = false;  ///< False when the input was too short for
                                ///  lpc_order: detection then reduces to the
                                ///  threshold mask and every fill is linear.
                                ///  True says the AR path was viable, not that
                                ///  every fill took it -- the two-sided solver
                                ///  re-estimates its own model over a bounded
                                ///  window per run and declines a run whose gap
                                ///  or context it cannot work with.
};

Audio declick(const Audio& audio, const DeclickConfig& config = {});

/// @brief Declicks @p audio and reports what the pass found and did.
Audio declick(const Audio& audio, const DeclickConfig& config, DeclickReport* report);

/// @brief A declicked stereo pair and what each channel's pass did.
struct DeclickStereoResult {
  Audio left;
  Audio right;
  DeclickReport left_report;
  DeclickReport right_report;
};

/// @brief Declicks a stereo pair, choosing the repaired regions from both
///        channels.
/// @details A common-mode click repaired on one side only moves the image, so
///   the union of the two channels' selected runs is repaired in both. Only the
///   selection is shared: each channel's fill is computed from its own samples
///   and its own AR model. Overlapping runs from the two channels merge, which
///   can leave a repaired region longer than @c max_click_samples -- the cap
///   governs what may be selected, not how far a selection reaches once both
///   channels agree a click is there.
DeclickStereoResult declick_stereo(const Audio& left, const Audio& right,
                                   const DeclickConfig& config = {});

/// @brief Declicks any number of channels, repairing the union of every channel's selected runs.
/// @details The N-channel form of @ref declick_stereo with the same guarantee over the whole set.
///   One channel reproduces @ref declick and two reproduce @ref declick_stereo bit for bit.
/// @param out Receives one declicked channel per input channel, in input order.
/// @return One report per channel, in input order.
std::vector<DeclickReport> declick_linked(const Audio* const* channels, size_t channel_count,
                                          std::vector<Audio>* out, const DeclickConfig& config);

}  // namespace sonare::mastering::repair
