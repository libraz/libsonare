#pragma once

/// @file pan_law.h
/// @brief Mixer-facing names for the shared pan-law evaluator.
///
/// The one implementation lives in rt/pan_law.h so the mastering stereo
/// processors and the clip player can reach it without depending on the mixer.
/// These declarations only re-export it under sonare::mixing, which is where the
/// mixer, the engine and the C ABI already spell it.

#include "rt/pan_law.h"

namespace sonare::mixing {

using ::sonare::rt::PanGains;
using ::sonare::rt::PanLaw;
using ::sonare::rt::PanNormalization;

using ::sonare::rt::compute_pan_gains;
using ::sonare::rt::normalize_pan_gains;
using ::sonare::rt::pan_law_from_index;

}  // namespace sonare::mixing
