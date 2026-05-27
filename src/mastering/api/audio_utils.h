#pragma once

#include <cstddef>
#include <stdexcept>
#include <vector>

#include "util/db.h"

namespace sonare::mastering::api::detail {

inline std::vector<float> mono_mix(const std::vector<float>& left,
                                   const std::vector<float>& right) {
  if (left.size() != right.size()) {
    throw std::invalid_argument("stereo channel lengths must match");
  }
  std::vector<float> mono(left.size());
  for (std::size_t index = 0; index < left.size(); ++index) {
    mono[index] = 0.5f * (left[index] + right[index]);
  }
  return mono;
}

inline void apply_gain_db(std::vector<float>& samples, float gain_db) {
  const float gain = db_to_linear(gain_db);
  for (float& sample : samples) {
    sample *= gain;
  }
}

inline void apply_gain_db(std::vector<float>& left, std::vector<float>& right, float gain_db) {
  if (left.size() != right.size()) {
    throw std::invalid_argument("stereo channel lengths must match");
  }
  const float gain = db_to_linear(gain_db);
  for (std::size_t index = 0; index < left.size(); ++index) {
    left[index] *= gain;
    right[index] *= gain;
  }
}

}  // namespace sonare::mastering::api::detail
