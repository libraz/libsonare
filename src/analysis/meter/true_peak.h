#pragma once

/// @file true_peak.h
/// @brief Offline true-peak style inter-sample peak meter.

#include <cstddef>

#include "core/audio.h"

namespace sonare::analysis::meter {

float true_peak(const float* samples, size_t length, int oversample_factor = 4);
float true_peak(const Audio& audio, int oversample_factor = 4);
float true_peak_db(const Audio& audio, int oversample_factor = 4);

}  // namespace sonare::analysis::meter
