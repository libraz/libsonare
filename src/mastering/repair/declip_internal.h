#pragma once

/// @file declip_internal.h
/// @brief The declipper's write-or-leave decision for one reconstructed run, declared so a
///        test can hand it a reconstruction no measured signal produces.

#include <cstddef>

#include "mastering/repair/declip.h"

namespace sonare::mastering::repair::detail {

/// @brief Keeps a reconstructed run or restores it from @p original and counts it.
/// @details A run whose peak passes @p clip_threshold + @ref kDeclipMaxReconstructionGainDb is
///   restored bit for bit and counted in @c DeclipReport::unreconstructed_runs; otherwise it is
///   counted in @c lpc_reconstructed_runs and @c repaired_samples.
/// @return Whether the reconstruction was kept.
bool keep_reconstruction(float* run, const float* original, size_t length, float clip_threshold,
                         DeclipReport& report);

}  // namespace sonare::mastering::repair::detail
