/// @file mastering_api.cpp
/// @brief Embind bindings for named mastering processors, presets, assistant, and streaming preview
/// APIs.

#ifdef __EMSCRIPTEN__

#include <array>

#include "c_api/sonare_c_internal.h"
#include "core/audio_encode.h"
#include "mastering/api/chain.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "mastering/assistant/config_from_params.h"
#include "mastering/assistant/platform_targets.h"
#include "mastering/match/ab_switcher.h"
#include "midi/synth/synth_presets.h"
#include "mixing/api/presets.h"
#include "playback/config.h"
#include "sonare.h"
#include "util/json.h"
#include "util/parameter_checks.h"
#include "wasm/bindings/common/common.h"
#include "wasm/bindings/mastering/chain_result.h"

val js_mastering_processor_names() { return stringVectorToVal(mastering::api::processor_names()); }

// Read-only catalog for the named amp rigs used by saturation.ampSim, as a
// JSON array string. The TS facade parses it. Keep the values resolved by the
// core preset registry rather than duplicating them in a binding table: the
// Studio UI can display a preset's actual starting knobs while `presetIndex`
// and sparse overrides remain the source of DSP truth.
std::string js_mastering_amp_preset_catalog() { return mastering::api::amp_preset_catalog_json(); }

// Names of the insert processors the mastering chain can instantiate by name
// (mastering::api::insert_factory_names). Mirrors the C ABI
// sonare_mastering_insert_names (which joins this list) as a string[].
val js_mastering_insert_names() {
  return stringVectorToVal(mastering::api::insert_factory_names());
}

// Parameter names a given insert processor reads (mastering::api::insert_param_names).
// The library refuses a key not in this list, and a name outside the insert registry with
// "unknown insert processor: <id>".
val js_mastering_insert_param_names(std::string name) {
  wasmRefuseEmbeddedNul(name, "name");
  return stringVectorToVal(mastering::api::insert_param_names(name));
}

// Parameter descriptors for the keys an insert reads, ordered by automation id then by name; the
// field contract is sonare_mastering_insert_param_info's. The library refuses a name outside the
// processor catalog, then a supplied rate outside the audio sample-rate domain. An omitted rate
// selects the rate-less query; a supplied rate resolves the Nyquist ceilings.
std::string js_mastering_insert_param_info(std::string name, const val& sample_rate_val) {
  wasmRefuseEmbeddedNul(name, "name");
  if (sample_rate_val.isUndefined() || sample_rate_val.isNull()) {
    return mastering::api::insert_param_info_json(name);
  }
  return mastering::api::insert_param_info_json_at_rate(
      name, numberFromVal(sample_rate_val, "sampleRate"));
}

// Latency and tail an insert reports once built from `json_params` and
// prepared at `sample_rate`, as {latencySamples, tailSamples}
// (mastering::api::insert_timing). Throws for an unknown `name`, a key the
// insert does not read, or a value its construction or prepare refuses; see
// sonare_mastering_insert_timing for the exact error text.
val js_mastering_insert_timing(std::string name, std::string json_params,
                               const val& sample_rate_val) {
  wasmRefuseEmbeddedNul(name, "name");
  // The library refuses the name, the parameter bag under `params`, then the rate.
  const mastering::api::InsertTiming timing = mastering::api::insert_timing(
      name, json_params, numberFromVal(sample_rate_val, "sampleRate"));
  val out = val::object();
  out.set("latencySamples", timing.latency_samples);
  out.set("tailSamples", timing.tail_samples);
  return out;
}

// The number the enum-valued mastering parameter `key` carries for `choice`, or null when `key`
// is not an enum (the facade refuses that as a wrong type). `processor` is empty for a flat chain
// key. Throws for an unknown choice; see sonare_mastering_enum_value.
val js_mastering_enum_value(std::string processor, std::string key, std::string choice) {
  const std::optional<double> value = mastering::api::mastering_enum_value(processor, key, choice);
  return value.has_value() ? val(*value) : val::null();
}

// Machine-readable classification catalog for every named processor id, as a JSON
// array string [{"id","kind","realtimeInsertable","stereoOnly"}, ...]. The TS
// facade parses it; lets a host filter a processor picker by realtime
// insertability instead of offering ids the realtime strip would reject.
std::string js_mastering_processor_catalog() { return mastering::api::processor_catalog_json(); }

namespace {

sonare::util::json::Array catalog_name_array(const std::vector<std::string>& names) {
  sonare::util::json::Array values;
  values.reserve(names.size());
  for (const auto& name : names) values.emplace_back(name);
  return values;
}

sonare::util::json::Array catalog_synth_preset_names() {
  sonare::util::json::Array values;
#if defined(SONARE_WITH_ARRANGEMENT)
  const size_t count = midi::synth::synth_preset_count();
  values.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    if (const auto* preset = midi::synth::synth_preset_at(index); preset != nullptr) {
      values.emplace_back(preset->name);
    }
  }
#endif
  return values;
}

