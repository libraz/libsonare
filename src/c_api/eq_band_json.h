#pragma once

/// @file eq_band_json.h
/// @brief Shared C-API JSON parser for EQ band specs.

#include "mastering/eq/eq_band.h"

namespace sonare::c_api {

sonare::mastering::eq::EqBand parse_eq_band_json(const char* band_json);

}  // namespace sonare::c_api
