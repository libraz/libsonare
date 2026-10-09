#pragma once

/// @file cut_band.h
/// @brief Shared private helpers for EQ cut-band routing and validation.

#include "mastering/eq/eq_band.h"
#include "util/exception.h"

namespace sonare::mastering::eq::detail {

inline bool is_cut_band(EqBandType type) noexcept {
  return type == EqBandType::LowPass || type == EqBandType::HighPass;
}

inline int cut_order(int slope_db_oct) {
  if (slope_db_oct == 0) {
    return 0;
  }
  if (slope_db_oct < 6 || slope_db_oct > 96 || (slope_db_oct % 6) != 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "cut slope must be 0 or 6..96 dB/oct in 6 dB steps");
  }
  return slope_db_oct / 6;
}

}  // namespace sonare::mastering::eq::detail