sonare::util::json::Array catalog_voice_changer_preset_names() {
  sonare::util::json::Array values;
#if defined(SONARE_WITH_VOICE_CHANGER)
  return catalog_name_array(editing::voice_changer::realtime_voice_changer_preset_names());
#else
  return values;
#endif
}

}  // namespace

namespace {

// The WASM target deliberately does not link the aggregate C-ABI TU. Build the
// identical schema from the same core registries that back that ABI instead.
std::string build_capability_catalog() {
  namespace json = sonare::util::json;

  json::Object catalog;
  catalog["version"] = SONARE_VERSION_STRING;
  json::Object abi;
  abi["project"] = static_cast<int>(SONARE_PROJECT_ABI_VERSION);
  abi["engine"] = static_cast<int>(sonare::rt::kEngineAbiVersion);
  catalog["abi"] = std::move(abi);
  json::Object encode;
  json::Array builtin_formats;
  builtin_formats.emplace_back("wav");
  encode["builtin"] = std::move(builtin_formats);
  json::Array ffmpeg_formats;
  for (const std::string_view format : sonare::kOptionalAudioEncodeFormats) {
    if (sonare::audio_encode_format_available(format)) {
      ffmpeg_formats.emplace_back(std::string(format));
    }
  }
  encode["ffmpeg"] = std::move(ffmpeg_formats);
  catalog["encode"] = std::move(encode);
  catalog["processors"] = json::parse_strict(mastering::api::processor_catalog_json());
  catalog["chain"] = json::parse_strict(mastering::api::chain_catalog_json());

  json::Object presets;
  presets["mastering"] = catalog_name_array(mastering::api::preset_names());
  presets["synth"] = catalog_synth_preset_names();
#if defined(SONARE_WITH_MIXING)
  presets["mixingScene"] = catalog_name_array(mixing::api::scene_preset_names());
#else
  presets["mixingScene"] = json::Array{};
#endif
  presets["voiceChanger"] = catalog_voice_changer_preset_names();
#if defined(SONARE_WITH_PLAYBACK)
  json::Array playback_room_presets;
  for (auto preset : {playback::RoomPreset::None, playback::RoomPreset::LivingRoom,
                      playback::RoomPreset::HomeTheater, playback::RoomPreset::ScreeningRoom}) {
    playback_room_presets.emplace_back(playback::room_preset_name(preset));
  }
  presets["playbackRoom"] = std::move(playback_room_presets);
#else
  presets["playbackRoom"] = json::Array{};
#endif
  catalog["presets"] = std::move(presets);

  // Mirrors sonare_c_mastering_apply.cpp's masteringPresets entries field for
  // field: name, kind, and the three loudness values that only a Mastering
  // preset carries (null for a Restoration preset, whose repair stages leave
  // level alone).
  json::Array mastering_presets;
  for (const auto& name : mastering::api::preset_names()) {
    const auto preset = mastering::api::preset_from_string(name);
    const auto config = mastering::api::preset_config(preset);
    const bool is_mastering =
        mastering::api::preset_kind(preset) == mastering::api::PresetKind::Mastering;

    json::Object entry;
    entry["name"] = name;
    entry["kind"] = is_mastering ? "mastering" : "restoration";
    entry["targetLufs"] =
        is_mastering ? json::Value(config.loudness.target_lufs) : json::Value(nullptr);
    entry["truePeakCeilingDbtp"] =
        is_mastering ? json::Value(config.loudness.ceiling_dbtp) : json::Value(nullptr);
    entry["maxLimiterGainReductionDb"] =
        is_mastering ? json::Value(config.loudness.max_limiter_gain_reduction_db)
                     : json::Value(nullptr);
    mastering_presets.emplace_back(json::Value(std::move(entry)));
  }
  catalog["masteringPresets"] = std::move(mastering_presets);
  return json::dump(json::Value(std::move(catalog)));
}

}  // namespace

std::string js_capability_catalog() {
  // A pure function of the build, so it is serialized once per module instance.
  static const std::string catalog = build_capability_catalog();
  return catalog;
}

// ---------------------------------------------------------------------------
// Mastering presets (high-level master_audio API).
// Overrides accept a flat object whose keys match `parse_chain_config_params`
// dot-notation (e.g. "loudness.targetLufs"). Numeric and boolean values are
// supported. Pass null/undefined for "preset only".
// ---------------------------------------------------------------------------

val js_mastering_preset_names() { return stringVectorToVal(mastering::api::preset_names()); }

val js_mastering_platform_names() {
  // Read from the shared delivery-target table, so the names a caller can pass
  // as `targetPlatform` are the names the assistant actually accepts.
  return stringVectorToVal(mastering::assistant::platform_names());
}

