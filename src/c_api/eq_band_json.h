#pragma once

/// @file eq_band_json.h
/// @brief Shared C-API JSON parser for EQ band specs.

#include <array>
#include <optional>
#include <utility>

#include "mastering/eq/eq_band.h"

namespace sonare::c_api {

// Core EQ field paths and the public band record members they describe.
inline constexpr std::array<std::pair<const char*, const char*>, 12> kEqBandRefusalNames{{
    {"frequency_hz", "band.frequencyHz"},
    {"gain_db", "band.gainDb"},
    {"slope_db_oct", "band.slopeDbOct"},
    {"dyn.ratio", "band.ratio"},
    {"dyn.threshold_db", "band.thresholdDb"},
    {"dyn.range_db", "band.rangeDb"},
    {"dyn.sidechain_q", "band.sidechainQ"},
    {"dyn.attack_ms", "band.attackMs"},
    {"dyn.release_ms", "band.releaseMs"},
    {"dyn.detector_delay_ms", "band.detectorDelayMs"},
    {"dyn.sidechain_freq_hz", "band.sidechainFreqHz"},
    {"q", "band.q"},
}};

sonare::mastering::eq::EqBand parse_eq_band_json(const char* band_json,
                                                 std::optional<double> sample_rate = std::nullopt);

}  // namespace sonare::c_api
