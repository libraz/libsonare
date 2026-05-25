#pragma once

/// @file automation_lane.h
/// @brief Persistent PPQ breakpoint automation lane.

#include <cstdint>
#include <vector>

#include "automation/parameter.h"

namespace sonare::automation {

struct Breakpoint {
  double ppq = 0.0;
  float value = 0.0f;
  CurveType curve_to_next = CurveType::kLinear;
};

class AutomationLane {
 public:
  AutomationLane() = default;
  explicit AutomationLane(uint32_t target_param_id);

  uint32_t target_param_id() const noexcept { return target_param_id_; }
  void set_target_param_id(uint32_t id) noexcept { target_param_id_ = id; }
  void set_points(std::vector<Breakpoint> points);
  const std::vector<Breakpoint>& points() const noexcept { return points_; }

  float value_at(double ppq) const noexcept;
  double next_breakpoint_after(double ppq) const noexcept;

 private:
  uint32_t target_param_id_ = 0;
  std::vector<Breakpoint> points_;
};

}  // namespace sonare::automation
