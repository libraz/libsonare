#pragma once

/// @file lpc.h
/// @brief Linear prediction (LPC) helpers — Burg / autocorrelation estimation,
/// residual (inverse) filtering, and two-sided AR gap interpolation. Used by
/// mastering repair processors (declick / declip) and the editing voice
/// changer (formant warp); placed under util/ so editing/ does not need to
/// reach into mastering/ for a generic DSP primitive.

#include <cstddef>
#include <vector>

#include "util/math_utils.h"

namespace sonare {

struct LpcResult {
  std::vector<float> ar;  // [1, a1, ..., ap] for e[n] = x[n] + sum ak*x[n-k]
  float variance = 0.0f;
  // Highest order the recursion completed; coefficients past it are zero.
  int order_reached = 0;
};

// Burg's method. The recursion ends at the last order whose reflection stays inside the unit
// circle, so a signal predictable at a lower order keeps a stable model padded with zeros.
LpcResult lpc_burg(const float* x, size_t n, int order);
LpcResult lpc_autocorrelation(const float* x, size_t n, int order);
// Same estimate written into a caller-owned result, so a per-frame analysis loop
// reuses one coefficient buffer instead of building a fresh vector each frame.
void lpc_autocorrelation(const float* x, size_t n, int order, LpcResult* out);

/// Autocorrelation plan and Levinson buffers a per-frame analysis loop reuses.
struct LpcWorkspace {
  AutocorrelationWorkspace autocorrelation;
  std::vector<float> lags;
  std::vector<double> r;
  std::vector<double> a;
  std::vector<double> next_a;
};

// Same estimate again, with the transform and recursion scratch also reused.
void lpc_autocorrelation(const float* x, size_t n, int order, LpcResult* out,
                         LpcWorkspace* workspace);
std::vector<float> lpc_residual(const float* x, size_t n, const LpcResult& model);
// Same residual written into a caller-owned buffer.
void lpc_residual(const float* x, size_t n, const LpcResult& model, std::vector<float>* out);

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
  size_t max_gap = 512;              ///< Longest gap the solver accepts.
  size_t max_context_radius = 4096;  ///< Longest one-sided context window.
  /// Above 0, each solved sample whose input sits at or past this magnitude keeps its input's
  /// sign and at least its input's magnitude: the consistency constraint of a clipped run.
  /// Samples inside it, and every sample at 0, are solved unconstrained.
  float min_magnitude = 0.0f;
};

/// @brief What @ref ar_interpolate_region did with a gap.
enum class ArFill {
  kFilled,      ///< The solution is written.
  kGapTooLong,  ///< The gap is past @c ArInterpolateParams::max_gap; nothing is written.
  kNotSolved,   ///< No usable solution: too little context or a failed or non-finite solve.
                ///  Nothing is written.
};

/// @brief Janssen (1986) two-sided AR interpolation of one contiguous gap.
///
/// Reference: A. J. E. M. Janssen, "Adaptive interpolation of discrete-time signals
/// that can be modeled as autoregressive processes," IEEE Trans. ASSP, vol. 34, no. 2,
/// pp. 317–330, Apr. 1986. DOI: 10.1109/TASSP.1986.1164824
///
/// Outline (cf. Section III): initialise the unknowns x_u by @ref interpolate_gap,
/// then for each outer round estimate AR(p) coefficients over a bounded window by
/// Burg's method (minimum-phase, stable), and solve
/// `(A_u^T A_u + lambda I) x_u = -A_u^T A_k y_k` in double by LDLT, where A is the
/// prediction-error filter split into unknown and known columns. A_u^T A_u is formed
/// directly from the banded filter, so the working set is O(gap^2) whatever the context.
/// Re-estimating from progressively better x_u converges in 2-5 rounds on music.
///
/// Solving for the whole gap against both edges at once is what separates this
/// from a forward AR extrapolation, which has no term anchoring it to the right
/// boundary and drifts away from it across a long gap.
///
/// With @c ArInterpolateParams::min_magnitude set, the original formulation's
/// clipping-consistency constraint applies as an active set: a sample whose solution falls
/// inside its input is pinned at its input and the rest are solved again;
/// the set is rebuilt each outer round.
/// Without it the solve is a general gap filler, for a spike that is not a clipping event.
ArFill ar_interpolate_region(float* samples, size_t n, size_t start, size_t end,
                             const ArInterpolateParams& params);

}  // namespace sonare
