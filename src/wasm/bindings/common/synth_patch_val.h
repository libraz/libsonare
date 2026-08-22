#pragma once

/// @file synth_patch_val.h
/// @brief JS <-> SonareSynthPatch conversion shared by the WASM project and
///        realtime-engine TUs. The object field names and enum-name strings
///        agree with the Node and Python facades.

#ifdef __EMSCRIPTEN__

#include <emscripten/val.h>
#include <sonare/sonare_c_project.h>
#include <sonare/sonare_c_types.h>

#include <cstring>
#include <iterator>
#include <string>

#include "wasm/bindings/common/common.h"

namespace sonare_wasm_synth {

inline constexpr const char* kEngineModes[] = {
    "default",    "subtractive",    "fm",         "karplus-strong", "modal",      "additive",
    "percussion", "piano",          "pipe-organ", "bowed-string",   "reed",       "brass",
    "flute",      "plucked-string", "vocal",      "free-reed",      "harpsichord"};
inline constexpr const char* kWaveforms[] = {"default", "sine",     "saw",
                                             "square",  "triangle", "noise"};
inline constexpr const char* kFilterModels[] = {"default", "svf", "moog-ladder", "diode-ladder",
                                                "sallen-key"};
inline constexpr const char* kFilterOutputs[] = {"default", "lowpass", "bandpass", "highpass"};
inline constexpr const char* kBodyTypes[] = {"default",   "none",       "guitar", "violin",
                                             "wood-tube", "brass-bell", "vocal"};
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
  auto array_from = [](const char* joined) {
    val out = val::array();
    if (joined == nullptr || joined[0] == '\0') return out;
    std::string names(joined);
    unsigned index = 0;
    size_t start = 0;
    while (start <= names.size()) {
      const size_t end = names.find('\n', start);
      if (end == std::string::npos) {
        out.set(index++, names.substr(start));
        break;
      }
      out.set(index++, names.substr(start, end - start));
      start = end + 1;
    }
    return out;
  };
  val out = val::object();
  out.set("engineModes", array_from(sonare_synth_enum_names(SONARE_SYNTH_ENUM_ENGINE_MODE)));
  out.set("waveforms", array_from(sonare_synth_enum_names(SONARE_SYNTH_ENUM_OSC_WAVEFORM)));
  out.set("builtinWaveforms",
          array_from(sonare_synth_enum_names(SONARE_SYNTH_ENUM_BUILTIN_WAVEFORM)));
  out.set("filterModels", array_from(sonare_synth_enum_names(SONARE_SYNTH_ENUM_FILTER_MODEL)));
  out.set("filterOutputs", array_from(sonare_synth_enum_names(SONARE_SYNTH_ENUM_FILTER_OUTPUT)));
  out.set("bodyTypes", array_from(sonare_synth_enum_names(SONARE_SYNTH_ENUM_BODY_TYPE)));
  out.set("modSources", array_from(sonare_synth_enum_names(SONARE_SYNTH_ENUM_MOD_SOURCE)));
  out.set("modDestinations",
          array_from(sonare_synth_enum_names(SONARE_SYNTH_ENUM_MOD_DESTINATION)));
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
  // Reject non-number, non-string values (e.g. a boolean) instead of coercing
  // them to a bogus ordinal, matching the Node addon's enum reader.
  if (value.typeOf().as<std::string>() != "number") {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  std::string("Expected ") + what + " to be a number or string");
  }
  *out = value.as<int>();
}

inline void setPresetName(SonareSynthPatch* patch, const std::string& name) {
  const std::string bare = name.rfind("va:", 0) == 0 ? name.substr(3) : name;
  std::strncpy(patch->preset, bare.c_str(), SONARE_SYNTH_PRESET_NAME_MAX - 1);
}

