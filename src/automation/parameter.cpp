#include "automation/parameter.h"

#include <algorithm>

namespace sonare::automation {

void ParameterRegistry::clear() { parameters_.clear(); }

bool ParameterRegistry::add(ParameterInfo info) {
  // Refuse the reserved id here, where every surface's add path converges, so a
  // registration that could never be bound or set fails at registration instead
  // of succeeding and reporting the parameter as available.
  if (info.id == kInvalidParameterId) {
    return false;
  }
  // parameters_ stays sorted by id, so the insertion point is also the duplicate check.
  const auto pos =
      std::lower_bound(parameters_.begin(), parameters_.end(), info, parameter_info_id_before);
  if (pos != parameters_.end() && pos->id == info.id) {
    return false;
  }
  parameters_.insert(pos, info);
  return true;
}

bool ParameterRegistry::parameter_info(uint32_t id, ParameterInfo* out) const noexcept {
  ParameterInfo key{};
  key.id = id;
  const auto pos =
      std::lower_bound(parameters_.begin(), parameters_.end(), key, parameter_info_id_before);
  if (pos == parameters_.end() || pos->id != id) return false;
  if (out) *out = *pos;
  return true;
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
