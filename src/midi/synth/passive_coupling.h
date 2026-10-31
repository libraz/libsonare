#pragma once

/// @file passive_coupling.h
/// @brief The valve-tube junction solved IMPLICITLY: the wave the valve launches
///        is part of the pressure the valve sees (McIntyre, Schumacher & Woodhouse 1983).
///
/// Per sample the one unknown is the drop. With `X` the drop over the pressure
/// scale and `u = sgn(X) sqrt|X|` the junction is `Y(u) = X + zeta * o(X) * u = T`,
/// `o` the valve's opening law and `T` the driving pressure, twice the returning
/// wave and the steady flow. The steady subtraction sits INSIDE the solve, so no
/// returning wave is a self-consistent fixed point. A junction reading the drop
/// off the returning wave alone has gain `1 - k` where this one has
/// `(1 - k) / (1 + k)`, which alone decides whether a valve near threshold oscillates.
///
/// Newton in `u`, warm-started, bracketed, stopped on the STEP `|du| <= kTol *
/// max(1, |u|)` with a hard cap -- not on the residual, which is exactly zero on
/// the shut branch after one step and small near `X = 0` while `u` still moves.
/// An opening fixed by the valve's state makes the map a quadratic, solved exactly.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace sonare::midi::synth {

/// A valve's opening for this sample as a function of the normalized drop `X`:
/// `o(X) = clamp(alpha - beta * X, 0, ceiling)`. A massless valve moves with the
/// drop (`beta != 0`); a valve with inertia has its opening fixed by its state
/// for the sample (`beta == 0`).
struct ValveLaw {
  float alpha = 1.0f;
  float beta = 0.0f;
  float ceiling = std::numeric_limits<float>::infinity();
};

/// What the solver did, accumulated over every solve() since reset_stats(). The
/// step at exit is the convergence instrument; a cap hit means the operating
/// point lies outside the clamped domain, not that the cap is too low.
struct CouplingStats {
  std::uint64_t solves = 0;
  std::uint64_t cap_hits = 0;
  std::uint64_t iterations = 0;
  int max_iterations = 0;
  float max_exit_step = 0.0f;
  float max_accepted_u = 0.0f;
  float max_warm_start_gap = 0.0f;
};

class PassiveCoupling {
 public:
  /// Hard cap on Newton steps. Nine suffice at zeta = 0.9; reaching the cap reads
  /// as a patch outside the clamp. A solver parameter, not a calibration constant.
  static constexpr int kNewtonCap = 16;
  /// Relative step tolerance: the loosest that still reaches float32's arithmetic floor.
  static constexpr float kTol = 3.0e-7f;
  /// Upper clamp on zeta. Uniqueness needs zeta < 1; a bounded iteration count
  /// needs distance from 1, where the map's minimum slope `min(zeta, 2 - 2 zeta)` collapses.
  static constexpr float kMaxZeta = 0.9f;

  struct Junction {
    float drop;       ///< Pressure across the valve, upstream minus downstream.
    float flow;       ///< Valve flow, `o * sgn(drop) * sqrt|drop|`.
    float injection;  ///< Pressure wave launched into the tube.
  };

  /// @param pressure_scale The valve's pressure scale (a reed's closing pressure); > 0.
  /// @param zeta           Dimensionless flow-to-pressure gain, clamped to [0, kMaxZeta].
  void configure(float pressure_scale, float zeta) noexcept {
    pressure_scale_ = std::max(pressure_scale, std::numeric_limits<float>::min());
    zeta_ = std::clamp(zeta, 0.0f, kMaxZeta);
    root_scale_ = std::sqrt(pressure_scale_);
    flow_gain_ = zeta_ * root_scale_;
    reset();
  }

  float zeta() const noexcept { return zeta_; }
  /// The flow-to-pressure gain in physical units, `zeta * sqrt(pressure_scale)`.
  float flow_gain() const noexcept { return flow_gain_; }

