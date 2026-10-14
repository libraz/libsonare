#pragma once

/// @file synth_patch_val.h
/// @brief JS <-> SonareSynthPatch conversion shared by the WASM project and
///        realtime-engine TUs. The object field names and enum-name strings
///        agree with the Node and Python facades.

#ifdef __EMSCRIPTEN__

#include <emscripten/val.h>
#include <sonare/sonare_c_engine.h>
#include <sonare/sonare_c_project.h>
#include <sonare/sonare_c_types.h>

#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "c_api/synth_enum_names.h"
#include "wasm/bindings/common/common.h"

namespace sonare_wasm_synth {

// The spellings are the C ABI's own tables; these names keep the call sites short.
namespace synth_names = sonare::c_api::synth_names;
inline constexpr const auto& kEngineModes = synth_names::kEngineModes;
inline constexpr const auto& kSampleLoopModes = synth_names::kSampleLoopModes;
inline constexpr const auto& kSampleKeyTracks = synth_names::kSampleKeyTracks;
inline constexpr const auto& kWaveforms = synth_names::kWaveforms;
inline constexpr const auto& kFilterModels = synth_names::kFilterModels;
inline constexpr const auto& kFilterOutputs = synth_names::kFilterOutputs;
inline constexpr const auto& kBodyTypes = synth_names::kBodyTypes;
inline constexpr const auto& kRetriggers = synth_names::kRetriggers;
inline constexpr const auto& kModSources = synth_names::kModSources;
inline constexpr const auto& kModDestinations = synth_names::kModDestinations;
inline constexpr const auto& kControllerInputs = synth_names::kControllerInputs;
inline constexpr const auto& kControllerAxes = synth_names::kControllerAxes;
inline constexpr const auto& kArticulations = synth_names::kArticulations;
inline constexpr const auto& kMpeDimensions = synth_names::kMpeDimensions;
inline constexpr const auto& kNoteTrackings = synth_names::kNoteTrackings;

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
  out.set("controllerInputs",
          array_from(sonare_synth_enum_names(SONARE_SYNTH_ENUM_CONTROLLER_INPUT)));
  out.set("controllerAxes", array_from(sonare_synth_enum_names(SONARE_SYNTH_ENUM_CONTROLLER_AXIS)));
  out.set("articulations", array_from(sonare_synth_enum_names(SONARE_SYNTH_ENUM_ARTICULATION)));
  out.set("mpeDimensions", array_from(sonare_synth_enum_names(SONARE_SYNTH_ENUM_MPE_DIMENSION)));
  out.set("noteTrackings", array_from(sonare_synth_enum_names(SONARE_SYNTH_ENUM_NOTE_TRACKING)));
  out.set("sampleLoopModes",
          array_from(sonare_synth_enum_names(SONARE_SYNTH_ENUM_SAMPLE_LOOP_MODE)));
  out.set("sampleKeyTracks",
          array_from(sonare_synth_enum_names(SONARE_SYNTH_ENUM_SAMPLE_KEY_TRACK)));
  out.set("retriggers", array_from(sonare_synth_enum_names(SONARE_SYNTH_ENUM_RETRIGGER)));
  return out;
}

/// enumProperty for a field that has no "keep base" reading, so absence is
/// refused rather than defaulted: where ordinal 0 is itself a working value, a
/// caller who omitted the key would get a selection they never asked for.
inline void requiredEnumProperty(emscripten::val object, const char* key, const char* const* names,
                                 int count, const char* what, int* out);