// A preset's chain config (absent: the chain defaults) with the overrides document applied.
mastering::api::MasteringChainConfig presetConfigWithOverrides(const val& preset,
                                                               const val& overrides) {
  const bool no_preset = preset.isUndefined() || preset.isNull();
  const bool no_overrides = overrides.isUndefined() || overrides.isNull();
  if (no_preset) {
    if (no_overrides) return mastering::api::MasteringChainConfig{};
    return mastering::api::chain_config_from_json(
        stringFromVal(overrides, "overridesJson"),
        mastering::api::chain_params_argument("overrides"));
  }
  const std::string preset_name = stringFromVal(preset, "preset");
  auto config = mastering::api::preset_config(mastering::api::preset_from_string(preset_name));
  if (no_overrides) return config;
  return mastering::api::chain_config_overrides_from_json(
      std::move(config), stringFromVal(overrides, "overridesJson"),
      mastering::api::chain_params_argument("overrides"));
}

val js_master_audio(val preset, val samples, const val& sample_rate_val, val overrides,
                    const val& progress_callback, const val& cancel_callback) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const bool has_progress = callbackGiven(progress_callback, "onProgress");
  const bool has_cancel = callbackGiven(cancel_callback, "cancel");
  std::vector<float> data = float32ArrayToVector(samples, "samples");
  mastering::api::MasteringChain chain(presetConfigWithOverrides(preset, overrides));
  if (!has_progress && !has_cancel) {
    return masteringMonoResultToVal(chain.process_mono(data.data(), data.size(), sample_rate));
  }
  installMasteringChainCallbacks(chain, progress_callback, cancel_callback);
  const auto result = chain.process_mono_cancellable(data.data(), data.size(), sample_rate);
  if (!result) {
    throw SonareException(ErrorCode::Cancelled, "mastering cancelled");
  }
  return masteringMonoResultToVal(*result);
}

val js_master_audio_stereo(val preset, val left_samples, val right_samples,
                           const val& sample_rate_val, val overrides, const val& progress_callback,
                           const val& cancel_callback) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const bool has_progress = callbackGiven(progress_callback, "onProgress");
  const bool has_cancel = callbackGiven(cancel_callback, "cancel");
  validateWasmFloat32ArrayPair(left_samples, "left", right_samples, "right", true);
  std::vector<float> left = float32ArrayToVector(left_samples, "left");
  std::vector<float> right = float32ArrayToVector(right_samples, "right");
  mastering::api::MasteringChain chain(presetConfigWithOverrides(preset, overrides));
  if (!has_progress && !has_cancel) {
    return masteringStereoResultToVal(
        chain.process_stereo(left.data(), right.data(), left.size(), sample_rate));
  }
  installMasteringChainCallbacks(chain, progress_callback, cancel_callback);
  const auto result =
      chain.process_stereo_cancellable(left.data(), right.data(), left.size(), sample_rate);
  if (!result) {
    throw SonareException(ErrorCode::Cancelled, "mastering cancelled");
  }
  return masteringStereoResultToVal(*result);
}

val js_mastering_pair_processor_names() {
  return stringVectorToVal(mastering::api::pair_processor_names());
}

val js_mastering_pair_analysis_names() {
  return stringVectorToVal(mastering::api::pair_analysis_names());
}

val js_mastering_stereo_analysis_names() {
  return stringVectorToVal(mastering::api::stereo_analysis_names());
}

val js_mastering_process(std::string processor_name, val samples, const val& sample_rate_val,
                         val params) {
  wasmRefuseEmbeddedNul(processor_name, "processorName");
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  std::vector<float> data = float32ArrayToVector(samples, "samples");
  validate_offline_audio_input("samples", data.data(), data.size(), sample_rate);
  const auto parsed = masteringParamsFromObject(params);
  auto result = sonare::util::check::under_member("params", [&] {
    return mastering::api::apply_named_processor(processor_name, data.data(), data.size(),
                                                 sample_rate, parsed);
  });
  val out = val::object();
  out.set("samples", vectorToFloat32Array(result.samples));
  out.set("sampleRate", result.sample_rate);
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  out.set("latencySamples", result.latency_samples);
  out.set("loudnessTargetLimited", result.loudness_target_limited);
  out.set("nonFiniteSubstitutionCount", static_cast<double>(result.non_finite_substitution_count));
  setOptionalCount(out, "declipUnreconstructedRuns", result.declip_unreconstructed_runs);
  setOptionalCount(out, "quantizeClippedSamples", result.quantize_clipped_samples);
  return out;
}

