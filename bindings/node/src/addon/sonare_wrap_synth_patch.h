#ifndef SONARE_NODE_SONARE_WRAP_SYNTH_PATCH_H_
#define SONARE_NODE_SONARE_WRAP_SYNTH_PATCH_H_

#include <napi.h>
#include <sonare/sonare_c.h>

#include <cstring>
#include <iterator>
#include <string>

#include "sonare_wrap_options.h"

namespace sonare_node {

inline constexpr const char* kSynthEngineModes[] = {
    "default",    "subtractive",    "fm",         "karplus-strong", "modal", "additive",
    "percussion", "piano",          "pipe-organ", "bowed-string",   "reed",  "brass",
    "flute",      "plucked-string", "vocal",      "free-reed"};
inline constexpr const char* kSynthWaveforms[] = {"default", "sine",     "saw",
                                                  "square",  "triangle", "noise"};
inline constexpr const char* kSynthFilterModels[] = {"default", "svf", "moog-ladder",
                                                     "diode-ladder", "sallen-key"};
inline constexpr const char* kSynthFilterOutputs[] = {"default", "lowpass", "bandpass", "highpass"};
inline constexpr const char* kSynthBodyTypes[] = {"default",   "none",       "guitar", "violin",
                                                  "wood-tube", "brass-bell", "vocal"};
inline constexpr const char* kSynthModSources[] = {"none",      "amp-env",   "filter-env",
                                                   "lfo1",      "lfo2",      "velocity",
                                                   "key-track", "mod-wheel", "random"};
inline constexpr const char* kSynthModDestinations[] = {"none", "pitch-cents", "cutoff-cents",
                                                        "amp-gain", "pan-units"};

static_assert(std::size(kSynthEngineModes) == SONARE_SYNTH_ENGINE_MODE_COUNT,
              "Node SynthEngineMode table drifted from C");
static_assert(std::size(kSynthWaveforms) == SONARE_SYNTH_OSC_WAVEFORM_COUNT,
              "Node SynthOscWaveform table drifted from C");
static_assert(std::size(kSynthFilterModels) == SONARE_SYNTH_FILTER_MODEL_COUNT,
              "Node SynthFilterModel table drifted from C");
static_assert(std::size(kSynthFilterOutputs) == SONARE_SYNTH_FILTER_OUTPUT_COUNT,
              "Node SynthFilterOutput table drifted from C");
static_assert(std::size(kSynthBodyTypes) == SONARE_SYNTH_BODY_TYPE_COUNT,
              "Node SynthBodyType table drifted from C");
static_assert(std::size(kSynthModSources) == SONARE_SYNTH_MOD_SOURCE_COUNT,
              "Node SynthModSource table drifted from C");
static_assert(std::size(kSynthModDestinations) == SONARE_SYNTH_MOD_DESTINATION_COUNT,
              "Node SynthModDestination table drifted from C");

// SonareSynthPatch enum-name maps (shared by the project bounce and realtime
// engine TUs; the names agree with the Python / WASM facades). 0 / "default"
// keeps the base patch's value per the C ABI contract.
inline int SynthEnumFromName(const std::string& name, const char* const* names, int count) {
  for (int i = 0; i < count; ++i) {
    if (name == names[i]) return i;
  }
  return -1;
}

// Reads an enum field that accepts the C ordinal or a name. Returns false
// (with a pending JS exception) on an unknown name.
inline bool SynthEnumProperty(Napi::Env env, const Napi::Object& obj, const char* key,
                              const char* const* names, int count, const char* what, int* out) {
  Napi::Value value = obj.Get(key);
  if (value.IsUndefined()) return true;
  if (value.IsString()) {
    const std::string name = value.As<Napi::String>().Utf8Value();
    const int mapped = SynthEnumFromName(name, names, count);
    if (mapped < 0) {
      Napi::TypeError::New(env, std::string("Unknown ") + what + " name: '" + name + "'")
          .ThrowAsJavaScriptException();
      return false;
    }
    *out = mapped;
    return true;
  }
  if (!value.IsNumber()) {
    Napi::TypeError::New(env, std::string("Expected ") + what + " to be a number or string")
        .ThrowAsJavaScriptException();
    return false;
  }
  *out = value.As<Napi::Number>().Int32Value();
  return true;
}

// True when the descriptor carries the key with a usable value. JS absence and
// an explicit undefined/null are the same thing here, so only a real value
// marks the field present.
inline bool SynthFieldPresent(const Napi::Object& obj, const char* key) {
  if (!obj.Has(key)) return false;
  const Napi::Value value = obj.Get(key);
  return !value.IsUndefined() && !value.IsNull();
}

// Parses a JS SynthPatch descriptor (a preset-name string — a "va:" routing
// prefix is accepted — or an object of wrapper-section overrides) into the
// versioned C struct. A key the caller actually supplied also sets its
// present_fields bit, so `stereoSpread: 0` reaches the core as an explicit zero
// rather than the "keep base" sentinel a bare zero would be. Returns false with
// a pending JS exception on an unknown enum name; unknown PRESET names are
// validated downstream by the C ABI.
inline bool ReadSynthPatch(Napi::Env env, const Napi::Value& desc, SonareSynthPatch* patch) {
  *patch = SonareSynthPatch{};
  patch->struct_version = 2;
  if (desc.IsUndefined() || desc.IsNull()) return true;

  auto set_preset = [patch](const std::string& name) {
    const std::string bare = name.rfind("va:", 0) == 0 ? name.substr(3) : name;
    std::strncpy(patch->preset, bare.c_str(), SONARE_SYNTH_PRESET_NAME_MAX - 1);
  };
  if (desc.IsString()) {
    set_preset(desc.As<Napi::String>().Utf8Value());
    return true;
  }
  if (!desc.IsObject()) {
    Napi::TypeError::New(env, "synth patch must be a preset-name string or an object")
        .ThrowAsJavaScriptException();
    return false;
  }
  Napi::Object obj = desc.As<Napi::Object>();
  if (obj.Has("preset")) {
    Napi::Value preset = obj.Get("preset");
    if (preset.IsUndefined() || preset.IsNull()) {
      // Treat explicit JS absence the same as an omitted preset key.
    } else if (!preset.IsString()) {
      Napi::TypeError::New(env, "synth patch preset must be a string").ThrowAsJavaScriptException();
      return false;
    } else {
      set_preset(preset.As<Napi::String>().Utf8Value());
    }
  }
  if (!SynthEnumProperty(env, obj, "engineMode", kSynthEngineModes, SONARE_SYNTH_ENGINE_MODE_COUNT,
                         "synth engine mode", &patch->engine_mode) ||
      !SynthEnumProperty(env, obj, "waveform", kSynthWaveforms, SONARE_SYNTH_OSC_WAVEFORM_COUNT,
                         "oscillator waveform", &patch->waveform) ||
      !SynthEnumProperty(env, obj, "filterModel", kSynthFilterModels,
                         SONARE_SYNTH_FILTER_MODEL_COUNT, "filter model", &patch->filter_model) ||
      !SynthEnumProperty(env, obj, "filterOutput", kSynthFilterOutputs,
                         SONARE_SYNTH_FILTER_OUTPUT_COUNT, "filter output",
                         &patch->filter_output) ||
      !SynthEnumProperty(env, obj, "body", kSynthBodyTypes, SONARE_SYNTH_BODY_TYPE_COUNT,
                         "body type", &patch->body)) {
    return false;
  }
  auto read_float = [&](const char* key, uint32_t bit, float* out) {
    *out = FloatProperty(obj, key, 0.0f);
    if (SynthFieldPresent(obj, key)) patch->present_fields |= bit;
  };
  auto read_int = [&](const char* key, uint32_t bit, int* out) {
    *out = IntProperty(obj, key, 0);
    if (SynthFieldPresent(obj, key)) patch->present_fields |= bit;
  };
  read_int("unison", SONARE_SYNTH_FIELD_UNISON, &patch->unison);
  read_float("detuneCents", SONARE_SYNTH_FIELD_DETUNE_CENTS, &patch->detune_cents);
  read_float("driftCents", SONARE_SYNTH_FIELD_DRIFT_CENTS, &patch->drift_cents);
  read_float("drive", SONARE_SYNTH_FIELD_DRIVE, &patch->drive);
  read_float("cutoffHz", SONARE_SYNTH_FIELD_CUTOFF_HZ, &patch->cutoff_hz);
  read_float("resonanceQ", SONARE_SYNTH_FIELD_RESONANCE_Q, &patch->resonance_q);
  read_float("keyTrack", SONARE_SYNTH_FIELD_KEY_TRACK, &patch->key_track);
  read_float("envToCutoffCents", SONARE_SYNTH_FIELD_ENV_TO_CUTOFF_CENTS,
             &patch->env_to_cutoff_cents);
  read_float("velToCutoffCents", SONARE_SYNTH_FIELD_VEL_TO_CUTOFF_CENTS,
             &patch->vel_to_cutoff_cents);
  read_float("ampAttackMs", SONARE_SYNTH_FIELD_AMP_ATTACK_MS, &patch->amp_attack_ms);
  read_float("ampDecayMs", SONARE_SYNTH_FIELD_AMP_DECAY_MS, &patch->amp_decay_ms);
  read_float("ampSustain", SONARE_SYNTH_FIELD_AMP_SUSTAIN, &patch->amp_sustain);
  read_float("ampReleaseMs", SONARE_SYNTH_FIELD_AMP_RELEASE_MS, &patch->amp_release_ms);
  read_float("filterAttackMs", SONARE_SYNTH_FIELD_FILTER_ATTACK_MS, &patch->filter_attack_ms);
  read_float("filterDecayMs", SONARE_SYNTH_FIELD_FILTER_DECAY_MS, &patch->filter_decay_ms);
  read_float("filterSustain", SONARE_SYNTH_FIELD_FILTER_SUSTAIN, &patch->filter_sustain);
  read_float("filterReleaseMs", SONARE_SYNTH_FIELD_FILTER_RELEASE_MS, &patch->filter_release_ms);
  read_float("lfoRateHz", SONARE_SYNTH_FIELD_LFO_RATE_HZ, &patch->lfo_rate_hz);
  read_float("lfoToPitchCents", SONARE_SYNTH_FIELD_LFO_TO_PITCH_CENTS, &patch->lfo_to_pitch_cents);
  read_float("lfo2RateHz", SONARE_SYNTH_FIELD_LFO2_RATE_HZ, &patch->lfo2_rate_hz);
  read_float("glideMs", SONARE_SYNTH_FIELD_GLIDE_MS, &patch->glide_ms);
  read_float("bodyMix", SONARE_SYNTH_FIELD_BODY_MIX, &patch->body_mix);
  read_float("stereoSpread", SONARE_SYNTH_FIELD_STEREO_SPREAD, &patch->stereo_spread);
  read_float("gain", SONARE_SYNTH_FIELD_GAIN, &patch->gain);
  read_int("polyphony", SONARE_SYNTH_FIELD_POLYPHONY, &patch->polyphony);
  read_float("busDrive", SONARE_SYNTH_FIELD_BUS_DRIVE, &patch->bus_drive);

  Napi::Value routings = obj.Get("modRoutings");
  if (routings.IsArray()) {
    Napi::Array arr = routings.As<Napi::Array>();
    const uint32_t count = arr.Length();
    if (count > SONARE_SYNTH_PATCH_MOD_ROUTINGS) {
      Napi::RangeError::New(env, "a synth patch supports at most 8 mod routings")
          .ThrowAsJavaScriptException();
      return false;
    }
    patch->num_mod_routings = static_cast<int>(count);
    // An explicitly supplied array — including an empty one — replaces the base
    // matrix; omitting the key keeps it.
    patch->present_fields |= SONARE_SYNTH_FIELD_MOD_ROUTINGS;
    for (uint32_t i = 0; i < count; ++i) {
      Napi::Value entry = arr.Get(i);
      if (!entry.IsObject()) {
        Napi::TypeError::New(env, "mod routings must be objects").ThrowAsJavaScriptException();
        return false;
      }
      Napi::Object routing = entry.As<Napi::Object>();
      SonareSynthModRouting& out = patch->mod_routings[i];
      if (!SynthEnumProperty(env, routing, "source", kSynthModSources,
                             SONARE_SYNTH_MOD_SOURCE_COUNT, "mod source", &out.source) ||
          !SynthEnumProperty(env, routing, "destination", kSynthModDestinations,
                             SONARE_SYNTH_MOD_DESTINATION_COUNT, "mod destination",
                             &out.destination)) {
        return false;
      }
      out.depth = FloatProperty(routing, "depth", 0.0f);
    }
  }
  return true;
}

// Converts a versioned C synth patch into the JS SynthPatch object shape
// (the read direction for synthPresetPatch): enum ordinals become their
// canonical names so the object can be passed back verbatim.
inline Napi::Object SynthPatchToObject(Napi::Env env, const SonareSynthPatch& patch) {
  auto enum_name = [&env](int value, const char* const* names, int count) -> Napi::Value {
    if (value >= 0 && value < count) return Napi::String::New(env, names[value]);
    return Napi::Number::New(env, value);
  };
  Napi::Object out = Napi::Object::New(env);
  out.Set("preset", Napi::String::New(env, patch.preset));
  out.Set("engineMode",
          enum_name(patch.engine_mode, kSynthEngineModes, SONARE_SYNTH_ENGINE_MODE_COUNT));
  out.Set("waveform", enum_name(patch.waveform, kSynthWaveforms, SONARE_SYNTH_OSC_WAVEFORM_COUNT));
  out.Set("unison", patch.unison);
  out.Set("detuneCents", patch.detune_cents);
  out.Set("driftCents", patch.drift_cents);
  out.Set("drive", patch.drive);
  out.Set("filterModel",
          enum_name(patch.filter_model, kSynthFilterModels, SONARE_SYNTH_FILTER_MODEL_COUNT));
  out.Set("filterOutput",
          enum_name(patch.filter_output, kSynthFilterOutputs, SONARE_SYNTH_FILTER_OUTPUT_COUNT));
  out.Set("cutoffHz", patch.cutoff_hz);
  out.Set("resonanceQ", patch.resonance_q);
  out.Set("keyTrack", patch.key_track);
  out.Set("envToCutoffCents", patch.env_to_cutoff_cents);
  out.Set("velToCutoffCents", patch.vel_to_cutoff_cents);
  out.Set("ampAttackMs", patch.amp_attack_ms);
  out.Set("ampDecayMs", patch.amp_decay_ms);
  out.Set("ampSustain", patch.amp_sustain);
  out.Set("ampReleaseMs", patch.amp_release_ms);
  out.Set("filterAttackMs", patch.filter_attack_ms);
  out.Set("filterDecayMs", patch.filter_decay_ms);
  out.Set("filterSustain", patch.filter_sustain);
  out.Set("filterReleaseMs", patch.filter_release_ms);
  out.Set("lfoRateHz", patch.lfo_rate_hz);
  out.Set("lfoToPitchCents", patch.lfo_to_pitch_cents);
  out.Set("lfo2RateHz", patch.lfo2_rate_hz);
  out.Set("glideMs", patch.glide_ms);
  out.Set("body", enum_name(patch.body, kSynthBodyTypes, SONARE_SYNTH_BODY_TYPE_COUNT));
  out.Set("bodyMix", patch.body_mix);
  out.Set("stereoSpread", patch.stereo_spread);
  Napi::Array routings = Napi::Array::New(
      env, static_cast<size_t>(patch.num_mod_routings > 0 ? patch.num_mod_routings : 0));
  for (int i = 0; i < patch.num_mod_routings && i < SONARE_SYNTH_PATCH_MOD_ROUTINGS; ++i) {
    Napi::Object routing = Napi::Object::New(env);
    routing.Set("source", enum_name(patch.mod_routings[i].source, kSynthModSources,
                                    SONARE_SYNTH_MOD_SOURCE_COUNT));
    routing.Set("destination", enum_name(patch.mod_routings[i].destination, kSynthModDestinations,
                                         SONARE_SYNTH_MOD_DESTINATION_COUNT));
    routing.Set("depth", patch.mod_routings[i].depth);
    routings.Set(static_cast<uint32_t>(i), routing);
  }
  out.Set("modRoutings", routings);
  out.Set("gain", patch.gain);
  out.Set("polyphony", patch.polyphony);
  out.Set("busDrive", patch.bus_drive);
  return out;
}

}  // namespace sonare_node

#endif  // SONARE_NODE_SONARE_WRAP_SYNTH_PATCH_H_
