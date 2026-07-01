#include "midi/synth/synth_presets.h"

#include <array>
#include <cstring>

#include "midi/synth/gm_fallback_map.h"

namespace sonare::midi::synth {

namespace {

/// Catalog size (§E preset table).
constexpr size_t kPresetCount = 32;

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
    patch.pipe_organ.radiation = 0.4f;  // an open flute speaks brightly into the room
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
    patch.pipe_organ.radiation = 0.15f;  // a stopped flute stays soft and hollow
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

  // --- bowed string (friction-excited waveguide) ---
  // The violin family (GM 40-43): one bowed-string core voiced across four
  // instrument sizes. The engine tunes to the played note, so the members differ
  // by timbre rather than range — the larger the instrument, the darker and
  // slower-speaking the string and the more the corpus (the shared violin
  // BodyResonator) colours it. All bow near the natural playing point with a
  // touch of rosin grip; the bow contour handles the swell, so the amp envelope
  // just opens and holds.
  {
    auto bowed = [&](const char* name, float bow_position, float bow_force, float brightness,
                     float damping, float attack_ms, float release_ms, float body_mix, float gain) {
      SynthPreset& v = t[i++];
      v.name = name;
      NativeSynthPatch patch{};
      patch.mode = SynthEngineMode::kBowedString;
      patch.amp_env.attack_ms = 20.0f;
      patch.amp_env.sustain = 1.0f;
      patch.amp_env.release_ms = release_ms;
      patch.cutoff_hz = 20000.0f;
      patch.bowed_string.bow_position = bow_position;
      patch.bowed_string.bow_force = bow_force;
      patch.bowed_string.brightness = brightness;
      patch.bowed_string.damping = damping;
      patch.bowed_string.attack_ms = attack_ms;
      patch.bowed_string.release_ms = release_ms;
      patch.bowed_string.rosin = 0.15f;
      patch.body = BodyType::kViolin;
      patch.body_mix = body_mix;
      patch.gain = gain;
      v.config = from_patch(clamp_synth_patch(patch));
    };
    //     name          bow_pos force bright  damp  atk    rel   body  gain
    bowed("violin", 0.12f, 0.55f, 0.62f, 0.30f, 45.0f, 110.0f, 0.28f, 0.70f);
    bowed("viola", 0.13f, 0.55f, 0.52f, 0.34f, 55.0f, 120.0f, 0.34f, 0.70f);
    bowed("cello", 0.14f, 0.60f, 0.44f, 0.38f, 70.0f, 140.0f, 0.40f, 0.72f);
    bowed("contrabass", 0.15f, 0.62f, 0.36f, 0.44f, 90.0f, 160.0f, 0.46f, 0.72f);
  }

  // --- reed woodwind (breath-excited waveguide) ---
  // The reed family (GM 65-72): one reed core voiced across the single- and
  // double-reed winds. The engine tunes to the played note, so the members
  // differ by timbre: the CLARINET is the only cylinder (odd-harmonic, hollow);
  // the saxes and double reeds are conical (full harmonic series). The bell
  // brightness is the main timbral axis (bright/nasal oboe -> dark bassoon /
  // baritone), the shared wood-tube BodyResonator adds the bore/formant colour,
  // and the breath contour handles the speech so the amp envelope just holds.
  {
    auto reed = [&](const char* name, bool conical, float reed_stiffness, float reed_opening,
                    float brightness, float damping, float attack_ms, float release_ms,
                    float breath, float body_mix, float gain) {
      SynthPreset& v = t[i++];
      v.name = name;
      NativeSynthPatch patch{};
      patch.mode = SynthEngineMode::kReed;
      patch.amp_env.attack_ms = 15.0f;
      patch.amp_env.sustain = 1.0f;
      patch.amp_env.release_ms = release_ms;
      patch.cutoff_hz = 20000.0f;
      patch.reed.conical = conical;
      patch.reed.reed_stiffness = reed_stiffness;
      patch.reed.reed_opening = reed_opening;
      patch.reed.brightness = brightness;
      patch.reed.damping = damping;
      patch.reed.attack_ms = attack_ms;
      patch.reed.release_ms = release_ms;
      patch.reed.breath_pressure = breath;
      patch.body = BodyType::kWoodTube;
      patch.body_mix = body_mix;
      patch.gain = gain;
      v.config = from_patch(clamp_synth_patch(patch));
    };
    //    name             cone  stiff  open  bright damp  atk    rel    breath body  gain
    reed("clarinet", false, 0.40f, 0.50f, 0.45f, 0.30f, 25.0f, 90.0f, 0.60f, 0.25f, 0.70f);
    reed("soprano-sax", true, 0.55f, 0.55f, 0.60f, 0.32f, 20.0f, 80.0f, 0.65f, 0.30f, 0.70f);
    reed("alto-sax", true, 0.55f, 0.55f, 0.54f, 0.34f, 22.0f, 90.0f, 0.65f, 0.32f, 0.70f);
    reed("tenor-sax", true, 0.60f, 0.50f, 0.48f, 0.36f, 26.0f, 100.0f, 0.68f, 0.36f, 0.72f);
    reed("baritone-sax", true, 0.60f, 0.50f, 0.40f, 0.40f, 32.0f, 120.0f, 0.70f, 0.40f, 0.72f);
    reed("oboe", true, 0.80f, 0.35f, 0.70f, 0.30f, 18.0f, 70.0f, 0.62f, 0.30f, 0.68f);
    reed("english-horn", true, 0.70f, 0.40f, 0.60f, 0.34f, 24.0f, 90.0f, 0.64f, 0.34f, 0.68f);
    reed("bassoon", true, 0.65f, 0.45f, 0.42f, 0.40f, 30.0f, 120.0f, 0.68f, 0.40f, 0.72f);
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
