#pragma once

/// @file tonnetz.h
/// @brief Tonal centroid (Tonnetz) feature extraction.

#include <vector>

#include "feature/chroma.h"

namespace sonare {

/// @brief Computes the Tonnetz tonal centroid representation from a chromagram.
/// @details Implements `librosa.feature.tonnetz`. The Tonnetz projects each
/// 12-bin chroma column onto a 6-dimensional space describing harmonic
/// relations along fifths, minor thirds, and major thirds (Harte et al. 2006).
/// @param chroma Chromagram (chroma values are L1-normalized internally per frame)
/// @return Row-major matrix [6 x n_frames]
std::vector<float> tonnetz(const Chroma& chroma);

/// @brief Computes the Tonnetz representation from raw chromagram data.
/// @param chromagram Chromagram values [n_chroma x n_frames] (row-major).
///        Currently only n_chroma == 12 is supported (matches librosa).
/// @param n_chroma Number of chroma bins (must be 12).
/// @param n_frames Number of time frames.
/// @return Row-major matrix [6 x n_frames]
std::vector<float> tonnetz(const float* chromagram, int n_chroma, int n_frames);

}  // namespace sonare
