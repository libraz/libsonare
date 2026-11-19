#pragma once

/// @file cut_band.h
/// @brief Shared private helpers for EQ cut-band routing and validation.

#include <array>

#include "mastering/eq/eq_band.h"
#include "util/exception.h"
#include "util/parameter_checks.h"

namespace sonare::mastering::eq::detail {

inline bool is_cut_band(EqBandType type) noexcept {
  return type == EqBandType::LowPass || type == EqBandType::HighPass;
}

inline int cut_order(int slope_db_oct) {
  if (slope_db_oct == 0) {
    return 0;
  }
  static constexpr std::array<int, 17> kSlopes = {0,  6,  12, 18, 24, 30, 36, 42, 48,
                                                  54, 60, 66, 72, 78, 84, 90, 96};
  util::check::one_of(slope_db_oct, kSlopes, "slope_db_oct");
  return slope_db_oct / 6;
}

}  // namespace sonare::mastering::eq::detail