/// Resolves an enum value accepting the C ordinal or a name; throws on an
/// unknown name and on an ordinal outside [0, @p count). Shared with @ref
/// enumProperty so a standalone argument and an object field read a spelling on
/// identical terms.
inline int enumFromVal(emscripten::val value, const char* const* names, int count,
                       const char* what) {
  if (value.typeOf().as<std::string>() == "string") {
    const std::string name = value.as<std::string>();
    for (int i = 0; i < count; ++i) {
      if (name == names[i]) return i;
    }
    const int alias = synth_names::input_alias(names, name);
    if (alias >= 0) return alias;
    throw WasmRangeError(std::string("Unknown ") + what + " name: '" + name + "'");
  }
  // Reject non-number, non-string values (e.g. a boolean) instead of coercing
  // them to a bogus ordinal, matching the Node addon's enum reader.
  if (value.typeOf().as<std::string>() != "number") {
    throw WasmTypeError(std::string("Expected ") + what + " to be a number or string");
  }
  // The name path cannot spell an ordinal outside the table, so the numeric path
  // must not either: mod source and destination land in a struct field the core
  // reads without re-checking, where 9, 10, 99 and -1 all render as "none".
  const int ordinal = checkedIntFromVal(value, what);
  requireOrdinalInRange(ordinal, 0, count - 1, what);
  return ordinal;
}

/// Spells an enum ordinal as its canonical name, or hands back the ordinal
/// itself when the table cannot spell it. The surface's one reading of an enum
/// value back out, so a patch field and a standalone getter agree.
inline emscripten::val enumNameVal(int value, const char* const* names, int count) {
  if (value >= 0 && value < count) return emscripten::val(std::string(names[value]));
  return emscripten::val(value);
}

/// Reads an enum field accepting the C ordinal or a name; throws on an unknown
/// name and on an ordinal outside [0, @p count). Absent fields keep @p out
/// unchanged (0 = "keep base").
inline void enumProperty(emscripten::val object, const char* key, const char* const* names,
                         int count, const char* what, int* out) {
  if (!hasProperty(object, key)) return;
  *out = enumFromVal(object[key], names, count, what);
}

inline void requiredEnumProperty(emscripten::val object, const char* key, const char* const* names,
                                 int count, const char* what, int* out) {
  if (!hasProperty(object, key)) {
    throw WasmTypeError(std::string("Missing required ") + what + ": '" + key + "'");
  }
  enumProperty(object, key, names, count, what, out);
}

inline void setPresetName(SonareSynthPatch* patch, const std::string& name) {
  const std::string bare = name.rfind("va:", 0) == 0 ? name.substr(3) : name;
  std::strncpy(patch->preset, wasmCString(bare, "preset"), SONARE_SYNTH_PRESET_NAME_MAX - 1);
}

/// A parsed patch together with the memory its @c engine_params pointer
/// borrows. The keys are NUL-terminated @c std::vector<char> buffers rather than
/// strings: a moved vector keeps its heap buffer, whereas a short string keeps
/// its bytes inline and would dangle when the storage is moved into a caller's
/// container. The pointers are fixed once at construction, so a move is safe and
/// a copy (which would alias them) is not offered. Keep the storage alive across
/// the C call that reads @c patch.
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

/// Reads `engineParams` (a plain key -> number object) into @p storage. The
/// value type is checked here; range, integrality and key validity are the C
/// ABI's to refuse, so the message and bounds have one source.
inline void readEngineParams(emscripten::val desc, SynthPatchStorage* storage) {
  using emscripten::val;
  if (!hasProperty(desc, "engineParams")) return;
  val map = desc["engineParams"];
  if (map.typeOf().as<std::string>() != "object" ||
      val::global("Array").call<bool>("isArray", map)) {
    throw WasmTypeError("synth patch engineParams must be an object of key -> number");
  }
  val names = val::global("Object").call<val>("keys", map);
  const size_t count = wasmArrayLikeLength(names, "engineParams keys");
  storage->keys.reserve(count);
  storage->params.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const std::string key = names[i].as<std::string>();
    wasmRefuseEmbeddedNul(key, "engineParams key");
    const std::string subject = "engineParams '" + key + "'";
    const double value = numberFromVal(map[key], subject.c_str());
    storage->keys.emplace_back(key.begin(), key.end());
    storage->keys.back().push_back('\0');
    storage->params.push_back({nullptr, value});
  }
  for (size_t i = 0; i < count; ++i) storage->params[i].key = storage->keys[i].data();
  storage->patch.engine_params = storage->params.empty() ? nullptr : storage->params.data();
  storage->patch.engine_param_count = storage->params.size();
}