val js_mastering_process_stereo(std::string processor_name, val left_samples, val right_samples,
                                const val& sample_rate_val, val params) {
  wasmRefuseEmbeddedNul(processor_name, "processorName");
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateWasmFloat32ArrayPair(left_samples, "left", right_samples, "right", true);
  std::vector<float> left = float32ArrayToVector(left_samples, "left");
  std::vector<float> right = float32ArrayToVector(right_samples, "right");
  validate_offline_audio_input("left", left.data(), left.size(), sample_rate);
  validate_offline_audio_input("right", right.data(), right.size(), sample_rate);
  const auto parsed = masteringParamsFromObject(params);
  auto result = sonare::util::check::under_member("params", [&] {
    return mastering::api::apply_named_processor_stereo(processor_name, left.data(), right.data(),
                                                        left.size(), sample_rate, parsed);
  });
  val out = val::object();
  out.set("left", vectorToFloat32Array(result.left));
  out.set("right", vectorToFloat32Array(result.right));
  out.set("sampleRate", result.sample_rate);
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  out.set("latencySamples", result.latency_samples);
  out.set("loudnessTargetLimited", result.loudness_target_limited);
  out.set("nonFiniteSubstitutionCount", static_cast<double>(result.non_finite_substitution_count));
  setOptionalCount(out, "declipUnreconstructedRuns", result.declip_unreconstructed_runs);
  setOptionalCount(out, "quantizeClippedSamples", result.quantize_clipped_samples);
  return out;
}

val js_mastering_pair_process(std::string processor_name, val source_samples, val reference_samples,
                              const val& sample_rate_val, const val& reference_sample_rate_val,
                              val params) {
  wasmRefuseEmbeddedNul(processor_name, "processorName");
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int reference_sample_rate =
      checkedIntFromVal(reference_sample_rate_val, "referenceSampleRate");
  validateWasmFloat32ArrayPair(source_samples, "source", reference_samples, "reference", false);
  std::vector<float> source = float32ArrayToVector(source_samples, "source");
  std::vector<float> reference = float32ArrayToVector(reference_samples, "reference");
  // source and reference may have independent lengths and rates.
  validate_offline_audio_input("source", source.data(), source.size(), sample_rate);
  validate_offline_audio_input("reference", reference.data(), reference.size(),
                               reference_sample_rate);
  auto result = mastering::api::apply_named_pair_processor(
      processor_name, source.data(), reference.data(), source.size(), reference.size(), sample_rate,
      reference_sample_rate, masteringParamsFromObject(params));
  val out = val::object();
  out.set("samples", vectorToFloat32Array(result.samples));
  out.set("sampleRate", result.sample_rate);
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  out.set("latencySamples", result.latency_samples);
  out.set("nonFiniteSubstitutionCount", static_cast<double>(result.non_finite_substitution_count));
  setOptionalCount(out, "declipUnreconstructedRuns", result.declip_unreconstructed_runs);
  setOptionalCount(out, "quantizeClippedSamples", result.quantize_clipped_samples);
  return out;
}

val js_mastering_pair_process_stereo(std::string processor_name, val source_left_samples,
                                     val source_right_samples, val reference_left_samples,
                                     val reference_right_samples, const val& sample_rate_val,
                                     const val& reference_sample_rate_val, val params) {
  wasmRefuseEmbeddedNul(processor_name, "processorName");
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int reference_sample_rate =
      checkedIntFromVal(reference_sample_rate_val, "referenceSampleRate");
  validateWasmFloat32ArrayPair(source_left_samples, "sourceLeft", source_right_samples,
                               "sourceRight", true);
  validateWasmFloat32ArrayPair(reference_left_samples, "referenceLeft", reference_right_samples,
                               "referenceRight", true);
  validatePlainElementBudget({{&source_left_samples, "sourceLeft"},
                              {&source_right_samples, "sourceRight"},
                              {&reference_left_samples, "referenceLeft"},
                              {&reference_right_samples, "referenceRight"}});
  std::vector<float> source_left = float32ArrayToVector(source_left_samples, "sourceLeft");
  std::vector<float> source_right = float32ArrayToVector(source_right_samples, "sourceRight");
  std::vector<float> reference_left = float32ArrayToVector(reference_left_samples, "referenceLeft");
  std::vector<float> reference_right =
      float32ArrayToVector(reference_right_samples, "referenceRight");
  validate_offline_audio_input("sourceLeft", source_left.data(), source_left.size(), sample_rate);
  validate_offline_audio_input("sourceRight", source_right.data(), source_right.size(),
                               sample_rate);
  validate_offline_audio_input("referenceLeft", reference_left.data(), reference_left.size(),
                               reference_sample_rate);
  validate_offline_audio_input("referenceRight", reference_right.data(), reference_right.size(),
                               reference_sample_rate);
  auto result = mastering::api::apply_named_pair_processor_stereo(
      processor_name, source_left.data(), source_right.data(), source_left.size(),
      reference_left.data(), reference_right.data(), reference_left.size(), sample_rate,
      reference_sample_rate, masteringParamsFromObject(params));
  val out = val::object();
  out.set("left", vectorToFloat32Array(result.left));
  out.set("right", vectorToFloat32Array(result.right));
  out.set("sampleRate", result.sample_rate);
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  out.set("latencySamples", result.latency_samples);
  out.set("loudnessTargetLimited", result.loudness_target_limited);
  out.set("nonFiniteSubstitutionCount", static_cast<double>(result.non_finite_substitution_count));
  setOptionalCount(out, "declipUnreconstructedRuns", result.declip_unreconstructed_runs);
  setOptionalCount(out, "quantizeClippedSamples", result.quantize_clipped_samples);
  return out;
}

