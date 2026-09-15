#pragma once

#include <cstddef>

#include "core/audio.h"

namespace sonare::mastering::repair {

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

}  // namespace sonare::mastering::repair
