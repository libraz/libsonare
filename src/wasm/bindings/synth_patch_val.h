#pragma once

/// @file synth_patch_val.h
/// @brief JS <-> SonareSynthPatch conversion shared by the WASM project and
///        realtime-engine TUs. The object field names and enum-name strings
///        agree with the Node and Python facades.

#ifdef __EMSCRIPTEN__

#include <emscripten/val.h>

#include <cstring>
#include <iterator>
#include <string>

#include "sonare_c_types.h"
#include "wasm/bindings/common.h"

namespace sonare_wasm_synth {

inline constexpr const char* kEngineModes[] = {
    "default", "subtractive", "fm", "karplus-strong", "modal", "additive", "percussion", "piano"};
inline constexpr const char* kWaveforms[] = {"default", "sine",     "saw",
                                             "square",  "triangle", "noise"};
inline constexpr const char* kFilterModels[] = {"default", "svf", "moog-ladder", "diode-ladder",
                                                "sallen-key"};
inline constexpr const char* kFilterOutputs[] = {"default", "lowpass", "bandpass", "highpass"};
inline constexpr const char* kBodyTypes[] = {"default", "none", "guitar", "violin", "wood-tube"};
inline constexpr const char* kModSources[] = {"none",      "amp-env",   "filter-env",
                                              "lfo1",      "lfo2",      "velocity",
                                              "key-track", "mod-wheel", "random"};
inline constexpr const char* kModDestinations[] = {"none", "pitch-cents", "cutoff-cents",
                                                   "amp-gain", "pan-units"};

static_assert(std::size(kEngineModes) == SONARE_SYNTH_ENGINE_MODE_COUNT,
              "WASM SynthEngineMode table drifted from C");
static_assert(std::size(kWaveforms) == SONARE_SYNTH_OSC_WAVEFORM_COUNT,
              "WASM SynthOscWaveform table drifted from C");
static_assert(std::size(kFilterModels) == SONARE_SYNTH_FILTER_MODEL_COUNT,
              "WASM SynthFilterModel table drifted from C");
static_assert(std::size(kFilterOutputs) == SONARE_SYNTH_FILTER_OUTPUT_COUNT,
              "WASM SynthFilterOutput table drifted from C");
static_assert(std::size(kBodyTypes) == SONARE_SYNTH_BODY_TYPE_COUNT,
              "WASM SynthBodyType table drifted from C");
static_assert(std::size(kModSources) == SONARE_SYNTH_MOD_SOURCE_COUNT,
              "WASM SynthModSource table drifted from C");
static_assert(std::size(kModDestinations) == SONARE_SYNTH_MOD_DESTINATION_COUNT,
              "WASM SynthModDestination table drifted from C");

inline emscripten::val synthEnumTablesToVal() {
  using emscripten::val;
  auto array_from = [](const char* const* names, size_t count) {
    val out = val::array();
    for (size_t i = 0; i < count; ++i) out.set(static_cast<unsigned>(i), std::string(names[i]));
    return out;
  };
  val out = val::object();
  out.set("engineModes", array_from(kEngineModes, std::size(kEngineModes)));
  out.set("waveforms", array_from(kWaveforms, std::size(kWaveforms)));
  out.set("filterModels", array_from(kFilterModels, std::size(kFilterModels)));
  out.set("filterOutputs", array_from(kFilterOutputs, std::size(kFilterOutputs)));
  out.set("bodyTypes", array_from(kBodyTypes, std::size(kBodyTypes)));
  out.set("modSources", array_from(kModSources, std::size(kModSources)));
  out.set("modDestinations", array_from(kModDestinations, std::size(kModDestinations)));
  return out;
}

/// Reads an enum field accepting the C ordinal or a name; throws on an
/// unknown name. Absent fields keep @p out unchanged (0 = "keep base").
inline void enumProperty(emscripten::val object, const char* key, const char* const* names,
                         int count, const char* what, int* out) {
  if (!hasProperty(object, key)) return;
  emscripten::val value = object[key];
  if (value.typeOf().as<std::string>() == "string") {
    const std::string name = value.as<std::string>();
    for (int i = 0; i < count; ++i) {
      if (name == names[i]) {
        *out = i;
        return;
      }
    }
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  std::string("Unknown ") + what + " name: '" + name + "'");
  }
  *out = value.as<int>();
}