std::string js_mastering_pair_analyze(std::string analysis_name, val source_samples,
                                      val reference_samples, const val& sample_rate_val,
                                      const val& reference_sample_rate_val, val params) {
  wasmRefuseEmbeddedNul(analysis_name, "analysisName");
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int reference_sample_rate =
      checkedIntFromVal(reference_sample_rate_val, "referenceSampleRate");
  validateWasmFloat32ArrayPair(source_samples, "source", reference_samples, "reference", false);
  std::vector<float> source = float32ArrayToVector(source_samples, "source");
  std::vector<float> reference = float32ArrayToVector(reference_samples, "reference");
  // source and reference may have independent lengths and rates.
  validate_offline_audio_input("source", source.data(), source.size(), sample_rate);
  validate_offline_audio_input("reference", reference.data(), reference.size(),
                               reference_sample_rate);
  return mastering::api::analyze_named_pair(
      analysis_name, source.data(), reference.data(), source.size(), reference.size(), sample_rate,
      reference_sample_rate, masteringParamsFromObject(params));
}

std::string js_mastering_pair_analyze_stereo(std::string analysis_name, val source_left_samples,
                                             val source_right_samples, val reference_left_samples,
                                             val reference_right_samples,
                                             const val& sample_rate_val,
                                             const val& reference_sample_rate_val, val params) {
  wasmRefuseEmbeddedNul(analysis_name, "analysisName");
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int reference_sample_rate =
      checkedIntFromVal(reference_sample_rate_val, "referenceSampleRate");
  validateWasmFloat32ArrayPair(source_left_samples, "sourceLeft", source_right_samples,
                               "sourceRight", true);
  validateWasmFloat32ArrayPair(reference_left_samples, "referenceLeft", reference_right_samples,
                               "referenceRight", true);
  validatePlainElementBudget({{&source_left_samples, "sourceLeft"},
                              {&source_right_samples, "sourceRight"},
                              {&reference_left_samples, "referenceLeft"},
                              {&reference_right_samples, "referenceRight"}});
  std::vector<float> source_left = float32ArrayToVector(source_left_samples, "sourceLeft");
  std::vector<float> source_right = float32ArrayToVector(source_right_samples, "sourceRight");
  std::vector<float> reference_left = float32ArrayToVector(reference_left_samples, "referenceLeft");
  std::vector<float> reference_right =
      float32ArrayToVector(reference_right_samples, "referenceRight");
  validate_offline_audio_input("sourceLeft", source_left.data(), source_left.size(), sample_rate);
  validate_offline_audio_input("sourceRight", source_right.data(), source_right.size(),
                               sample_rate);
  validate_offline_audio_input("referenceLeft", reference_left.data(), reference_left.size(),
                               reference_sample_rate);
  validate_offline_audio_input("referenceRight", reference_right.data(), reference_right.size(),
                               reference_sample_rate);
  return mastering::api::analyze_named_pair(
      analysis_name, source_left.data(), source_right.data(), source_left.size(),
      reference_left.data(), reference_right.data(), reference_left.size(), sample_rate,
      reference_sample_rate, masteringParamsFromObject(params));
}

// Gain-matches `source` to `reference`'s integrated loudness. The core takes the
// reference first; the C ABI and every facade take (source, reference), so the
// two arguments are swapped here rather than at the call site.
val js_mastering_ab_match_loudness(val source_samples, val reference_samples,
                                   const val& sample_rate_val,
                                   const val& reference_sample_rate_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int reference_sample_rate =
      checkedIntFromVal(reference_sample_rate_val, "referenceSampleRate");
  validateWasmFloat32ArrayPair(source_samples, "source", reference_samples, "reference", false);
  // source and reference may have independent lengths and rates; the core
  // resamples the reference onto the source rate.
  const Audio source = loadValidatedAudio("source", source_samples, sample_rate);
  const Audio reference = loadValidatedAudio("reference", reference_samples, reference_sample_rate);
  const auto matched = mastering::match::ab_match_loudness(reference, source);

  val out = val::object();
  out.set("samples", vectorToFloat32Array(matched.b.data(), matched.b.size()));
  out.set("sampleRate", matched.b.sample_rate());
  out.set("referenceLufs", matched.reference_lufs);
  out.set("sourceLufs", matched.source_lufs);
  out.set("appliedGainDb", matched.applied_gain_db);
  out.set("matchedTruePeakDbtp", matched.matched_true_peak_dbtp);
  return out;
}

