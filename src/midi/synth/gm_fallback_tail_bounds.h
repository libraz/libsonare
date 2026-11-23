#pragma once

#include <cstdint>

#include "midi/synth/gs_layer.h"
#include "midi/synth/native_synth_patch.h"

namespace sonare::midi::synth {

/// Release tail of one melodic fallback patch at @p sample_rate under the part's envelope scales.
int64_t gm_fallback_program_tail_samples(const NativeSynthPatch& patch, double sample_rate,
                                         const EnvelopeTimeScales& scales) noexcept;
/// Release tail of one drum note after the GS kit's per-note variation is applied to @p patch.
int64_t gm_fallback_drum_note_tail_samples(const NativeSynthPatch& patch, uint8_t kit, uint8_t note,
                                           double sample_rate,
                                           const EnvelopeTimeScales& scales) noexcept;

}  // namespace sonare::midi::synth
