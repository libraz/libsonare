#pragma once

/// @file harmonic.h
/// @brief Harmonic-aware spectral helpers (salience, interp_harmonics, f0_harmonics).

#include <vector>

namespace sonare {

/// @brief Computes the harmonic salience map.
/// @details For each bin frequency `freqs[k]` and harmonic `h`, the spectrum
/// is sampled at `h * freqs[k]` via linear interpolation, then a (per-default
/// uniform) weighted sum is taken across harmonics. Mirrors
/// `librosa.salience`.
/// @param S Input magnitude or power spectrum [n_bins x n_frames] row-major.
/// @param n_bins Number of bins (i.e. length of @p freqs).
/// @param n_frames Number of time frames.
/// @param freqs Bin center frequencies in Hz, length @p n_bins.
/// @param harmonics Harmonic multipliers (e.g. {1.0, 2.0, 3.0}).
/// @param fill_value Value to use when the harmonic lies outside @p freqs.
/// @param weights Optional per-harmonic weights (length must equal @p harmonics);
///        empty (the default) applies uniform weights, matching the prior
///        behaviour and `librosa.salience`'s `weights=None`. The weighted sum is
///        normalized by the sum of the weights.
/// @return Salience map [n_bins x n_frames] row-major.
std::vector<float> salience(const float* S, int n_bins, int n_frames,
                            const std::vector<float>& freqs, const std::vector<float>& harmonics,
                            float fill_value = 0.0f, const std::vector<float>& weights = {});

std::vector<float> salience(const std::vector<float>& S, int n_bins, int n_frames,
                            const std::vector<float>& freqs, const std::vector<float>& harmonics,
                            float fill_value = 0.0f, const std::vector<float>& weights = {});

/// @brief Linearly interpolates the spectrum at integer multiples of each
///        frequency in @p freqs.
/// @return [n_harmonics x n_bins x n_frames] flattened row-major.
std::vector<float> interp_harmonics(const float* x, int n_bins, int n_frames,
                                    const std::vector<float>& freqs,
                                    const std::vector<float>& harmonics);

std::vector<float> interp_harmonics(const std::vector<float>& x, int n_bins, int n_frames,
                                    const std::vector<float>& freqs,
                                    const std::vector<float>& harmonics);

/// @brief Extracts harmonic energy at multiples of @p f0 for each frame.
/// @return [n_harmonics x n_frames] row-major.
std::vector<float> f0_harmonics(const float* S, int n_bins, int n_frames,
                                const std::vector<float>& f0, const std::vector<float>& freqs,
                                const std::vector<float>& harmonics);

std::vector<float> f0_harmonics(const std::vector<float>& S, int n_bins, int n_frames,
                                const std::vector<float>& f0, const std::vector<float>& freqs,
                                const std::vector<float>& harmonics);

}  // namespace sonare