val js_mastering_ab_match_loudness_stereo(val source_left_samples, val source_right_samples,
                                          val reference_left_samples, val reference_right_samples,
                                          const val& sample_rate_val,
                                          const val& reference_sample_rate_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int reference_sample_rate =
      checkedIntFromVal(reference_sample_rate_val, "referenceSampleRate");
  validateWasmFloat32ArrayPair(source_left_samples, "sourceLeft", source_right_samples,
                               "sourceRight", true);
  validateWasmFloat32ArrayPair(reference_left_samples, "referenceLeft", reference_right_samples,
                               "referenceRight", true);
  validatePlainElementBudget({{&source_left_samples, "sourceLeft"},
                              {&source_right_samples, "sourceRight"},
                              {&reference_left_samples, "referenceLeft"},
                              {&reference_right_samples, "referenceRight"}});
  const auto source_left = loadValidatedAudio("sourceLeft", source_left_samples, sample_rate);
  const auto source_right = loadValidatedAudio("sourceRight", source_right_samples, sample_rate);
  const auto reference_left =
      loadValidatedAudio("referenceLeft", reference_left_samples, reference_sample_rate);
  const auto reference_right =
      loadValidatedAudio("referenceRight", reference_right_samples, reference_sample_rate);
  const mastering::match::StereoAudioPair source{source_left, source_right};
  const mastering::match::StereoAudioPair reference{reference_left, reference_right};
  // A silent take is refused under the core's side name; the caller passed it as two channels.
  const std::array<sonare_c_detail::CPairName, 2> pairs{
      {{"reference", "referenceLeft", "referenceRight"}, {"source", "sourceLeft", "sourceRight"}}};
  const auto matched = sonare_c_detail::with_c_pair_names(
      pairs, [&] { return mastering::match::ab_match_loudness_stereo(reference, source); });

  val out = val::object();
  out.set("left", vectorToFloat32Array(matched.b.left.data(), matched.b.left.size()));
  out.set("right", vectorToFloat32Array(matched.b.right.data(), matched.b.right.size()));
  out.set("sampleRate", matched.b.left.sample_rate());
  out.set("referenceLufs", matched.reference_lufs);
  out.set("sourceLufs", matched.source_lufs);
  out.set("appliedGainDb", matched.applied_gain_db);
  out.set("matchedTruePeakDbtp", matched.matched_true_peak_dbtp);
  return out;
}

std::string js_mastering_stereo_analyze(std::string analysis_name, val left_samples,
                                        val right_samples, const val& sample_rate_val, val params) {
  wasmRefuseEmbeddedNul(analysis_name, "analysisName");
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateWasmFloat32ArrayPair(left_samples, "left", right_samples, "right", true);
  std::vector<float> left = float32ArrayToVector(left_samples, "left");
  std::vector<float> right = float32ArrayToVector(right_samples, "right");
  validate_offline_audio_input("left", left.data(), left.size(), sample_rate);
  validate_offline_audio_input("right", right.data(), right.size(), sample_rate);
  return mastering::api::analyze_named_stereo(analysis_name, left.data(), right.data(), left.size(),
                                              sample_rate, masteringParamsFromObject(params));
}

// The analysis entry points take an interleaved buffer so BS.1770 channel
// summing sees the program rather than a downmix; the JS surface keeps the
// planar left/right shape the rest of the stereo mastering API uses.
std::vector<float> interleaveValidatedPair(const char* left_name, const char* right_name,
                                           val left_samples, val right_samples, int sample_rate) {
  validateWasmFloat32ArrayPair(left_samples, "left", right_samples, "right", true);
  std::vector<float> left = float32ArrayToVector(left_samples, "left");
  std::vector<float> right = float32ArrayToVector(right_samples, "right");
  validate_offline_audio_input(left_name, left.data(), left.size(), sample_rate);
  validate_offline_audio_input(right_name, right.data(), right.size(), sample_rate);
  std::vector<float> interleaved(left.size() * 2);
  for (size_t index = 0; index < left.size(); ++index) {
    interleaved[2 * index] = left[index];
    interleaved[2 * index + 1] = right[index];
  }
  return interleaved;
}

