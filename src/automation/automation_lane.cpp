#include "automation/automation_lane.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "mixing/stereo_width.h"
#include "util/exception.h"
#include "util/insertion_sort.h"

namespace sonare::automation {
namespace {

float interpolate(const Breakpoint& a, const Breakpoint& b, double ppq) noexcept {
  const double span = b.ppq - a.ppq;
  if (span <= 0.0) return b.value;
  const double t = std::clamp((ppq - a.ppq) / span, 0.0, 1.0);
  return static_cast<float>(::sonare::interpolate_curve(a.curve_to_next, a.value, b.value, t));
}

}  // namespace

AutomationLane::AutomationLane(uint32_t target_param_id, AutomationTargetKind target_kind)
    : AutomationLane(target_param_id, target_kind, {}) {}

AutomationLane::AutomationLane(uint32_t target_param_id, AutomationTargetKind target_kind,
                               AutomationTargetIdentity identity)
    : target_param_id_(target_param_id), target_kind_(target_kind), identity_(std::move(identity)) {
  SONARE_CHECK_MSG(valid_automation_target_kind(target_kind), ErrorCode::InvalidParameter,
                   "automation target kind is invalid");
  SONARE_CHECK_MSG(valid_target_identity(), ErrorCode::InvalidParameter,
                   "automation target identity does not match its kind");
}

bool AutomationLane::valid_target_identity() const noexcept {
  const bool has_insert = identity_.insert_index.has_value() || !identity_.processor.empty() ||
                          !identity_.param_name.empty();
  switch (target_kind_) {
    case AutomationTargetKind::kTrackSendDb:
      return !identity_.send_id.empty() && !has_insert;
    case AutomationTargetKind::kTrackInsertParam:
      return identity_.send_id.empty() && identity_.insert_index.has_value() &&
             !identity_.processor.empty() && !identity_.param_name.empty();
    case AutomationTargetKind::kOpaque:
    case AutomationTargetKind::kTrackFaderDb:
    case AutomationTargetKind::kTrackPan:
    case AutomationTargetKind::kTrackWidth:
      return identity_.send_id.empty() && !has_insert;
  }
  return false;
}

void AutomationLane::set_points(std::vector<Breakpoint> points) {
  for (const Breakpoint& point : points) {
    SONARE_CHECK_MSG(valid_public_breakpoint(point), ErrorCode::InvalidParameter,
                     "automation breakpoint values must be finite and curve must be valid");
    if (target_kind_ == AutomationTargetKind::kTrackWidth) {
      SONARE_CHECK_MSG(mixing::width_in_range(point.value), ErrorCode::InvalidParameter,
                       "track width breakpoint is outside its range");
    }
  }
  // A stable sort so the duplicate-ppq policy below ("first occurrence in the
  // supplied list wins") is deterministic; std::sort leaves the relative order
  // of equal-ppq points unspecified.
  insertion_sort(points.begin(), points.end(),
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
