#pragma once

/// @file lpc.h
/// @brief Linear prediction (LPC) helpers — Burg / autocorrelation estimation,
/// residual (inverse) filtering, and two-sided AR gap interpolation. Used by
/// mastering repair processors (declick / declip) and the editing voice
/// changer (formant warp); placed under util/ so editing/ does not need to
/// reach into mastering/ for a generic DSP primitive.

#include <cstddef>
#include <vector>

namespace sonare {

struct LpcResult {
  std::vector<float> ar;  // [1, a1, ..., ap] for e[n] = x[n] + sum ak*x[n-k]
  float variance = 0.0f;
};

LpcResult lpc_burg(const float* x, size_t n, int order);
LpcResult lpc_autocorrelation(const float* x, size_t n, int order);
// Same estimate written into a caller-owned result, so a per-frame analysis loop
// reuses one coefficient buffer instead of building a fresh vector each frame.
void lpc_autocorrelation(const float* x, size_t n, int order, LpcResult* out);
std::vector<float> lpc_residual(const float* x, size_t n, const LpcResult& model);

/// @brief Fills [@p start, @p end) by cubic Hermite interpolation where four
///        surrounding samples exist, linear otherwise.
/// @details Reads only samples outside the gap, so writing in place is safe and
/// every filled position sees the same endpoints. This is both the baseline
/// @ref ar_interpolate_region initialises its unknowns with and the fill a
/// caller applies to a gap past @c ArInterpolateParams::max_gap.
void interpolate_gap(float* samples, size_t n, size_t start, size_t end);

/// @brief Knobs of @ref ar_interpolate_region. The defaults are the solver's own
///        working values, not a processor's: each caller passes its own caps.
struct ArInterpolateParams {
  int order = 32;                    ///< Requested AR order; lowered when the context is short.
  int iterations = 2;                ///< Outer estimate-and-solve rounds.
  float blend = 1.0f;                ///< Weight of the AR estimate against the
                                     ///  interpolation baseline, clamped to [0, 1].
                                     ///  0 leaves the baseline untouched.
  size_t max_gap = 512;              ///< Longest gap the solver accepts.
  size_t max_context_radius = 4096;  ///< Longest one-sided context window.
};

/// @brief Janssen (1986) two-sided AR interpolation of one contiguous gap.
///
/// Reference: A. J. E. M. Janssen, "Adaptive interpolation of discrete-time signals
/// that can be modeled as autoregressive processes," IEEE Trans. ASSP, vol. 34, no. 2,
/// pp. 317–330, Apr. 1986. DOI: 10.1109/TASSP.1986.1164824
///
/// Outline (cf. Section III): initialise the unknowns x_u by @ref interpolate_gap,
/// then for each outer round estimate AR(p) coefficients over a bounded window by
/// Burg's method (minimum-phase, stable), build the prediction-error filter matrix
/// A, partition it into unknown and known columns, and solve
/// `(A_u^T A_u + lambda I) x_u = -A_u^T A_k y_k` by LDLT — the Tikhonov term
/// covering a gap wider than the model's effective rank. Re-estimating from
/// progressively better x_u converges in 2-5 rounds on music.
///
/// Solving for the whole gap against both edges at once is what separates this
/// from a forward AR extrapolation, which has no term anchoring it to the right
/// boundary and drifts away from it across a long gap.
///
/// The original formulation's clipping-consistency constraint
/// (|x_u[i]| >= clip_threshold) is omitted so this stays a general gap filler: a
/// spike that exceeds a clipping threshold without being a clipping event should
/// be interpolated smoothly, not anchored at it.
///
/// @return false when the gap is longer than @c params.max_gap, in which case
///   nothing is written and the caller applies its own fallback. Both dense
///   matrices are sized from the gap, so the cap is what keeps peak memory and
///   per-gap compute a function of the caps rather than of the input.
bool ar_interpolate_region(float* samples, size_t n, size_t start, size_t end,
                           const ArInterpolateParams& params);

}  // namespace sonare