// Builds an assistant config from a JS params object. `targetPlatform` and
// `preset` are NAMES on this surface: they are read here, validated against the
// shared tables, and kept out of the numeric conversion. The index the C ABI
// carries is a transport detail for callers that cannot pass a string, so a
// number is rejected here rather than silently accepted as an index.
mastering::assistant::AssistantConfig assistantConfigFromParams(val params_obj) {
  static const std::vector<std::string> kNameKeys = {"targetPlatform", "preset"};
  std::string platform;
  bool has_platform = false;
  std::string preset;
  bool has_preset = false;
  if (!params_obj.isNull() && !params_obj.isUndefined()) {
    if (hasProperty(params_obj, "targetPlatform")) {
      val value = params_obj["targetPlatform"];
      if (value.typeOf().as<std::string>() != "string") {
        throw WasmRangeError("'targetPlatform' must be one of the delivery-target names: " +
                             mastering::assistant::platform_names_joined());
      }
      platform = value.as<std::string>();
      wasmRefuseEmbeddedNul(platform, "targetPlatform");
      has_platform = true;
    }
    if (hasProperty(params_obj, "preset")) {
      val value = params_obj["preset"];
      if (value.typeOf().as<std::string>() != "string") {
        throw WasmRangeError("'preset' must be a mastering preset name");
      }
      preset = value.as<std::string>();
      wasmRefuseEmbeddedNul(preset, "preset");
      has_preset = true;
    }
  }
  const std::vector<mastering::api::Param> params =
      masteringParamsFromObject(params_obj, kNameKeys);
  mastering::assistant::AssistantConfig config =
      mastering::assistant::assistant_config_from_params(params.data(), params.size());
  if (has_platform) mastering::assistant::set_target_platform(config, platform);
  if (has_preset) config.preset = mastering::api::preset_from_string(preset);
  return config;
}

std::string js_mastering_assistant_suggest(val samples, const val& sample_rate_val,
                                           val params_obj) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  std::vector<float> data = float32ArrayToVector(samples, "samples");
  validate_offline_audio_input("samples", data.data(), data.size(), sample_rate);
  const mastering::assistant::AssistantConfig config = assistantConfigFromParams(params_obj);
  const auto result =
      mastering::assistant::suggest_chain(data.data(), data.size(), sample_rate, config);
  return mastering::assistant::assistant_result_to_json(result);
}

// The chain document text of preset `preset_name`'s built-in configuration, the same shape
// masteringAssistantSuggest carries as chainConfig for an analyzed chain -- passing it as the
// overrides document to masterAudio reproduces the preset unchanged. Calls the core directly
// rather than the C ABI's mirror sonare_mastering_preset_params_json.
//
// Restoration presets' repair stages, like every classical denoise
// configuration, can mistake a steady tone -- a calibration tone, a drone, a
// long held note -- for noise and pull it down by the configured reduction
// depth. Check for musical sustained tones before applying one.
std::string js_mastering_preset_params(std::string preset_name) {
  wasmRefuseEmbeddedNul(preset_name, "preset");
  const auto preset = mastering::api::preset_from_string(preset_name);
  return mastering::api::chain_config_to_json(mastering::api::preset_config(preset));
}

// A caller's chain configuration document in canonical form, validated by the parser the
// native CLI applies to --chain-config; a refused document is a coded InvalidFormat.
std::string js_mastering_chain_config_params(std::string document) {
  wasmRefuseEmbeddedNul(document, "document");
  return mastering::api::chain_config_to_json(mastering::api::chain_config_from_json(document));
}

std::string js_mastering_audio_profile(val samples, const val& sample_rate_val, val params_obj) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  std::vector<float> data = float32ArrayToVector(samples, "samples");
  validate_offline_audio_input("samples", data.data(), data.size(), sample_rate);
  std::vector<mastering::api::Param> params = masteringParamsFromObject(params_obj);
  const mastering::assistant::AudioProfileConfig config =
      mastering::assistant::audio_profile_config_from_params(params.data(), params.size());
  const auto profile =
      mastering::assistant::analyze_audio_profile(data.data(), data.size(), sample_rate, config);
  return mastering::assistant::audio_profile_to_json(profile);
}

std::string js_mastering_assistant_suggest_stereo(val left_samples, val right_samples,
                                                  const val& sample_rate_val, val params_obj) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const std::vector<float> interleaved =
      interleaveValidatedPair("left", "right", left_samples, right_samples, sample_rate);
  const mastering::assistant::AssistantConfig config = assistantConfigFromParams(params_obj);
  const auto result = mastering::assistant::suggest_chain_interleaved(
      interleaved.data(), interleaved.size() / 2, 2, sample_rate, config);
  return mastering::assistant::assistant_result_to_json(result);
}

std::string js_mastering_audio_profile_stereo(val left_samples, val right_samples,
                                              const val& sample_rate_val, val params_obj) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const std::vector<float> interleaved =
      interleaveValidatedPair("left", "right", left_samples, right_samples, sample_rate);
  std::vector<mastering::api::Param> params = masteringParamsFromObject(params_obj);
  const mastering::assistant::AudioProfileConfig config =
      mastering::assistant::audio_profile_config_from_params(params.data(), params.size());
  const auto profile = mastering::assistant::analyze_audio_profile_interleaved(
      interleaved.data(), interleaved.size() / 2, 2, sample_rate, config);
  return mastering::assistant::audio_profile_to_json(profile);
}

