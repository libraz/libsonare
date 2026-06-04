#pragma once

/// @file synth_presets.h
/// @brief Named NativeSynth patches — the host-facing preset catalog (data,
///        not code). Each preset is a full NativeSynthConfig (patch + voice
///        pool + bus polish) covering one §E catalog entry: the subtractive
///        leads/pads/basses, the FM e-piano/bell/brass, the Karplus-Strong
///        pluck/electric-guitar/harp, the modal marimba/glass, the drawbar
///        organ, the GM drum kit and the extended-waveguide acoustic piano.
///
/// The returned pointers reference static const data: safe to copy, safe to
/// keep for the process lifetime, never invalidated.

#include <cstddef>

#include "midi/synth/native_synth.h"

namespace sonare::midi::synth {

/// One named preset: the catalog name (stable API surface, kebab-case) and
/// the full synth configuration it selects.
struct SynthPreset {
  const char* name;
  NativeSynthConfig config;
};

/// Number of presets in the catalog.
size_t synth_preset_count() noexcept;

/// Preset by catalog index (nullptr when out of range).
const SynthPreset* synth_preset_at(size_t index) noexcept;

/// Preset by name (exact match; nullptr when unknown). Accepts the bare
/// catalog name ("saw-lead") — hosts strip any "va:" routing prefix first.
const SynthPreset* find_synth_preset(const char* name) noexcept;

}  // namespace sonare::midi::synth
