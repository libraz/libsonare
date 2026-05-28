#pragma once

/// @file lpc.h
/// @brief Linear prediction (LPC) helpers — Burg / autocorrelation estimation,
/// residual (inverse) filtering, and AR-based sample interpolation. Used by
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
std::vector<float> lpc_residual(const float* x, size_t n, const LpcResult& model);
std::vector<float> ar_interpolate(const float* x, const bool* mask, size_t n,
                                  const LpcResult& model);

}  // namespace sonare
