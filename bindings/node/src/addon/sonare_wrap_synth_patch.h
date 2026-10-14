#ifndef SONARE_NODE_SONARE_WRAP_SYNTH_PATCH_H_
#define SONARE_NODE_SONARE_WRAP_SYNTH_PATCH_H_

#include <napi.h>
#include <sonare/sonare_c.h>

#include <cmath>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "c_api/synth_enum_names.h"
#include "sonare_wrap_options.h"

namespace sonare_node {

// The spellings are the C ABI's own tables; these names keep the call sites short.
namespace synth_names = sonare::c_api::synth_names;
inline constexpr const auto& kSynthEngineModes = synth_names::kEngineModes;
inline constexpr const auto& kSampleLoopModes = synth_names::kSampleLoopModes;
inline constexpr const auto& kSampleKeyTracks = synth_names::kSampleKeyTracks;
inline constexpr const auto& kSynthWaveforms = synth_names::kWaveforms;
inline constexpr const auto& kSynthFilterModels = synth_names::kFilterModels;
inline constexpr const auto& kSynthFilterOutputs = synth_names::kFilterOutputs;
inline constexpr const auto& kSynthBodyTypes = synth_names::kBodyTypes;
inline constexpr const auto& kSynthRetriggers = synth_names::kRetriggers;
inline constexpr const auto& kSynthModSources = synth_names::kModSources;
inline constexpr const auto& kSynthModDestinations = synth_names::kModDestinations;
inline constexpr const auto& kPartRigModes = synth_names::kPartRigModes;
inline constexpr int kPartRigModeCount = synth_names::kPartRigModeCount;

// SonareSynthPatch enum-name maps (shared by the project bounce and realtime
// engine TUs; the names agree with the Python / WASM facades). 0 / "default"
// keeps the base patch's value per the C ABI contract.
inline int SynthEnumFromName(const std::string& name, const char* const* names, int count) {
  for (int i = 0; i < count; ++i) {
    if (name == names[i]) return i;
  }
  return synth_names::input_alias(names, name);
}

// Resolves an enum value that accepts the C ordinal or a name. Returns false
// with a pending JS RangeError for an unknown name or an ordinal outside
// [0, count), and a TypeError for a value that is neither a string nor a number.
inline bool SynthEnumValue(Napi::Env env, const Napi::Value& value, const char* const* names,
                           int count, const char* what, int* out) {
  if (value.IsString()) {
    const std::string name = node_narrow_string(env, value, what);
    const int mapped = SynthEnumFromName(name, names, count);
    if (mapped < 0) {
      Napi::RangeError::New(env, std::string("Unknown ") + what + " name: '" + name + "'")
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
  // Checked before the int read, because Int32Value() WRAPS: 2^32 + 2 arrived as
  // 2 and rendered a patch the caller never sent. @p count is the C ABI's own
  // enumerator count, so the domain widens with the enum rather than with a
  // literal here.
  const double number = value.As<Napi::Number>().DoubleValue();
  if (!std::isfinite(number) || std::trunc(number) != number || number < 0.0 ||
      number >= static_cast<double>(count)) {
    Napi::RangeError::New(env, std::string(what) + " must be a name or an ordinal in [0, " +
                                   std::to_string(count) + ")")
        .ThrowAsJavaScriptException();
    return false;
  }
  *out = static_cast<int>(number);
  return true;
}

// Reads an enum field through SynthEnumValue. An absent field leaves @p out at
// whatever the caller seeded it with.
inline bool SynthEnumProperty(Napi::Env env, const Napi::Object& obj, const char* key,
                              const char* const* names, int count, const char* what, int* out) {
  const Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) return true;
  return SynthEnumValue(env, value, names, count, what, out);
}

// Spells an enum ordinal as its canonical name, so a value read back out of the
// C ABI can be handed straight back in. An ordinal past the table stays a number
// rather than becoming a wrong name.
inline Napi::Value SynthEnumName(Napi::Env env, int value, const char* const* names, int count) {
  if (value >= 0 && value < count) return Napi::String::New(env, names[value]);
  return Napi::Number::New(env, value);
}

// True when the descriptor carries the key with a usable value. JS absence and
// an explicit undefined/null are the same thing here, so only a real value
// marks the field present.
inline bool SynthFieldPresent(const Napi::Object& obj, const char* key) {
  if (!obj.Has(key)) return false;
  const Napi::Value value = obj.Get(key);
  return !value.IsUndefined() && !value.IsNull();
}

// Parses the wrapper-section fields of a JS SynthPatch descriptor (a preset-name string — a "va:"
// routing prefix is accepted — or an object of wrapper-section overrides) into the versioned C
// struct. A key the caller actually supplied also sets its present_fields bit, so `stereoSpread: 0`
// reaches the core as an explicit zero rather than the "keep base" sentinel a bare zero would be.
// Returns false with a pending JS exception on an unknown enum name; unknown PRESET names are
// validated downstream by the C ABI.
inline bool ReadSynthPatchFields(Napi::Env env, const Napi::Value& desc, SonareSynthPatch* patch) {
  *patch = SonareSynthPatch{};
  // Version 5 is a superset: the sample block at the tail is read only when the
  // resolved engine is the sample engine, and the series highpass and converter
  // past it are inert at zero, so a zero-filled tail is exactly version 2's
  // behaviour.
  patch->struct_version = SONARE_SYNTH_PATCH_STRUCT_VERSION;
  if (desc.IsUndefined() || desc.IsNull()) return true;

  auto set_preset = [patch](const std::string& name) {
    const std::string bare = name.rfind("va:", 0) == 0 ? name.substr(3) : name;
    std::strncpy(patch->preset, bare.c_str(), SONARE_SYNTH_PRESET_NAME_MAX - 1);
  };
  if (desc.IsString()) {
    set_preset(node_narrow_string(env, desc, "preset"));
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
      set_preset(node_narrow_string(env, preset, "preset"));
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
                         "body type", &patch->body) ||
      !SynthEnumProperty(env, obj, "retrigger", kSynthRetriggers, SONARE_SYNTH_RETRIGGER_COUNT,
                         "retrigger mode", &patch->retrigger) ||
      !SynthEnumProperty(env, obj, "sampleLoop", kSampleLoopModes, SONARE_SAMPLE_LOOP_MODE_COUNT,
                         "sample loop mode", &patch->sample_loop) ||
      !SynthEnumProperty(env, obj, "sampleKeyTrack", kSampleKeyTracks,
                         SONARE_SAMPLE_KEY_TRACK_COUNT, "sample key track",
                         &patch->sample_key_track)) {
    return false;
  }
  auto read_float = [&](const char* key, uint32_t bit, float* out) {
    *out = FloatProperty(obj, key, 0.0f);
    if (SynthFieldPresent(obj, key)) patch->present_fields |= bit;
  };
  auto read_int = [&](const char* key, uint32_t bit, int* out) {
    *out = IntProperty(obj, key, kZeroIsSentinel);
    if (SynthFieldPresent(obj, key)) patch->present_fields |= bit;
  };
  read_int("unison", SONARE_SYNTH_FIELD_UNISON, &patch->unison);
  read_float("detuneCents", SONARE_SYNTH_FIELD_DETUNE_CENTS, &patch->detune_cents);
  read_float("driftCents", SONARE_SYNTH_FIELD_DRIFT_CENTS, &patch->drift_cents);
  read_float("drive", SONARE_SYNTH_FIELD_DRIVE, &patch->drive);
  read_float("cutoffHz", SONARE_SYNTH_FIELD_CUTOFF_HZ, &patch->cutoff_hz);
  read_float("hpCutoffHz", SONARE_SYNTH_FIELD_HP_CUTOFF_HZ, &patch->hp_cutoff_hz);
  read_float("sampleHoldHz", SONARE_SYNTH_FIELD_SAMPLE_HOLD_HZ, &patch->sample_hold_hz);
  read_float("bitDepth", SONARE_SYNTH_FIELD_BIT_DEPTH, &patch->bit_depth);
  read_float("pitchOffsetCents", SONARE_SYNTH_FIELD_PITCH_OFFSET_CENTS, &patch->pitch_offset_cents);
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

  // Second presence word (struct_version 9).
  auto read_float2 = [&](const char* key, uint32_t bit, float* out) {
    *out = FloatProperty(obj, key, 0.0f);
    if (SynthFieldPresent(obj, key)) patch->present_fields2 |= bit;
  };
  read_float2("driftRateHz", SONARE_SYNTH_FIELD2_DRIFT_RATE_HZ, &patch->drift_rate_hz);
  read_float2("ampDelayMs", SONARE_SYNTH_FIELD2_AMP_DELAY_MS, &patch->amp_delay_ms);
  read_float2("ampHoldMs", SONARE_SYNTH_FIELD2_AMP_HOLD_MS, &patch->amp_hold_ms);
  read_float2("filterDelayMs", SONARE_SYNTH_FIELD2_FILTER_DELAY_MS, &patch->filter_delay_ms);
  read_float2("filterHoldMs", SONARE_SYNTH_FIELD2_FILTER_HOLD_MS, &patch->filter_hold_ms);
  patch->one_shot = BoolProperty(obj, "oneShot", false) ? 1 : 0;
  if (SynthFieldPresent(obj, "oneShot")) patch->present_fields2 |= SONARE_SYNTH_FIELD2_ONE_SHOT;

  // Sample engine (struct_version 3). No presence bits: only a sample patch
  // reads this block, which is what keeps keymap set 0 addressable as a plain
  // zero, and the other three keep the usual "0 => base".
  patch->sample_set = IntProperty(obj, "sampleSet", 0);
  patch->sample_level = FloatProperty(obj, "sampleLevel", 0.0f);
  patch->sample_start_offset = FloatProperty(obj, "sampleStartOffset", 0.0f);
  if (env.IsExceptionPending()) return false;

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

// A parsed patch together with the engine-param list its `engine_params`
// pointer borrows. Keys are char vectors rather than strings: a moved vector
// keeps its heap buffer, while a short string's bytes live inline and would
// dangle once the storage is moved into a caller's container. The pointers are
// fixed up once, so the storage is move-only and must outlive the C call.
struct SynthPatchStorage {
  SonareSynthPatch patch{};
  std::vector<std::vector<char>> keys;
  std::vector<SonareSynthEngineParam> params;

  SynthPatchStorage() = default;
  SynthPatchStorage(const SynthPatchStorage&) = delete;
  SynthPatchStorage& operator=(const SynthPatchStorage&) = delete;
  SynthPatchStorage(SynthPatchStorage&&) noexcept = default;
  SynthPatchStorage& operator=(SynthPatchStorage&&) noexcept = default;
};

// Parses a JS SynthPatch descriptor into @p storage, including the
// `engineParams` map (key -> number) that sets the selected engine's own model
// fields. A non-number value is refused by key; an unknown key or an
// out-of-range value is refused by the C ABI when the patch is bound.
inline bool ReadSynthPatch(Napi::Env env, const Napi::Value& desc, SynthPatchStorage* storage) {
  *storage = SynthPatchStorage{};
  if (!ReadSynthPatchFields(env, desc, &storage->patch)) return false;
  if (!desc.IsObject()) return true;
  const auto entries = NumberMapProperty(desc.As<Napi::Object>(), "engineParams");
  if (entries.empty()) return true;
  storage->keys.reserve(entries.size());
  storage->params.reserve(entries.size());
  for (const auto& [key, value] : entries) {
    storage->keys.emplace_back(key.begin(), key.end());
    storage->keys.back().push_back('\0');
    storage->params.push_back({storage->keys.back().data(), value});
  }
  storage->patch.engine_params = storage->params.data();
  storage->patch.engine_param_count = storage->params.size();
  return true;
}

// Converts a versioned C synth patch into the JS SynthPatch object shape
// (the read direction for synthPresetPatch): enum ordinals become their
// canonical names so the object can be passed back verbatim. The engine section
// stays out: a patch reads back without `engineParams`.
inline Napi::Object SynthPatchToObject(Napi::Env env, const SonareSynthPatch& patch) {
  auto enum_name = [&env](int value, const char* const* names, int count) -> Napi::Value {
    return SynthEnumName(env, value, names, count);
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
  out.Set("hpCutoffHz", patch.hp_cutoff_hz);
  out.Set("sampleHoldHz", patch.sample_hold_hz);
  out.Set("bitDepth", patch.bit_depth);
  out.Set("pitchOffsetCents", patch.pitch_offset_cents);
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
  out.Set("retrigger", enum_name(patch.retrigger, kSynthRetriggers, SONARE_SYNTH_RETRIGGER_COUNT));
  out.Set("driftRateHz", patch.drift_rate_hz);
  out.Set("ampDelayMs", patch.amp_delay_ms);
  out.Set("ampHoldMs", patch.amp_hold_ms);
  out.Set("filterDelayMs", patch.filter_delay_ms);
  out.Set("filterHoldMs", patch.filter_hold_ms);
  out.Set("oneShot", Napi::Boolean::New(env, patch.one_shot != 0));
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
  out.Set("sampleSet", patch.sample_set);
  out.Set("sampleLevel", patch.sample_level);
  out.Set("sampleLoop",
          enum_name(patch.sample_loop, kSampleLoopModes, SONARE_SAMPLE_LOOP_MODE_COUNT));
  out.Set("sampleStartOffset", patch.sample_start_offset);
  out.Set("sampleKeyTrack",
          enum_name(patch.sample_key_track, kSampleKeyTracks, SONARE_SAMPLE_KEY_TRACK_COUNT));
  return out;
}

}  // namespace sonare_node

#endif  // SONARE_NODE_SONARE_WRAP_SYNTH_PATCH_H_
