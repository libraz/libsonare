#include "automation/parameter.h"

#include <algorithm>

namespace sonare::automation {

void ParameterRegistry::clear() { parameters_.clear(); }

bool ParameterRegistry::add(ParameterInfo info) {
  const auto found = std::find_if(parameters_.begin(), parameters_.end(),
                                  [&](const ParameterInfo& item) { return item.id == info.id; });
  if (found != parameters_.end()) {
    *found = info;
    return false;
  }
  parameters_.push_back(info);
  std::sort(parameters_.begin(), parameters_.end(),
            [](const ParameterInfo& a, const ParameterInfo& b) { return a.id < b.id; });
  return true;
}

bool ParameterRegistry::parameter_info(uint32_t id, ParameterInfo* out) const noexcept {
  for (const ParameterInfo& info : parameters_) {
    if (info.id == id) {
      if (out) *out = info;
      return true;
    }
  }
  return false;
}

bool ParameterRegistry::parameter_info_by_index(size_t index, ParameterInfo* out) const noexcept {
  if (index >= parameters_.size()) return false;
  if (out) *out = parameters_[index];
  return true;
}

bool ParameterRegistry::parameter_is_realtime_safe(uint32_t id) const noexcept {
  ParameterInfo info{};
  return parameter_info(id, &info) && info.rt_safe;
}

}  // namespace sonare::automation
