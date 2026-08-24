#pragma once

/// @file parameter.h
/// @brief Non-RT parameter metadata registry for DAW automation UIs.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "util/automation_curve.h"

namespace sonare::automation {

/// Alias for the canonical curve enum. Spelled `CurveType` here for historical
/// reasons (the engine PPQ-domain automation API was the first consumer).
using CurveType = ::sonare::AutomationCurve;

/// @brief The id the engine reserves as "invalid / none".
///
/// AutomationEngine::bind_target and GraphRuntime::bind_parameter both refuse
/// it, and RealtimeEngine::start_smoothed_param routes it to the unbound-target
/// counter, so a parameter registered under this id could be listed but never
/// reached by set_parameter / bind_target.
inline constexpr uint32_t kInvalidParameterId = 0;

struct ParameterInfo {
  uint32_t id = 0;
  const char* name = "";
  const char* unit = "";
  // NOTE: min_value/max_value are descriptive UI metadata only. AutomationEngine
  // does NOT clamp applied values to this range — apply()/set_parameter() pass
  // the lane/host value straight to the processor (each processor clamps its own
  // parameters as needed). Callers must not assume the engine enforces [min,max].
  float min_value = 0.0f;
  float max_value = 1.0f;
  float default_value = 0.0f;
  bool rt_safe = true;
  CurveType default_curve = CurveType::Linear;
};

/// @brief Orders parameter metadata by id.
/// @details Shared rather than written as an equivalent lambda at each call
///          site: a lambda has its own closure type, so every site instantiates
///          std::sort's machinery again for the same element type.
inline bool parameter_info_id_before(const ParameterInfo& a, const ParameterInfo& b) {
  return a.id < b.id;
}

class ParameterRegistry {
 public:
  void clear();
  /// @brief Registers @p info.
  /// @return false when the id duplicates a registered one or is the reserved
  ///         kInvalidParameterId; the registry is unchanged in both cases.
  bool add(ParameterInfo info);
  size_t parameter_count() const noexcept { return parameters_.size(); }
  bool parameter_info(uint32_t id, ParameterInfo* out) const noexcept;
  bool parameter_info_by_index(size_t index, ParameterInfo* out) const noexcept;
  bool parameter_is_realtime_safe(uint32_t id) const noexcept;

 private:
  std::vector<ParameterInfo> parameters_;
};

}  // namespace sonare::automation
