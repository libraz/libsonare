#pragma once

/// @file metronome_c.h
/// @brief The one mapping from the core metronome settings to the C record.

#include <sonare/sonare_c.h>

#include "engine/metronome.h"

namespace sonare_c_detail {

/// @p config as the C record; the core spells the derived click length as 0.
inline SonareEngineMetronomeConfig metronome_to_c(const sonare::engine::MetronomeConfig& config) {
  return {sizeof(SonareEngineMetronomeConfig),
          config.enabled ? 1 : 0,
          config.beat_gain,
          config.accent_gain,
          config.click_samples == 0 ? SONARE_ENGINE_METRONOME_CLICK_FROM_SEC : config.click_samples,
          config.click_seconds};
}

}  // namespace sonare_c_detail
