#pragma once

/// @file automation_lane.h
/// @brief Persistent PPQ breakpoint automation lane.

#include <cmath>
#include <cstdint>
#include <vector>

#include "automation/parameter.h"

namespace sonare::automation {

struct Breakpoint {
  double ppq = 0.0;
  float value = 0.0f;
  CurveType curve_to_next = CurveType::Linear;

  bool operator==(const Breakpoint& o) const noexcept {
    return ppq == o.ppq && value == o.value && curve_to_next == o.curve_to_next;
  }
  bool operator!=(const Breakpoint& o) const noexcept { return !(*this == o); }
};

inline bool valid_public_breakpoint(double ppq, float value, int curve_ordinal) noexcept {
  return std::isfinite(ppq) && std::isfinite(value) && curve_ordinal >= 0 && curve_ordinal <= 3;
}

inline bool valid_public_breakpoint(const Breakpoint& point) noexcept {
  return valid_public_breakpoint(point.ppq, point.value, static_cast<int>(point.curve_to_next));
}

class AutomationLane {
 public:
  AutomationLane() = default;
  explicit AutomationLane(uint32_t target_param_id);

  uint32_t target_param_id() const noexcept { return target_param_id_; }
  void set_target_param_id(uint32_t id) noexcept { target_param_id_ = id; }
  /// Sorts by ppq (stable) and drops duplicate-ppq points, keeping the FIRST
  /// occurrence in the supplied list. A lane therefore cannot represent an
  /// instantaneous jump as two points at the same ppq — place the second point
  /// an epsilon later (or use a Hold curve on the preceding segment) instead.
  void set_points(std::vector<Breakpoint> points);
  const std::vector<Breakpoint>& points() const noexcept { return points_; }

  float value_at(double ppq) const noexcept;
  double next_breakpoint_after(double ppq) const noexcept;

 private:
  uint32_t target_param_id_ = 0;
  std::vector<Breakpoint> points_;
};

}  // namespace sonare::automation
