#include "automation/automation_lane.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sonare::automation {
namespace {

float interpolate(const Breakpoint& a, const Breakpoint& b, double ppq) noexcept {
  const double span = b.ppq - a.ppq;
  if (span <= 0.0) return b.value;
  double t = std::clamp((ppq - a.ppq) / span, 0.0, 1.0);
  switch (a.curve_to_next) {
    case CurveType::Hold:
      return a.value;
    case CurveType::Exponential: {
      // Interpolate in a signed, epsilon-shifted log domain so the curve stays
      // exponential even when an endpoint touches or crosses zero. Plain
      // log-interpolation is undefined for non-positive values; mixing in a
      // linear fallback there would emit discontinuous segment shapes.
      constexpr double kExpEpsilon = 1.0e-9;
      const double sa = a.value < 0.0 ? -1.0 : 1.0;
      const double sb = b.value < 0.0 ? -1.0 : 1.0;
      if (sa == sb) {
        const double la = std::log(std::abs(a.value) + kExpEpsilon);
        const double lb = std::log(std::abs(b.value) + kExpEpsilon);
        return static_cast<float>(sa * (std::exp(la + (lb - la) * t) - kExpEpsilon));
      }
      // Endpoints straddle zero: a single signed-log segment is ill-defined, so
      // fall back to linear interpolation for this segment only.
      return static_cast<float>(a.value + (b.value - a.value) * t);
    }
    case CurveType::SCurve:
      t = t * t * (3.0 - 2.0 * t);
      return static_cast<float>(a.value + (b.value - a.value) * t);
    case CurveType::Linear:
    default:
      return static_cast<float>(a.value + (b.value - a.value) * t);
  }
}

}  // namespace

AutomationLane::AutomationLane(uint32_t target_param_id) : target_param_id_(target_param_id) {}

void AutomationLane::set_points(std::vector<Breakpoint> points) {
  std::sort(points.begin(), points.end(),
            [](const Breakpoint& a, const Breakpoint& b) { return a.ppq < b.ppq; });
  points.erase(std::unique(points.begin(), points.end(),
                           [](const Breakpoint& a, const Breakpoint& b) { return a.ppq == b.ppq; }),
               points.end());
  points_ = std::move(points);
}

float AutomationLane::value_at(double ppq) const noexcept {
  if (points_.empty()) return 0.0f;
  if (ppq <= points_.front().ppq) return points_.front().value;
  if (ppq >= points_.back().ppq) return points_.back().value;

  size_t lo = 0;
  size_t hi = points_.size() - 1;
  while (lo + 1 < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (points_[mid].ppq <= ppq) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return interpolate(points_[lo], points_[hi], ppq);
}

double AutomationLane::next_breakpoint_after(double ppq) const noexcept {
  // points_ is kept sorted by ppq in set_points(); binary-search the first
  // breakpoint strictly after ppq so dense lanes stay O(log N) per query.
  const auto it =
      std::upper_bound(points_.begin(), points_.end(), ppq,
                       [](double value, const Breakpoint& point) { return value < point.ppq; });
  if (it == points_.end()) return std::numeric_limits<double>::infinity();
  return it->ppq;
}

}  // namespace sonare::automation
