#include "midi/synth/synth_presets.h"

#include <array>
#include <cstring>

#include "midi/synth/gm_fallback_map.h"

namespace sonare::midi::synth {

namespace {

/// Catalog size (§E preset table).
constexpr size_t kPresetCount = 16;

NativeSynthConfig from_patch(const NativeSynthPatch& patch) noexcept {
  NativeSynthConfig cfg;
  cfg.patch = patch;
  return cfg;
}

/// Most catalog entries are the voiced GM fallback patches under their
/// instrument name (one data table, two address spaces); the pure-synth
/// entries are voiced here.
std::array<SynthPreset, kPresetCount> build_presets() noexcept {
  std::array<SynthPreset, kPresetCount> t{};
  size_t i = 0;

  // --- subtractive ---
  NativeSynthPatch sine{};
  sine.waveform = VaWaveform::kSine;
  sine.cutoff_hz = 20000.0f;
  sine.amp_env.attack_ms = 3.0f;
  sine.amp_env.decay_ms = 60.0f;
  sine.amp_env.sustain = 0.8f;
  sine.amp_env.release_ms = 150.0f;
  t[i++] = {"sine", from_patch(clamp_synth_patch(sine))};

  t[i++] = {"saw-lead", from_patch(gm_fallback_patch(0, 80))};

  NativeSynthPatch square = gm_fallback_patch(0, 80);
  square.waveform = VaWaveform::kSquare;
  square.unison = 2;
  square.detune_cents = 8.0f;
  square.drift_cents = 4.0f;  // PWM-ish movement from the seeded drift
  square.cutoff_hz = 3000.0f;
  t[i++] = {"square-lead", from_patch(clamp_synth_patch(square))};

  NativeSynthPatch sub = gm_fallback_patch(0, 33);
  sub.unison = 1;
  sub.cutoff_hz = 600.0f;
  t[i++] = {"sub-bass", from_patch(clamp_synth_patch(sub))};

  {
    SynthPreset& pad = t[i++];
    pad.name = "warm-pad";
    pad.config = from_patch(gm_fallback_patch(0, 88));
    pad.config.bus_drive = 0.15f;  // glue the supersaw stack
  }

  // --- FM ---
  t[i++] = {"e-piano", from_patch(gm_fallback_patch(0, 4))};
  t[i++] = {"bell", from_patch(gm_fallback_patch(0, 14))};
  t[i++] = {"brass", from_patch(gm_fallback_patch(0, 56))};

  // --- Karplus-Strong ---
  t[i++] = {"pluck", from_patch(gm_fallback_patch(0, 104))};
  t[i++] = {"electric-guitar", from_patch(gm_fallback_patch(0, 26))};
  t[i++] = {"harp", from_patch(gm_fallback_patch(0, 46))};

  // --- modal / additive ---
  t[i++] = {"marimba", from_patch(gm_fallback_patch(0, 12))};
  t[i++] = {"glass", from_patch(gm_fallback_patch(0, 9))};
  t[i++] = {"organ", from_patch(gm_fallback_patch(0, 16))};

  // --- percussion / piano ---
  {
    SynthPreset& kit = t[i++];
    kit.name = "drum-kit";
    NativeSynthPatch patch{};
    patch.mode = SynthEngineMode::kPercussion;
    patch.percussion.gm_kit = true;
    patch.cutoff_hz = 20000.0f;
    patch.gain = 0.8f;
    kit.config = from_patch(clamp_synth_patch(patch));
    kit.config.polyphony = 24;  // a kit stacks pieces, not melodic lines
  }

  t[i++] = {"acoustic-piano", from_patch(gm_fallback_patch(0, 0))};

  return t;
}

const std::array<SynthPreset, kPresetCount>& presets() noexcept {
  static const std::array<SynthPreset, kPresetCount> kTable = build_presets();
  return kTable;
}

}  // namespace

size_t synth_preset_count() noexcept { return kPresetCount; }

const SynthPreset* synth_preset_at(size_t index) noexcept {
  if (index >= kPresetCount) return nullptr;
  return &presets()[index];
}

const SynthPreset* find_synth_preset(const char* name) noexcept {
  if (name == nullptr) return nullptr;
  for (const SynthPreset& preset : presets()) {
    if (std::strcmp(preset.name, name) == 0) return &preset;
  }
  return nullptr;
}

}  // namespace sonare::midi::synth