/// Parses a JS SynthPatch descriptor (a preset-name string — a "va:" routing
/// prefix is accepted — or an object of wrapper-section overrides) into the
/// versioned C struct. A key the caller actually supplied also sets its
/// present_fields bit, so `stereoSpread: 0` reaches the core as an explicit
/// zero rather than the "keep base" sentinel a bare zero would be. Throws on
/// unknown enum names; unknown PRESET names are validated downstream.
inline SonareSynthPatch synthPatchFromVal(emscripten::val desc) {
  SonareSynthPatch patch{};
  patch.struct_version = 2;
  if (desc.isUndefined() || desc.isNull()) return patch;
  if (desc.typeOf().as<std::string>() == "string") {
    setPresetName(&patch, desc.as<std::string>());
    return patch;
  }
  if (desc.typeOf().as<std::string>() != "object") {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "synth patch must be a preset-name string or an object");
  }
  if (hasProperty(desc, "preset")) {
    emscripten::val preset = desc["preset"];
    if (preset.typeOf().as<std::string>() != "string") {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "synth patch preset must be a string");
    }
    setPresetName(&patch, preset.as<std::string>());
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
  auto read_float = [&desc, &patch](const char* key, uint32_t bit, float* out) {
    *out = floatProperty(desc, key, 0.0f);
    if (hasProperty(desc, key)) patch.present_fields |= bit;
  };
  auto read_int = [&desc, &patch](const char* key, uint32_t bit, int* out) {
    *out = intProperty(desc, key, 0);
    if (hasProperty(desc, key)) patch.present_fields |= bit;
  };
  read_int("unison", SONARE_SYNTH_FIELD_UNISON, &patch.unison);
  read_float("detuneCents", SONARE_SYNTH_FIELD_DETUNE_CENTS, &patch.detune_cents);
  read_float("driftCents", SONARE_SYNTH_FIELD_DRIFT_CENTS, &patch.drift_cents);
  read_float("drive", SONARE_SYNTH_FIELD_DRIVE, &patch.drive);
  read_float("cutoffHz", SONARE_SYNTH_FIELD_CUTOFF_HZ, &patch.cutoff_hz);
  read_float("resonanceQ", SONARE_SYNTH_FIELD_RESONANCE_Q, &patch.resonance_q);
  read_float("keyTrack", SONARE_SYNTH_FIELD_KEY_TRACK, &patch.key_track);
  read_float("envToCutoffCents", SONARE_SYNTH_FIELD_ENV_TO_CUTOFF_CENTS,
             &patch.env_to_cutoff_cents);
  read_float("velToCutoffCents", SONARE_SYNTH_FIELD_VEL_TO_CUTOFF_CENTS,
             &patch.vel_to_cutoff_cents);
  read_float("ampAttackMs", SONARE_SYNTH_FIELD_AMP_ATTACK_MS, &patch.amp_attack_ms);
  read_float("ampDecayMs", SONARE_SYNTH_FIELD_AMP_DECAY_MS, &patch.amp_decay_ms);
  read_float("ampSustain", SONARE_SYNTH_FIELD_AMP_SUSTAIN, &patch.amp_sustain);
  read_float("ampReleaseMs", SONARE_SYNTH_FIELD_AMP_RELEASE_MS, &patch.amp_release_ms);
  read_float("filterAttackMs", SONARE_SYNTH_FIELD_FILTER_ATTACK_MS, &patch.filter_attack_ms);
  read_float("filterDecayMs", SONARE_SYNTH_FIELD_FILTER_DECAY_MS, &patch.filter_decay_ms);
  read_float("filterSustain", SONARE_SYNTH_FIELD_FILTER_SUSTAIN, &patch.filter_sustain);
  read_float("filterReleaseMs", SONARE_SYNTH_FIELD_FILTER_RELEASE_MS, &patch.filter_release_ms);
  read_float("lfoRateHz", SONARE_SYNTH_FIELD_LFO_RATE_HZ, &patch.lfo_rate_hz);
  read_float("lfoToPitchCents", SONARE_SYNTH_FIELD_LFO_TO_PITCH_CENTS, &patch.lfo_to_pitch_cents);
  read_float("lfo2RateHz", SONARE_SYNTH_FIELD_LFO2_RATE_HZ, &patch.lfo2_rate_hz);
  read_float("glideMs", SONARE_SYNTH_FIELD_GLIDE_MS, &patch.glide_ms);
  read_float("bodyMix", SONARE_SYNTH_FIELD_BODY_MIX, &patch.body_mix);
  read_float("stereoSpread", SONARE_SYNTH_FIELD_STEREO_SPREAD, &patch.stereo_spread);
  read_float("gain", SONARE_SYNTH_FIELD_GAIN, &patch.gain);
  read_int("polyphony", SONARE_SYNTH_FIELD_POLYPHONY, &patch.polyphony);
  read_float("busDrive", SONARE_SYNTH_FIELD_BUS_DRIVE, &patch.bus_drive);

  if (hasProperty(desc, "modRoutings")) {
    emscripten::val routings = desc["modRoutings"];
    if (emscripten::val::global("Array").call<bool>("isArray", routings)) {
      const size_t count = routings["length"].as<size_t>();
      if (count > SONARE_SYNTH_PATCH_MOD_ROUTINGS) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "a synth patch supports at most 8 mod routings");
      }
      patch.num_mod_routings = static_cast<int>(count);
      // An explicitly supplied array — including an empty one — replaces the
      // base matrix; omitting the key keeps it.
      patch.present_fields |= SONARE_SYNTH_FIELD_MOD_ROUTINGS;
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
