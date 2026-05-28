#pragma once

/// @file stereo_metrics.h
/// @brief Pure scalar stereo metrics shared between mastering and metering layers.
/// @details These helpers compute layer-agnostic stereo statistics (correlation
///          coefficient and side/mid energy ratio). They are placed in `util/`
///          so that both `metering/` (full meters) and `mastering/` (mono-compat
///          checks, stereo processors) can reuse them without crossing layer
///          boundaries.

#include <cstddef>

namespace sonare::util {

/// @brief Sample-mean-free correlation coefficient between two equal-length
///        channels (a.k.a. cosine similarity, un-centered Pearson).
/// @param left  Pointer to the left channel buffer (length samples).
/// @param right Pointer to the right channel buffer (length samples).
/// @param length Number of samples per channel; must be > 0.
/// @return Correlation in [-1, 1], or 0.0f if either channel has zero energy.
/// @throws SonareException(InvalidParameter) if buffers are null or length is 0.
float stereo_correlation(const float* left, const float* right, size_t length);

/// @brief Stereo width measured as sqrt(side_energy / mid_energy) in M/S domain
///        with the orthonormal 1/sqrt(2) M/S transform.
/// @param left  Pointer to the left channel buffer (length samples).
/// @param right Pointer to the right channel buffer (length samples).
/// @param length Number of samples per channel; must be > 0.
/// @return Width in [0, inf). Returns 0 if both mid and side are silent; returns
///         +inf if mid is silent but side has energy.
/// @throws SonareException(InvalidParameter) if buffers are null or length is 0.
float stereo_width(const float* left, const float* right, size_t length);

}  // namespace sonare::util
