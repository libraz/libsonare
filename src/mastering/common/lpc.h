#pragma once

/// @file lpc.h
/// @brief Linear prediction helpers for repair processors.

#include <cstddef>
#include <vector>

namespace sonare::mastering::common {

struct LpcResult {
  std::vector<float> ar;  // [1, a1, ..., ap] for e[n] = x[n] + sum ak*x[n-k]
  float variance = 0.0f;
};

LpcResult lpc_burg(const float* x, size_t n, int order);
LpcResult lpc_autocorrelation(const float* x, size_t n, int order);
std::vector<float> lpc_residual(const float* x, size_t n, const LpcResult& model);
std::vector<float> ar_interpolate(const float* x, const bool* mask, size_t n,
                                  const LpcResult& model);

}  // namespace sonare::mastering::common
