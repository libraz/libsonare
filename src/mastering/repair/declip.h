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

struct DeclipConfig {
  float clip_threshold = 0.98f;
  int lpc_order = 36;
  int iterations = 2;
  // Blend weight for the LPC prediction; the interpolation fallback gets (1 - lpc_blend).
  float lpc_blend = 0.65f;
};

Audio declip(const Audio& audio, const DeclipConfig& config = {});

}  // namespace sonare::mastering::repair
