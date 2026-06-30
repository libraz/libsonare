#include "midi/synth/synth_presets.h"

#include <array>
#include <cstring>

#include "midi/synth/gm_fallback_map.h"

namespace sonare::midi::synth {

namespace {

/// Catalog size (§E preset table).
constexpr size_t kPresetCount = 20;

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

  // --- pipe organ (flue pipe waveguide) ---
  // The full principal chorus (plenum) voiced under the GM Church Organ
  // program, with a gentle tremulant drawn for the showcase preset.
  {
    SynthPreset& organ = t[i++];
    organ.name = "church-organ";
    NativeSynthPatch patch = gm_fallback_patch(0, 19);
    patch.pipe_organ.tremulant_rate_hz = 5.2f;
    patch.pipe_organ.tremulant_depth = 0.5f;
    patch.pipe_organ.swell = 0.8f;  // behind a swell shutter (expression = CC11)
    organ.config = from_patch(clamp_synth_patch(patch));
  }
  {
    // Open flute: softer and darker than the principal (a wide, breathy stop).
    SynthPreset& flute = t[i++];
    flute.name = "church-flute";
    NativeSynthPatch patch{};
    patch.mode = SynthEngineMode::kPipeOrgan;
    patch.amp_env.attack_ms = 12.0f;
    patch.amp_env.sustain = 1.0f;
    patch.amp_env.release_ms = 120.0f;
    patch.cutoff_hz = 20000.0f;
    patch.pipe_organ.stopped = false;
    patch.pipe_organ.brightness = 0.4f;
    patch.pipe_organ.tone_decay_s = 8.0f;
    patch.pipe_organ.breath = 0.45f;
    patch.pipe_organ.chiff = 0.3f;
    patch.gain = 0.7f;
    flute.config = from_patch(clamp_synth_patch(patch));
  }
  {
    // Bourdon / gedackt: a stopped pipe — closed at one end, so it speaks an
    // octave lower for its length and radiates odd harmonics only (a soft,
    // hollow flute).
    SynthPreset& bourdon = t[i++];
    bourdon.name = "church-bourdon";
    NativeSynthPatch patch{};
    patch.mode = SynthEngineMode::kPipeOrgan;
    patch.amp_env.attack_ms = 14.0f;
    patch.amp_env.sustain = 1.0f;
    patch.amp_env.release_ms = 120.0f;
    patch.cutoff_hz = 20000.0f;
    patch.pipe_organ.stopped = true;
    patch.pipe_organ.brightness = 0.35f;
    patch.pipe_organ.tone_decay_s = 8.0f;
    patch.pipe_organ.breath = 0.4f;
    patch.pipe_organ.chiff = 0.25f;
    patch.gain = 0.7f;
    bourdon.config = from_patch(clamp_synth_patch(patch));
  }
  {
    // Trompette / reed chorus: lingual reed pipes — the saturating reed valve
    // buzzes into a bright, brassy self-oscillation (an 8' reed under a 4'),
    // the fanfare colour of the full organ. Voiced under a swell shutter.
    SynthPreset& reed = t[i++];
    reed.name = "church-trumpet";
    NativeSynthPatch patch = gm_fallback_patch(0, 21);  // the Reed Organ voicing
    patch.pipe_organ.swell = 0.7f;
    reed.config = from_patch(clamp_synth_patch(patch));
  }

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
