#pragma once

/// @file weighting.h
/// @brief Perceptual frequency weighting curves (A/B/C/D) and spectrogram
///        weighting (librosa.A_weighting / frequency_weighting /
///        perceptual_weighting compatible).

#include <cstddef>
#include <string>
#include <vector>

namespace sonare {

/// @brief A-weighting (IEC 61672) for one or more frequencies in Hz.
/// @param freqs Frequencies in Hz.
/// @param min_db Clip weights below this value in dB (default -80 dB). Pass a
///        large negative value (e.g. -1e9) to effectively disable clipping.
/// @return Weights in dB, one per input frequency.
std::vector<float> A_weighting(const std::vector<float>& freqs, float min_db = -80.0f);

/// @brief B-weighting (IEC 61672) for one or more frequencies in Hz.
std::vector<float> B_weighting(const std::vector<float>& freqs, float min_db = -80.0f);

/// @brief C-weighting (IEC 61672) for one or more frequencies in Hz.
std::vector<float> C_weighting(const std::vector<float>& freqs, float min_db = -80.0f);

/// @brief D-weighting (IEC 61672) for one or more frequencies in Hz.
std::vector<float> D_weighting(const std::vector<float>& freqs, float min_db = -80.0f);

/// @brief Selects a weighting curve by kind string ("A", "B", "C", "D", or "Z").
/// @details "Z" returns zero weights (flat response).
std::vector<float> frequency_weighting(const std::vector<float>& freqs,
                                       const std::string& kind = "A", float min_db = -80.0f);

/// @brief Applies perceptual weighting to a power spectrogram.
/// @details Returns `frequency_weighting(freqs, kind) + power_to_db(S)` per
///          frequency bin. The output has the same shape as the input
///          spectrogram, row-major `[n_bins * n_frames]`.
/// @param S Power spectrogram (row-major, n_bins x n_frames).
/// @param n_bins Number of frequency bins (must equal freqs.size()).
/// @param n_frames Number of time frames.
/// @param freqs Center frequency for each bin.
/// @param kind Weighting curve kind (default "A").
/// @return Perceptually weighted spectrogram in dB.
std::vector<float> perceptual_weighting(const float* S, int n_bins, int n_frames,
                                        const std::vector<float>& freqs,
                                        const std::string& kind = "A");

}  // namespace sonare