inline void setPresetName(SonareSynthPatch* patch, const std::string& name) {
  const std::string bare = name.rfind("va:", 0) == 0 ? name.substr(3) : name;
  std::strncpy(patch->preset, bare.c_str(), SONARE_SYNTH_PRESET_NAME_MAX - 1);
}

/// Parses a JS SynthPatch descriptor (a preset-name string — a "va:" routing
/// prefix is accepted — or an object of wrapper-section overrides) into the
/// versioned C struct. Numeric zero values are the C ABI's "keep base" sentinel,
/// not explicit zero overrides. Throws on unknown enum names; unknown PRESET
/// names are validated downstream.
inline SonareSynthPatch synthPatchFromVal(emscripten::val desc) {
  SonareSynthPatch patch{};
  patch.struct_version = 1;
  if (desc.isUndefined() || desc.isNull()) return patch;
  if (desc.typeOf().as<std::string>() == "string") {
    setPresetName(&patch, desc.as<std::string>());
    return patch;
  }
  if (hasProperty(desc, "preset")) {
    setPresetName(&patch, desc["preset"].as<std::string>());
  }
  enumProperty(desc, "engineMode", kEngineModes, SONARE_SYNTH_ENGINE_MODE_COUNT,
               "synth engine mode", &patch.engine_mode);
  enumProperty(desc, "waveform", kWaveforms, SONARE_SYNTH_OSC_WAVEFORM_COUNT, "oscillator waveform",
               &patch.waveform);
  enumProperty(desc, "filterModel", kFilterModels, SONARE_SYNTH_FILTER_MODEL_COUNT, "filter model",
               &patch.filter_model);
  enumProperty(desc, "filterOutput", kFilterOutputs, SONARE_SYNTH_FILTER_OUTPUT_COUNT,
               "filter output", &patch.filter_output);
  enumProperty(desc, "body", kBodyTypes, SONARE_SYNTH_BODY_TYPE_COUNT, "body type", &patch.body);
  patch.unison = intProperty(desc, "unison", 0);
  patch.detune_cents = floatProperty(desc, "detuneCents", 0.0f);
  patch.drift_cents = floatProperty(desc, "driftCents", 0.0f);
  patch.drive = floatProperty(desc, "drive", 0.0f);
  patch.cutoff_hz = floatProperty(desc, "cutoffHz", 0.0f);
  patch.resonance_q = floatProperty(desc, "resonanceQ", 0.0f);
  patch.key_track = floatProperty(desc, "keyTrack", 0.0f);
  patch.env_to_cutoff_cents = floatProperty(desc, "envToCutoffCents", 0.0f);
  patch.vel_to_cutoff_cents = floatProperty(desc, "velToCutoffCents", 0.0f);
  patch.amp_attack_ms = floatProperty(desc, "ampAttackMs", 0.0f);
  patch.amp_decay_ms = floatProperty(desc, "ampDecayMs", 0.0f);
  patch.amp_sustain = floatProperty(desc, "ampSustain", 0.0f);
  patch.amp_release_ms = floatProperty(desc, "ampReleaseMs", 0.0f);
  patch.filter_attack_ms = floatProperty(desc, "filterAttackMs", 0.0f);
  patch.filter_decay_ms = floatProperty(desc, "filterDecayMs", 0.0f);
  patch.filter_sustain = floatProperty(desc, "filterSustain", 0.0f);
  patch.filter_release_ms = floatProperty(desc, "filterReleaseMs", 0.0f);
  patch.lfo_rate_hz = floatProperty(desc, "lfoRateHz", 0.0f);
  patch.lfo_to_pitch_cents = floatProperty(desc, "lfoToPitchCents", 0.0f);
  patch.lfo2_rate_hz = floatProperty(desc, "lfo2RateHz", 0.0f);
  patch.glide_ms = floatProperty(desc, "glideMs", 0.0f);
  patch.body_mix = floatProperty(desc, "bodyMix", 0.0f);
  patch.stereo_spread = floatProperty(desc, "stereoSpread", 0.0f);
  patch.gain = floatProperty(desc, "gain", 0.0f);
  patch.polyphony = intProperty(desc, "polyphony", 0);
  patch.bus_drive = floatProperty(desc, "busDrive", 0.0f);

  if (hasProperty(desc, "modRoutings")) {
    emscripten::val routings = desc["modRoutings"];
    if (emscripten::val::global("Array").call<bool>("isArray", routings)) {
      const size_t count = routings["length"].as<size_t>();
      if (count > SONARE_SYNTH_PATCH_MOD_ROUTINGS) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "a synth patch supports at most 8 mod routings");
      }
      patch.num_mod_routings = static_cast<int>(count);
      for (size_t i = 0; i < count; ++i) {
        emscripten::val routing = routings[i];
        SonareSynthModRouting& out = patch.mod_routings[i];
        enumProperty(routing, "source", kModSources, SONARE_SYNTH_MOD_SOURCE_COUNT, "mod source",
                     &out.source);
        enumProperty(routing, "destination", kModDestinations, SONARE_SYNTH_MOD_DESTINATION_COUNT,
                     "mod destination", &out.destination);
        out.depth = floatProperty(routing, "depth", 0.0f);
      }
    }
  }
  return patch;
}

