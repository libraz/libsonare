#pragma once

/// @file preemphasis.h
/// @brief First-order pre-emphasis and de-emphasis filters
///        (librosa.effects.preemphasis / deemphasis compatible).

#include <cstddef>
#include <optional>
#include <vector>

namespace sonare {

/// @brief First-order pre-emphasis high-pass filter.
/// @details Matches scipy.signal.lfilter([1, -coef], [1], x, zi=[zi]):
///          - y[0] = x[0] + zi
///          - y[n] = x[n] - coef * x[n-1]   (n >= 1)
///          When zi is unset, the librosa-compatible default is used:
///          zi = 2*x[0] - x[1] (matches `librosa.effects.preemphasis`).
/// @param x Input signal
/// @param n Length
/// @param coef Pre-emphasis coefficient (default 0.97)
/// @param zi Optional initial state. If unset, uses the librosa-compatible
///        reflect-padding default `2*x[0] - x[1]`.
/// @return Filtered signal of the same length.
/// @throw std::invalid_argument if x is null and n > 0.
std::vector<float> preemphasis(const float* x, std::size_t n, float coef = 0.97f,
                               std::optional<float> zi = std::nullopt);
std::vector<float> preemphasis(const std::vector<float>& x, float coef = 0.97f,
                               std::optional<float> zi = std::nullopt);

/// @brief Inverse of preemphasis (lossy integrator).
/// @details Matches scipy.signal.lfilter([1], [1, -coef], x, zi=[zi]):
///          - y[0] = x[0] + zi
///          - y[n] = x[n] + coef * y[n-1]   (n >= 1)
///          When zi is unset, defaults to 2*x[0] - x[1] (librosa parity).
std::vector<float> deemphasis(const float* x, std::size_t n, float coef = 0.97f,
                              std::optional<float> zi = std::nullopt);
std::vector<float> deemphasis(const std::vector<float>& x, float coef = 0.97f,
                              std::optional<float> zi = std::nullopt);

}  // namespace sonare
