#pragma once

/// @file seam_crossfade.h
/// @brief Cross-fade weights that keep a seam's level whatever the two sides' correlation.
///
/// An equal-power fade holds the level of independent sides and overshoots
/// coherent ones by up to +3 dB; an equal-gain fade does the reverse. These
/// weights are solved from the measured correlation rho of the two sides, so
/// g_in^2 + g_out^2 + 2 rho g_in g_out = 1 at every sample: equal gain at
/// rho = 1, the sin/cos equal-power pair at rho = 0. An anti-correlated pair is
/// faded as an independent one, since holding its level would boost both sides.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "util/constants.h"

namespace sonare::editing::note_model {

/// @brief Normalized correlation of @p a and @p b over @p n samples, weighted
///        toward the middle where a fade's cross term peaks; 0 if either is silent.
inline float seam_correlation(const float* a, const float* b, int64_t n) noexcept {
  double ab = 0.0;
  double aa = 0.0;
  double bb = 0.0;
  for (int64_t k = 0; k < n; ++k) {
    const double w =
        std::sin(sonare::constants::kPiD * (static_cast<double>(k) + 0.5) / static_cast<double>(n));
    ab += w * a[k] * b[k];
    aa += w * a[k] * a[k];
    bb += w * b[k] * b[k];
  }
  const double norm = std::sqrt(aa * bb);
  return norm > 0.0 ? static_cast<float>(ab / norm) : 0.0f;
}

/// @brief Gains of a level-keeping fade.
struct SeamGains {
  float outgoing;
  float incoming;
};

/// @brief Fade gains at @p phase in (0, 1) for sides of correlation @p rho.
inline SeamGains seam_gains(float rho, float phase) noexcept {
  const float r = std::clamp(rho, 0.0f, 1.0f);
  // Odd part o carries the fade; even part e solves the constant-level equation.
  const float o = sonare::constants::kInvSqrt2 *
                  std::sin(0.25f * sonare::constants::kPi * (2.0f * phase - 1.0f));
  const float e = std::sqrt(std::max(0.0f, (0.5f - (1.0f - r) * o * o) / (1.0f + r)));
  return {e - o, e + o};
}

/// @brief Cross-fades @p outgoing into @p incoming over @p n samples into @p dest.
/// @details @p dest may alias either side, since each sample reads both before writing.
inline void correlated_crossfade(float* dest, const float* outgoing, const float* incoming,
                                 int64_t n) noexcept {
  const float rho = seam_correlation(outgoing, incoming, n);
  for (int64_t k = 0; k < n; ++k) {
    const SeamGains g = seam_gains(rho, (static_cast<float>(k) + 0.5f) / static_cast<float>(n));
    dest[k] = g.outgoing * outgoing[k] + g.incoming * incoming[k];
  }
}

}  // namespace sonare::editing::note_model
