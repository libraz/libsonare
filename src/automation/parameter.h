#pragma once

/// @file parameter.h
/// @brief Non-RT parameter metadata registry for DAW automation UIs.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sonare::automation {

enum class CurveType {
  kHold,
  kLinear,
  kExponential,
  kSCurve,
};

struct ParameterInfo {
  uint32_t id = 0;
  const char* name = "";
  const char* unit = "";
  float min_value = 0.0f;
  float max_value = 1.0f;
  float default_value = 0.0f;
  bool rt_safe = true;
  CurveType default_curve = CurveType::kLinear;
};

class ParameterRegistry {
 public:
  void clear();
  bool add(ParameterInfo info);
  size_t parameter_count() const noexcept { return parameters_.size(); }
  bool parameter_info(uint32_t id, ParameterInfo* out) const noexcept;
  bool parameter_info_by_index(size_t index, ParameterInfo* out) const noexcept;
  bool parameter_is_realtime_safe(uint32_t id) const noexcept;

 private:
  std::vector<ParameterInfo> parameters_;
};

}  // namespace sonare::automation
