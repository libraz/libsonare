/// @file sonare_c_engine_test_helpers.h
/// @brief Shared fixtures for the engine C ABI tests: the mixing
///        parameter-target encoders and the MIDI word packer.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>

#include "c_api/sonare_c_engine_internal.h"
#include "sonare_c_test_helpers.h"
#include "support/audio_fixtures.h"
#include "util/json.h"
#include "util/resource_limits.h"

namespace {

#if defined(SONARE_WITH_MIXING)
[[maybe_unused]] constexpr uint32_t engine_lane_param_target(uint32_t lane_index,
                                                             uint32_t param_kind) {
  return 0x4D580000u | (lane_index << 8u) | param_kind;
}

[[maybe_unused]] constexpr uint32_t engine_bus_param_target(uint32_t bus_index,
                                                            uint32_t param_kind) {
  return 0x4D580000u | ((0xFEu - bus_index) << 8u) | param_kind;
}

[[maybe_unused]] constexpr uint32_t engine_master_param_target(uint32_t param_kind) {
  return 0x4D580000u | (0xFFu << 8u) | param_kind;
}

[[maybe_unused]] double rms(const std::array<float, 256>& data) {
  double sum = 0.0;
  for (float value : data) {
    sum += static_cast<double>(value) * static_cast<double>(value);
  }
  return std::sqrt(sum / static_cast<double>(data.size()));
}
#endif  // defined(SONARE_WITH_MIXING)

#if defined(SONARE_WITH_ARRANGEMENT)
[[maybe_unused]] float peak_abs(const std::vector<float>& data) {
  float peak = 0.0f;
  for (float value : data) peak = std::max(peak, std::abs(value));
  return peak;
}

[[maybe_unused]] uint32_t midi1_word(uint8_t status, uint8_t channel, uint8_t data0,
                                     uint8_t data1) {
  return (0x2u << 28u) | (static_cast<uint32_t>(status & 0x0f) << 20u) |
         (static_cast<uint32_t>(channel & 0x0f) << 16u) | (static_cast<uint32_t>(data0) << 8u) |
         static_cast<uint32_t>(data1);
}
#endif  // defined(SONARE_WITH_ARRANGEMENT)

}  // namespace
