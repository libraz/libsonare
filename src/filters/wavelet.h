#pragma once

/// @file wavelet.h
/// @brief Wavelet kernel and length helpers for the CQT / VQT.

#include <complex>
#include <vector>

#include "util/constants.h"
#include "util/parameter_checks.h"
#include "util/types.h"

namespace sonare {

namespace detail {

/// @brief Builds wavelet kernels with an explicitly selected periodic window.
/// @details Mirrors `librosa.filters.wavelet` sizing and normalization. By default returns the
/// concatenation of kernel arrays (lengths given by @ref wavelet_lengths). When @p pad_fft is true
/// the kernels are zero-padded (centered) to a common length `n_fft = next_pow2(max(lengths))` and
/// the result is a flat `[n_filters x n_fft]` row-major matrix; @p n_fft_out (if non-null)
/// receives the chosen `n_fft`. A positive @p Q overrides the default length rule so each kernel
/// length is `Q * sr / freq`.
std::vector<std::complex<float>> wavelet_with_window(const std::vector<float>& freqs, int sr,
                                                     float window_param, bool is_cqt, bool pad_fft,
                                                     float Q, WindowType window,
                                                     int* n_fft_out = nullptr);

/// @brief Computes librosa's relative bandwidth for each frequency bin.
/// @details The result includes the same floor used by wavelet lengths. A
/// single frequency uses the caller-compatible default alpha of 1.0.
std::vector<float> relative_bandwidths(const std::vector<float>& freqs);

/// @brief The relative bandwidth every bin of a geometric grid has at @p bins_per_octave.
/// @details Equal to relative_bandwidths over any two or more bins of that grid, so a lone bin
///          whose grid spacing is known takes it without a neighbour.
float geometric_relative_bandwidth(int bins_per_octave);

}  // namespace detail

/// @brief What the sample rate of wavelet_lengths accepts.
inline constexpr util::check::NumericDomain kWaveletSampleRateDomain =
    util::check::integer_domain_of<int>(1);

/// @brief Lengths of each wavelet kernel computed by detail::wavelet_with_window.
/// @details Returns the RAW fractional lengths to match librosa exactly (no
///          ceiling, no odd-length rounding); callers floor/ceil as needed.
///          The default rule is `window_param * sr / freq`.
/// @param Q If positive, overrides the default rule and uses `Q * sr / freq`
///          (still returned as a raw fractional value).
std::vector<float> wavelet_lengths(const std::vector<float>& freqs, int sr,
                                   float window_param = 1.0f, float Q = 0.0f);

}  // namespace sonare