/// Parses a JS SynthPatch descriptor (a preset-name string — a "va:" routing
/// prefix is accepted — or an object of wrapper-section overrides) into the
/// versioned C struct. A key the caller actually supplied also sets its
/// present_fields bit, so `stereoSpread: 0` reaches the core as an explicit
/// zero rather than the "keep base" sentinel a bare zero would be. Throws on
/// unknown enum names; unknown PRESET names and refused `engineParams` keys or
/// values are validated downstream by the C ABI.
inline SynthPatchStorage synthPatchFromVal(emscripten::val desc) {
  SynthPatchStorage storage;
  SonareSynthPatch& patch = storage.patch;
  patch.struct_version = SONARE_SYNTH_PATCH_STRUCT_VERSION;
  if (desc.isUndefined() || desc.isNull()) return storage;
  if (desc.typeOf().as<std::string>() == "string") {
    setPresetName(&patch, desc.as<std::string>());
    return storage;
  }
  if (desc.typeOf().as<std::string>() != "object") {
    throw WasmTypeError("synth patch must be a preset-name string or an object");
  }
  if (hasProperty(desc, "preset")) {
    emscripten::val preset = desc["preset"];
    if (preset.typeOf().as<std::string>() != "string") {
      throw WasmTypeError("synth patch preset must be a string");
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
  enumProperty(desc, "retrigger", kRetriggers, SONARE_SYNTH_RETRIGGER_COUNT, "retrigger mode",
               &patch.retrigger);
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
  read_float("hpCutoffHz", SONARE_SYNTH_FIELD_HP_CUTOFF_HZ, &patch.hp_cutoff_hz);
  read_float("sampleHoldHz", SONARE_SYNTH_FIELD_SAMPLE_HOLD_HZ, &patch.sample_hold_hz);
  read_float("bitDepth", SONARE_SYNTH_FIELD_BIT_DEPTH, &patch.bit_depth);
  read_float("pitchOffsetCents", SONARE_SYNTH_FIELD_PITCH_OFFSET_CENTS, &patch.pitch_offset_cents);
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

  // Second presence word (struct_version 9).
  auto read_float2 = [&desc, &patch](const char* key, uint32_t bit, float* out) {
    *out = floatProperty(desc, key, 0.0f);
    if (hasProperty(desc, key)) patch.present_fields2 |= bit;
  };
  read_float2("driftRateHz", SONARE_SYNTH_FIELD2_DRIFT_RATE_HZ, &patch.drift_rate_hz);
  read_float2("ampDelayMs", SONARE_SYNTH_FIELD2_AMP_DELAY_MS, &patch.amp_delay_ms);
  read_float2("ampHoldMs", SONARE_SYNTH_FIELD2_AMP_HOLD_MS, &patch.amp_hold_ms);
  read_float2("filterDelayMs", SONARE_SYNTH_FIELD2_FILTER_DELAY_MS, &patch.filter_delay_ms);
  read_float2("filterHoldMs", SONARE_SYNTH_FIELD2_FILTER_HOLD_MS, &patch.filter_hold_ms);
  patch.one_shot = boolProperty(desc, "oneShot", false) ? 1 : 0;
  if (hasProperty(desc, "oneShot")) patch.present_fields2 |= SONARE_SYNTH_FIELD2_ONE_SHOT;

  // Sample engine. No presence bits: only a sample patch reads this block, so
  // set 0 stays addressable without one.
  patch.sample_set = intProperty(desc, "sampleSet", 0);
  patch.sample_level = floatProperty(desc, "sampleLevel", 0.0f);
  patch.sample_start_offset = floatProperty(desc, "sampleStartOffset", 0.0f);
  enumProperty(desc, "sampleLoop", kSampleLoopModes, SONARE_SAMPLE_LOOP_MODE_COUNT,
               "sample loop mode", &patch.sample_loop);
  enumProperty(desc, "sampleKeyTrack", kSampleKeyTracks, SONARE_SAMPLE_KEY_TRACK_COUNT,
               "sample key track", &patch.sample_key_track);

  if (hasProperty(desc, "modRoutings")) {
    emscripten::val routings = desc["modRoutings"];
    if (emscripten::val::global("Array").call<bool>("isArray", routings)) {
      const size_t count = routings["length"].as<size_t>();
      if (count > SONARE_SYNTH_PATCH_MOD_ROUTINGS) {
        throw WasmRangeError("a synth patch supports at most 8 mod routings");
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
  readEngineParams(desc, &storage);
  return storage;
}

/// Converts a versioned C synth patch into the JS SynthPatch object shape
/// (enum ordinals become their canonical names, so the object can be passed
/// back verbatim).
inline emscripten::val synthPatchToVal(const SonareSynthPatch& patch) {
  using emscripten::val;
  val out = val::object();
  out.set("preset", std::string(patch.preset));
  out.set("engineMode",
          enumNameVal(patch.engine_mode, kEngineModes, SONARE_SYNTH_ENGINE_MODE_COUNT));
  out.set("waveform", enumNameVal(patch.waveform, kWaveforms, SONARE_SYNTH_OSC_WAVEFORM_COUNT));
  out.set("unison", patch.unison);
  out.set("detuneCents", patch.detune_cents);
  out.set("driftCents", patch.drift_cents);
  out.set("drive", patch.drive);
  out.set("filterModel",
          enumNameVal(patch.filter_model, kFilterModels, SONARE_SYNTH_FILTER_MODEL_COUNT));
  out.set("filterOutput",
          enumNameVal(patch.filter_output, kFilterOutputs, SONARE_SYNTH_FILTER_OUTPUT_COUNT));
  out.set("cutoffHz", patch.cutoff_hz);
  out.set("hpCutoffHz", patch.hp_cutoff_hz);
  out.set("sampleHoldHz", patch.sample_hold_hz);
  out.set("bitDepth", patch.bit_depth);
  out.set("pitchOffsetCents", patch.pitch_offset_cents);
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
  out.set("body", enumNameVal(patch.body, kBodyTypes, SONARE_SYNTH_BODY_TYPE_COUNT));
  out.set("bodyMix", patch.body_mix);
  out.set("stereoSpread", patch.stereo_spread);
  out.set("retrigger", enumNameVal(patch.retrigger, kRetriggers, SONARE_SYNTH_RETRIGGER_COUNT));
  out.set("driftRateHz", patch.drift_rate_hz);
  out.set("ampDelayMs", patch.amp_delay_ms);
  out.set("ampHoldMs", patch.amp_hold_ms);
  out.set("filterDelayMs", patch.filter_delay_ms);
  out.set("filterHoldMs", patch.filter_hold_ms);
  out.set("oneShot", patch.one_shot != 0);
  val routings = val::array();
  for (int i = 0; i < patch.num_mod_routings && i < SONARE_SYNTH_PATCH_MOD_ROUTINGS; ++i) {
    val routing = val::object();
    routing.set("source", enumNameVal(patch.mod_routings[i].source, kModSources,
                                      SONARE_SYNTH_MOD_SOURCE_COUNT));
    routing.set("destination", enumNameVal(patch.mod_routings[i].destination, kModDestinations,
                                           SONARE_SYNTH_MOD_DESTINATION_COUNT));
    routing.set("depth", patch.mod_routings[i].depth);
    routings.call<void>("push", routing);
  }
  out.set("modRoutings", routings);
  out.set("gain", patch.gain);
  out.set("polyphony", patch.polyphony);
  out.set("busDrive", patch.bus_drive);
  out.set("sampleSet", patch.sample_set);
  out.set("sampleLevel", patch.sample_level);
  out.set("sampleLoop",
          enumNameVal(patch.sample_loop, kSampleLoopModes, SONARE_SAMPLE_LOOP_MODE_COUNT));
  out.set("sampleStartOffset", patch.sample_start_offset);
  out.set("sampleKeyTrack",
          enumNameVal(patch.sample_key_track, kSampleKeyTracks, SONARE_SAMPLE_KEY_TRACK_COUNT));
  return out;
}

}  // namespace sonare_wasm_synth

#endif  // __EMSCRIPTEN__
