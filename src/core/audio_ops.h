#pragma once

/// @file audio_ops.h
/// @brief Time-domain audio operations: mu-law companding, autocorrelation,
///        and linear prediction coefficients (LPC via Burg's method).

#include <cstddef>
#include <vector>

namespace sonare {

/// @brief mu-law compression: `sign(x) * log(1 + mu*|x|) / log(1 + mu)`.
/// @details Mirrors `librosa.mu_compress`. When `quantize` is true, the output
///          is quantized into `1 + mu` discrete integer levels (cast back to
///          float). Input must be in `[-1, 1]`.
/// @throw std::invalid_argument if x is null with n > 0, mu <= 0, or values
///        outside [-1, 1].
std::vector<float> mu_compress(const float* x, std::size_t n, int mu = 255, bool quantize = false);
std::vector<float> mu_compress(const std::vector<float>& x, int mu = 255, bool quantize = false);

/// @brief Inverse of mu_compress.
/// @details `sign(x) / mu * ((1 + mu)^|x| - 1)`. When `quantize` is true the
///          input is first scaled `x * 2 / (1 + mu)`, matching librosa's
///          `mu_expand(quantize=True)`.
std::vector<float> mu_expand(const float* x, std::size_t n, int mu = 255, bool quantize = false);
std::vector<float> mu_expand(const std::vector<float>& x, int mu = 255, bool quantize = false);

/// @brief Bounded-lag autocorrelation of a signal.
/// @details FFT-based; equivalent to `librosa.autocorrelate`. The first
///          `min(max_size, n)` lags are returned. `max_size == 0` means "no
///          truncation" (use the full signal length).
std::vector<float> autocorrelate(const float* y, std::size_t n, int max_size = 0);
std::vector<float> autocorrelate(const std::vector<float>& y, int max_size = 0);

/// @brief Linear prediction coefficients via Burg's method.
/// @details Returns the AR filter denominator polynomial `[1, a_1, ..., a_order]`
///          (length `order + 1`). Matches `librosa.lpc`.
/// @throw std::invalid_argument if order < 1 or y is too short.
std::vector<float> lpc(const float* y, std::size_t n, int order);
std::vector<float> lpc(const std::vector<float>& y, int order);

}  // namespace sonare
