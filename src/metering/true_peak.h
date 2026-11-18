#pragma once

/// @file true_peak.h
/// @brief Offline true-peak style inter-sample peak meter.

#include <cstddef>

#include "core/audio.h"

namespace sonare::metering {

/// @brief Oversample factor the true-peak meter uses when the caller names none.
inline constexpr int kDefaultTruePeakOversample = 4;

float true_peak(const float* samples, size_t length,
                int oversample_factor = kDefaultTruePeakOversample);
float true_peak(const Audio& audio, int oversample_factor = kDefaultTruePeakOversample);
float true_peak_db(const Audio& audio, int oversample_factor = kDefaultTruePeakOversample);

}  // namespace sonare::metering