  /// @param upstream Driving pressure (mouth, lungs).
  /// @param incoming Pressure wave arriving back from the tube.
  /// @param steady   Flow subtracted from the injection, in the same units as the
  ///                 returned flow; a loop that sheds its DC passes the flow at rest.
  Junction solve(float upstream, float incoming, float steady, const ValveLaw& law) noexcept {
    const float target =
        (upstream - 2.0f * incoming) / pressure_scale_ + zeta_ * steady / root_scale_;
    // |Y(u)| >= u^2 brackets the root by sqrt|T|, which IS the root on the shut branch;
    // the slack keeps a Newton step that lands on it from reading as outside.
    const float bound = std::sqrt(std::fabs(target)) * (1.0f + kBracketSlack);
    float lo = target >= 0.0f ? 0.0f : -bound;
    float hi = target >= 0.0f ? bound : 0.0f;
    float u = std::clamp(u_prev_, lo, hi);
    float step = 0.0f;
    int iterations = 0;
    bool converged = lo == hi;
    if (law.beta == 0.0f && !converged) {
      // A fixed opening makes the map the quadratic |u| u + b u: exact, and Newton would
      // degrade to linear convergence at a shut valve, where the slope at u = 0 is b = 0.
      const float b = zeta_ * opening(0.0f, law);
      u = 2.0f * target / (b + std::sqrt(b * b + 4.0f * std::fabs(target)));
      converged = true;
    }
    while (!converged && iterations < kNewtonCap) {
      ++iterations;
      const float residual = map(u, zeta_, law) - target;
      if (residual > 0.0f) {
        hi = u;
      } else {
        lo = u;
      }
      const float slope_u = slope(u, zeta_, law);
      float next = slope_u > 0.0f ? u - residual / slope_u : 0.5f * (lo + hi);
      if (!(next >= lo && next <= hi)) next = 0.5f * (lo + hi);
      step = next - u;
      u = next;
      converged = std::fabs(step) <= kTol * std::max(1.0f, std::fabs(u));
    }
    record(converged, iterations, step, u);
    u_prev_ = u;
    const float x = u * std::fabs(u);
    const float flow = opening(x, law) * u * root_scale_;
    return {pressure_scale_ * x, flow, incoming + flow_gain_ * (flow - steady)};
  }

  /// The junction map `Y(u)` the solver inverts.
  static float map(float u, float zeta, const ValveLaw& law) noexcept {
    const float x = u * std::fabs(u);
    return x + zeta * opening(x, law) * u;
  }

  /// `dY/du`. Inside the opening's clamps the law contributes `-beta dX/du`.
  static float slope(float u, float zeta, const ValveLaw& law) noexcept {
    const float x = u * std::fabs(u);
    const float raw = law.alpha - law.beta * x;
    const bool moving = raw > 0.0f && raw < law.ceiling;
    const float two_abs_u = 2.0f * std::fabs(u);
    return two_abs_u + zeta * (opening(x, law) - (moving ? law.beta * u * two_abs_u : 0.0f));
  }

  static float opening(float x, const ValveLaw& law) noexcept {
    return std::clamp(law.alpha - law.beta * x, 0.0f, law.ceiling);
  }

  const CouplingStats& stats() const noexcept { return stats_; }
  void reset_stats() noexcept { stats_ = CouplingStats{}; }
  /// Clears the warm start; the stats are kept.
  void reset() noexcept { u_prev_ = 0.0f; }

 private:
  static constexpr float kBracketSlack = 1.0e-6f;

  void record(bool converged, int iterations, float step, float u) noexcept {
    ++stats_.solves;
    if (!converged) ++stats_.cap_hits;
    stats_.iterations += static_cast<std::uint64_t>(iterations);
    stats_.max_iterations = std::max(stats_.max_iterations, iterations);
    stats_.max_exit_step = std::max(stats_.max_exit_step, std::fabs(step));
    stats_.max_accepted_u = std::max(stats_.max_accepted_u, std::fabs(u));
    stats_.max_warm_start_gap = std::max(stats_.max_warm_start_gap, std::fabs(u - u_prev_));
  }

  float pressure_scale_ = 1.0f;
  float root_scale_ = 1.0f;
  float zeta_ = 0.0f;
  float flow_gain_ = 0.0f;
  float u_prev_ = 0.0f;
  CouplingStats stats_;
};

}  // namespace sonare::midi::synth