/// Converts a versioned C synth patch into the JS SynthPatch object shape
/// (enum ordinals become their canonical names, so the object can be passed
/// back verbatim).
inline emscripten::val synthPatchToVal(const SonareSynthPatch& patch) {
  using emscripten::val;
  auto enum_name = [](int value, const char* const* names, int count) -> val {
    if (value >= 0 && value < count) return val(std::string(names[value]));
    return val(value);
  };
  val out = val::object();
  out.set("preset", std::string(patch.preset));
  out.set("engineMode", enum_name(patch.engine_mode, kEngineModes, SONARE_SYNTH_ENGINE_MODE_COUNT));
  out.set("waveform", enum_name(patch.waveform, kWaveforms, SONARE_SYNTH_OSC_WAVEFORM_COUNT));
  out.set("unison", patch.unison);
  out.set("detuneCents", patch.detune_cents);
  out.set("driftCents", patch.drift_cents);
  out.set("drive", patch.drive);
  out.set("filterModel",
          enum_name(patch.filter_model, kFilterModels, SONARE_SYNTH_FILTER_MODEL_COUNT));
  out.set("filterOutput",
          enum_name(patch.filter_output, kFilterOutputs, SONARE_SYNTH_FILTER_OUTPUT_COUNT));
  out.set("cutoffHz", patch.cutoff_hz);
  out.set("resonanceQ", patch.resonance_q);
  out.set("keyTrack", patch.key_track);
  out.set("envToCutoffCents", patch.env_to_cutoff_cents);
  out.set("velToCutoffCents", patch.vel_to_cutoff_cents);
  out.set("ampAttackMs", patch.amp_attack_ms);
  out.set("ampDecayMs", patch.amp_decay_ms);
  out.set("ampSustain", patch.amp_sustain);
  out.set("ampReleaseMs", patch.amp_release_ms);
  out.set("filterAttackMs", patch.filter_attack_ms);
  out.set("filterDecayMs", patch.filter_decay_ms);
  out.set("filterSustain", patch.filter_sustain);
  out.set("filterReleaseMs", patch.filter_release_ms);
  out.set("lfoRateHz", patch.lfo_rate_hz);
  out.set("lfoToPitchCents", patch.lfo_to_pitch_cents);
  out.set("lfo2RateHz", patch.lfo2_rate_hz);
  out.set("glideMs", patch.glide_ms);
  out.set("body", enum_name(patch.body, kBodyTypes, SONARE_SYNTH_BODY_TYPE_COUNT));
  out.set("bodyMix", patch.body_mix);
  out.set("stereoSpread", patch.stereo_spread);
  val routings = val::array();
  for (int i = 0; i < patch.num_mod_routings && i < SONARE_SYNTH_PATCH_MOD_ROUTINGS; ++i) {
    val routing = val::object();
    routing.set("source", enum_name(patch.mod_routings[i].source, kModSources,
                                    SONARE_SYNTH_MOD_SOURCE_COUNT));
    routing.set("destination", enum_name(patch.mod_routings[i].destination, kModDestinations,
                                         SONARE_SYNTH_MOD_DESTINATION_COUNT));
    routing.set("depth", patch.mod_routings[i].depth);
    routings.call<void>("push", routing);
  }
  out.set("modRoutings", routings);
  out.set("gain", patch.gain);
  out.set("polyphony", patch.polyphony);
  out.set("busDrive", patch.bus_drive);
  return out;
}

}  // namespace sonare_wasm_synth

#endif  // __EMSCRIPTEN__