std::vector<mastering::maximizer::StreamingPlatform> streamingPlatformsFromVal(val platforms) {
  std::vector<mastering::maximizer::StreamingPlatform> out;
  if (platforms.isUndefined() || platforms.isNull()) {
    return out;
  }
  if (!val::global("Array").call<bool>("isArray", platforms)) {
    throw WasmTypeError("platforms must be an array");
  }
  const int length = static_cast<int>(wasmArrayLength(platforms, "platforms"));
  out.reserve(static_cast<size_t>(length));
  for (int index = 0; index < length; ++index) {
    // Every field is required: a missing one is refused by name, never filled with a default.
    const val platform = platforms[index];
    const std::string subject = "platforms[" + std::to_string(index) + "]";
    const std::string name =
        stringFromVal(requireTypedProperty(platform, "name", subject.c_str(), "string", "a string"),
                      (subject + ".name").c_str());
    const auto finite = [&](const char* key) {
      return checkedFloatFromVal(
          requireTypedProperty(platform, key, subject.c_str(), "number", "a number"),
          (subject + "." + key).c_str());
    };
    out.push_back({name, finite("targetLufs"), finite("ceilingDbtp")});
  }
  return out;
}

std::string js_mastering_streaming_preview(val samples, const val& sample_rate_val,
                                           val platforms_obj) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const Audio audio = loadValidatedAudio("samples", samples, sample_rate);
  const auto platforms = streamingPlatformsFromVal(platforms_obj);
  const auto results = platforms.empty()
                           ? mastering::maximizer::streaming_preview(audio)
                           : mastering::maximizer::streaming_preview(audio, platforms);
  return mastering::maximizer::streaming_preview_to_json(results);
}

std::string js_mastering_streaming_preview_stereo(val left_samples, val right_samples,
                                                  const val& sample_rate_val, val platforms_obj) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const std::vector<float> interleaved =
      interleaveValidatedPair("left", "right", left_samples, right_samples, sample_rate);
  const size_t frames = interleaved.size() / 2;
  const auto platforms = streamingPlatformsFromVal(platforms_obj);
  const auto results = platforms.empty()
                           ? mastering::maximizer::streaming_preview_interleaved(
                                 interleaved.data(), frames, 2, sample_rate)
                           : mastering::maximizer::streaming_preview_interleaved(
                                 interleaved.data(), frames, 2, sample_rate, platforms);
  return mastering::maximizer::streaming_preview_to_json(results);
}

void registerMasteringApiBindings() {
  function("masteringProcessorNames", &js_mastering_processor_names);
  function("masteringAmpPresetCatalog", &js_mastering_amp_preset_catalog);
  function("masteringInsertNames", &js_mastering_insert_names);
  function("masteringInsertParamNames", &js_mastering_insert_param_names);
  function("masteringInsertParamInfo", &js_mastering_insert_param_info);
  function("masteringInsertTiming", &js_mastering_insert_timing);
  function("masteringEnumValue", &js_mastering_enum_value);
  function("masteringProcessorCatalog", &js_mastering_processor_catalog);
  function("capabilityCatalog", &js_capability_catalog);
  function("masteringPairProcessorNames", &js_mastering_pair_processor_names);
  function("masteringPairAnalysisNames", &js_mastering_pair_analysis_names);
  function("masteringStereoAnalysisNames", &js_mastering_stereo_analysis_names);
  function("masteringProcess", &js_mastering_process);
  function("masteringProcessStereo", &js_mastering_process_stereo);
  function("masteringPairProcess", &js_mastering_pair_process);
  function("masteringPairProcessStereo", &js_mastering_pair_process_stereo);
  function("masteringPairAnalyze", &js_mastering_pair_analyze);
  function("masteringPairAnalyzeStereo", &js_mastering_pair_analyze_stereo);
  function("masteringAbMatchLoudness", &js_mastering_ab_match_loudness);
  function("masteringAbMatchLoudnessStereo", &js_mastering_ab_match_loudness_stereo);
  function("masteringStereoAnalyze", &js_mastering_stereo_analyze);
  function("masteringAssistantSuggest", &js_mastering_assistant_suggest);
  function("masteringAudioProfile", &js_mastering_audio_profile);
  function("masteringStreamingPreview", &js_mastering_streaming_preview);
  function("masteringAssistantSuggestStereo", &js_mastering_assistant_suggest_stereo);
  function("masteringAudioProfileStereo", &js_mastering_audio_profile_stereo);
  function("masteringStreamingPreviewStereo", &js_mastering_streaming_preview_stereo);
  function("masteringPresetNames", &js_mastering_preset_names);
  function("masteringPresetParams", &js_mastering_preset_params);
  function("masteringChainConfigParams", &js_mastering_chain_config_params);
  function("masteringPlatformNames", &js_mastering_platform_names);
  function("masterAudio", &js_master_audio);
  function("masterAudioStereo", &js_master_audio_stereo);
}

#endif  // __EMSCRIPTEN__
