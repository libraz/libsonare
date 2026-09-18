/// @file mastering_api.cpp
/// @brief Embind bindings for named mastering processors, presets, assistant, and streaming preview
/// APIs.

#ifdef __EMSCRIPTEN__

#include "mastering/api/chain.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "mastering/assistant/config_from_params.h"
#include "mastering/assistant/platform_targets.h"
#include "mastering/match/ab_switcher.h"
#include "midi/synth/synth_presets.h"
#include "mixing/api/presets.h"
#include "sonare.h"
#include "util/json.h"
#include "wasm/bindings/common/common.h"
#include "wasm/bindings/mastering/chain_result.h"

val js_mastering_processor_names() { return stringVectorToVal(mastering::api::processor_names()); }

// Names of the insert processors the mastering chain can instantiate by name
// (mastering::api::insert_factory_names). Mirrors the C ABI
// sonare_mastering_insert_names (which joins this list) as a string[].
val js_mastering_insert_names() {
  return stringVectorToVal(mastering::api::insert_factory_names());
}

// Parameter names a given insert processor reads (mastering::api::insert_param_names).
// Any key not in this list is silently ignored; for scene loads those ignored
// keys are reported via Mixer.sceneWarnings(). Empty array for an unknown name.
val js_mastering_insert_param_names(std::string name) {
  val out = val::array();
  auto names = mastering::api::insert_param_names(name);
  for (size_t index = 0; index < names.size(); ++index) {
    out.call<void>("push", names[index]);
  }
  return out;
}

// Realtime-automatable parameter descriptors for an insert processor, as a JSON
// array string [{"name","id","rtSafe"}, ...]. The TS facade parses it; "[]" for
// an unknown name or a processor with no automatable parameters.
std::string js_mastering_insert_param_info(std::string name) {
  return mastering::api::insert_param_info_json(name);
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

// The WASM target deliberately does not link the aggregate C-ABI TU. Build the
// identical schema from the same core registries that back that ABI instead.
std::string js_capability_catalog() {
  namespace json = sonare::util::json;

  json::Object catalog;
  catalog["version"] = SONARE_VERSION_STRING;
  json::Object abi;
  abi["project"] = static_cast<int>(SONARE_PROJECT_ABI_VERSION);
  abi["engine"] = static_cast<int>(sonare::rt::kEngineAbiVersion);
  catalog["abi"] = std::move(abi);
  catalog["processors"] = json::parse_strict(mastering::api::processor_catalog_json());

  json::Object presets;
  presets["mastering"] = catalog_name_array(mastering::api::preset_names());
  presets["synth"] = catalog_synth_preset_names();
#if defined(SONARE_WITH_MIXING)
  presets["mixingScene"] = catalog_name_array(mixing::api::scene_preset_names());
#else
  presets["mixingScene"] = json::Array{};
#endif
  presets["voiceChanger"] = catalog_voice_changer_preset_names();
  catalog["presets"] = std::move(presets);
  return json::dump(json::Value(std::move(catalog)));
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
  val out = val::array();
  for (const std::string& name : mastering::assistant::platform_names()) {
    out.call<void>("push", name);
  }
  return out;
}

val js_master_audio(std::string preset_name, val samples, const val& sample_rate_val,
                    val overrides) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  std::vector<float> data = float32ArrayToVector(samples);
  auto preset = mastering::api::preset_from_string(preset_name);
  auto overrides_vec = masteringParamsFromObject(overrides);
  auto result = mastering::api::master_audio_mono(
      preset, data.data(), data.size(), sample_rate,
      overrides_vec.empty() ? nullptr : overrides_vec.data(), overrides_vec.size());

  return masteringMonoResultToVal(result);
}

val js_master_audio_stereo(std::string preset_name, val left_samples, val right_samples,
                           const val& sample_rate_val, val overrides) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateWasmFloat32ArrayPair(left_samples, "left samples", right_samples, "right samples",
                               "masterAudioStereo input", true);
  std::vector<float> left = float32ArrayToVector(left_samples);
  std::vector<float> right = float32ArrayToVector(right_samples);
  auto preset = mastering::api::preset_from_string(preset_name);
  auto overrides_vec = masteringParamsFromObject(overrides);
  auto result = mastering::api::master_audio_stereo(
      preset, left.data(), right.data(), left.size(), sample_rate,
      overrides_vec.empty() ? nullptr : overrides_vec.data(), overrides_vec.size());

  return masteringStereoResultToVal(result);
}

val js_master_audio_with_progress(std::string preset_name, val samples, const val& sample_rate_val,
                                  val overrides, val progress_callback, val cancel_callback) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  std::vector<float> data = float32ArrayToVector(samples);
  auto preset = mastering::api::preset_from_string(preset_name);
  auto config = mastering::api::preset_config(preset);
  auto overrides_vec = masteringParamsFromObject(overrides);
  if (!overrides_vec.empty()) {
    mastering::api::apply_chain_config_overrides(config, overrides_vec.data(),
                                                 overrides_vec.size());
  }
  mastering::api::MasteringChain chain(std::move(config));
  installMasteringChainCallbacks(chain, progress_callback, cancel_callback);
  const auto result = chain.process_mono_cancellable(data.data(), data.size(), sample_rate);
  if (!result) {
    throw SonareException(ErrorCode::Cancelled, "mastering cancelled");
  }

  return masteringMonoResultToVal(*result);
}

val js_master_audio_stereo_with_progress(std::string preset_name, val left_samples,
                                         val right_samples, const val& sample_rate_val,
                                         val overrides, val progress_callback,
                                         val cancel_callback) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateWasmFloat32ArrayPair(left_samples, "left samples", right_samples, "right samples",
                               "masterAudioStereoWithProgress input", true);
  std::vector<float> left = float32ArrayToVector(left_samples);
  std::vector<float> right = float32ArrayToVector(right_samples);
  auto preset = mastering::api::preset_from_string(preset_name);
  auto config = mastering::api::preset_config(preset);
  auto overrides_vec = masteringParamsFromObject(overrides);
  if (!overrides_vec.empty()) {
    mastering::api::apply_chain_config_overrides(config, overrides_vec.data(),
                                                 overrides_vec.size());
  }
  mastering::api::MasteringChain chain(std::move(config));
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
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  auto result = mastering::api::apply_named_processor(
      processor_name, data.data(), data.size(), sample_rate, masteringParamsFromObject(params));
  val out = val::object();
  out.set("samples", vectorToFloat32Array(result.samples));
  out.set("sampleRate", result.sample_rate);
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  out.set("latencySamples", result.latency_samples);
  out.set("loudnessTargetLimited", result.loudness_target_limited);
  out.set("nonFiniteSubstitutionCount", static_cast<double>(result.non_finite_substitution_count));
  return out;
}

val js_mastering_process_stereo(std::string processor_name, val left_samples, val right_samples,
                                const val& sample_rate_val, val params) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateWasmFloat32ArrayPair(left_samples, "left samples", right_samples, "right samples",
                               "masteringProcessStereo input", true);
  std::vector<float> left = float32ArrayToVector(left_samples);
  std::vector<float> right = float32ArrayToVector(right_samples);
  validate_offline_audio_input(left.data(), left.size(), sample_rate);
  validate_offline_audio_input(right.data(), right.size(), sample_rate);
  auto result = mastering::api::apply_named_processor_stereo(processor_name, left.data(),
                                                             right.data(), left.size(), sample_rate,
                                                             masteringParamsFromObject(params));
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
  return out;
}

val js_mastering_pair_process(std::string processor_name, val source_samples, val reference_samples,
                              const val& sample_rate_val, val params) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateWasmFloat32ArrayPair(source_samples, "source samples", reference_samples,
                               "reference samples", "masteringPairProcess input", false);
  std::vector<float> source = float32ArrayToVector(source_samples);
  std::vector<float> reference = float32ArrayToVector(reference_samples);
  // source and reference may have independent lengths.
  validate_offline_audio_input(source.data(), source.size(), sample_rate);
  validate_offline_audio_input(reference.data(), reference.size(), sample_rate);
  auto result = mastering::api::apply_named_pair_processor(
      processor_name, source.data(), reference.data(), source.size(), reference.size(), sample_rate,
      masteringParamsFromObject(params));
  val out = val::object();
  out.set("samples", vectorToFloat32Array(result.samples));
  out.set("sampleRate", result.sample_rate);
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  out.set("latencySamples", result.latency_samples);
  out.set("nonFiniteSubstitutionCount", static_cast<double>(result.non_finite_substitution_count));
  return out;
}

std::string js_mastering_pair_analyze(std::string analysis_name, val source_samples,
                                      val reference_samples, const val& sample_rate_val,
                                      val params) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateWasmFloat32ArrayPair(source_samples, "source samples", reference_samples,
                               "reference samples", "masteringPairAnalyze input", false);
  std::vector<float> source = float32ArrayToVector(source_samples);
  std::vector<float> reference = float32ArrayToVector(reference_samples);
  // source and reference may have independent lengths.
  validate_offline_audio_input(source.data(), source.size(), sample_rate);
  validate_offline_audio_input(reference.data(), reference.size(), sample_rate);
  return mastering::api::analyze_named_pair(analysis_name, source.data(), reference.data(),
                                            source.size(), reference.size(), sample_rate,
                                            masteringParamsFromObject(params));
}

// Gain-matches `source` to `reference`'s integrated loudness. The core takes the
// reference first; the C ABI and every facade take (source, reference), so the
// two arguments are swapped here rather than at the call site.
val js_mastering_ab_match_loudness(val source_samples, val reference_samples,
                                   const val& sample_rate_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateWasmFloat32ArrayPair(source_samples, "source samples", reference_samples,
                               "reference samples", "masteringAbMatchLoudness input", false);
  // source and reference may have independent lengths.
  const Audio source = loadValidatedAudio(source_samples, sample_rate);
  const Audio reference = loadValidatedAudio(reference_samples, sample_rate);
  const auto matched = mastering::match::ab_match_loudness(reference, source);

  val out = val::object();
  out.set("samples", vectorToFloat32Array(std::vector<float>(matched.b.data(),
                                                             matched.b.data() + matched.b.size())));
  out.set("sampleRate", matched.b.sample_rate());
  out.set("referenceLufs", matched.reference_lufs);
  out.set("sourceLufs", matched.source_lufs);
  out.set("appliedGainDb", matched.applied_gain_db);
  out.set("matchedTruePeakDbtp", matched.matched_true_peak_dbtp);
  return out;
}

std::string js_mastering_stereo_analyze(std::string analysis_name, val left_samples,
                                        val right_samples, const val& sample_rate_val, val params) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateWasmFloat32ArrayPair(left_samples, "left samples", right_samples, "right samples",
                               "masteringStereoAnalyze input", true);
  std::vector<float> left = float32ArrayToVector(left_samples);
  std::vector<float> right = float32ArrayToVector(right_samples);
  validate_offline_audio_input(left.data(), left.size(), sample_rate);
  validate_offline_audio_input(right.data(), right.size(), sample_rate);
  return mastering::api::analyze_named_stereo(analysis_name, left.data(), right.data(), left.size(),
                                              sample_rate, masteringParamsFromObject(params));
}

// The analysis entry points take an interleaved buffer so BS.1770 channel
// summing sees the program rather than a downmix; the JS surface keeps the
// planar left/right shape the rest of the stereo mastering API uses.
std::vector<float> interleaveValidatedPair(val left_samples, val right_samples, int sample_rate,
                                           const char* context) {
  validateWasmFloat32ArrayPair(left_samples, "left samples", right_samples, "right samples",
                               context, true);
  std::vector<float> left = float32ArrayToVector(left_samples);
  std::vector<float> right = float32ArrayToVector(right_samples);
  validate_offline_audio_input(left.data(), left.size(), sample_rate);
  validate_offline_audio_input(right.data(), right.size(), sample_rate);
  std::vector<float> interleaved(left.size() * 2);
  for (size_t index = 0; index < left.size(); ++index) {
    interleaved[2 * index] = left[index];
    interleaved[2 * index + 1] = right[index];
  }
  return interleaved;
}

// Builds an assistant config from a JS params object. `targetPlatform` is a
// delivery-target NAME on this surface: it is read here, validated against the
// shared table, and kept out of the numeric conversion. The index the C ABI
// carries is a transport detail for callers that cannot pass a string, so a
// number is rejected here rather than silently accepted as an index.
mastering::assistant::AssistantConfig assistantConfigFromParams(val params_obj) {
  static const std::vector<std::string> kPlatformKeys = {"targetPlatform", "target_platform"};
  std::string platform;
  bool has_platform = false;
  if (!params_obj.isNull() && !params_obj.isUndefined()) {
    for (const std::string& key : kPlatformKeys) {
      if (!hasProperty(params_obj, key.c_str())) continue;
      val value = params_obj[key];
      if (value.typeOf().as<std::string>() != "string") {
        throw SonareException(ErrorCode::InvalidParameter,
                              "'" + key + "' must be one of the delivery-target names: " +
                                  mastering::assistant::platform_names_joined());
      }
      platform = value.as<std::string>();
      has_platform = true;
    }
  }
  const std::vector<mastering::api::Param> params =
      masteringParamsFromObject(params_obj, kPlatformKeys);
  mastering::assistant::AssistantConfig config =
      mastering::assistant::assistant_config_from_params(params.data(), params.size());
  if (has_platform) mastering::assistant::set_target_platform(config, platform);
  return config;
}

std::string js_mastering_assistant_suggest(val samples, const val& sample_rate_val,
                                           val params_obj) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  const mastering::assistant::AssistantConfig config = assistantConfigFromParams(params_obj);
  const auto result =
      mastering::assistant::suggest_chain(data.data(), data.size(), sample_rate, config);
  return mastering::assistant::assistant_result_to_json(result);
}

// Flattens a suggested chain config to the {key: number|boolean} map the JS
// facade hands straight to `overrides`, unwrapping the {"version","params"}
// envelope chain_config_to_json writes. Throws by key name rather than
// dropping silently if a param is a schema v2 nested object (e.g. a structured
// multibandComp), since a silently dropped override reads as "applied" when it
// was not. An empty params block is refused for the same reason: an empty
// overrides map applies the preset unchanged.
val chainConfigParamsToVal(const mastering::api::MasteringChainConfig& config) {
  namespace json = sonare::util::json;
  const json::Value parsed = json::parse_strict(mastering::api::chain_config_to_json(config));
  const json::Object& params = parsed["params"].as_object();
  if (params.empty()) {
    throw SonareException(ErrorCode::InvalidParameter, "chain config carries no params block");
  }
  val out = val::object();
  for (const auto& [key, value] : params) {
    if (value.is_number()) {
      out.set(key, value.as_number());
    } else if (value.is_bool()) {
      out.set(key, value.as_bool());
    } else {
      throw SonareException(
          ErrorCode::InvalidParameter,
          "chain config param '" + key + "' is not a number or boolean (schema v2 nested value)");
    }
  }
  return out;
}

val js_mastering_assistant_suggest_chain(val samples, const val& sample_rate_val, val params_obj) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  const mastering::assistant::AssistantConfig config = assistantConfigFromParams(params_obj);
  const auto result =
      mastering::assistant::suggest_chain(data.data(), data.size(), sample_rate, config);
  return chainConfigParamsToVal(result.config);
}

std::string js_mastering_audio_profile(val samples, const val& sample_rate_val, val params_obj) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
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
  const std::vector<float> interleaved = interleaveValidatedPair(
      left_samples, right_samples, sample_rate, "masteringAssistantSuggestStereo input");
  const mastering::assistant::AssistantConfig config = assistantConfigFromParams(params_obj);
  const auto result = mastering::assistant::suggest_chain_interleaved(
      interleaved.data(), interleaved.size() / 2, 2, sample_rate, config);
  return mastering::assistant::assistant_result_to_json(result);
}

val js_mastering_assistant_suggest_chain_stereo(val left_samples, val right_samples,
                                                const val& sample_rate_val, val params_obj) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const std::vector<float> interleaved = interleaveValidatedPair(
      left_samples, right_samples, sample_rate, "masteringAssistantSuggestChainStereo input");
  const mastering::assistant::AssistantConfig config = assistantConfigFromParams(params_obj);
  const auto result = mastering::assistant::suggest_chain_interleaved(
      interleaved.data(), interleaved.size() / 2, 2, sample_rate, config);
  return chainConfigParamsToVal(result.config);
}

std::string js_mastering_audio_profile_stereo(val left_samples, val right_samples,
                                              const val& sample_rate_val, val params_obj) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const std::vector<float> interleaved = interleaveValidatedPair(
      left_samples, right_samples, sample_rate, "masteringAudioProfileStereo input");
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
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "platforms must be an array");
  }
  const int length = static_cast<int>(wasmArrayLikeLength(platforms, "platforms"));
  out.reserve(static_cast<size_t>(length));
  for (int index = 0; index < length; ++index) {
    val platform = platforms[index];
    out.push_back({stringProperty(platform, "name", ""),
                   floatProperty(platform, "targetLufs", -14.0f),
                   floatProperty(platform, "ceilingDb", -1.0f)});
  }
  return out;
}

std::string js_mastering_streaming_preview(val samples, const val& sample_rate_val,
                                           val platforms_obj) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const Audio audio = loadValidatedAudio(samples, sample_rate);
  const auto platforms = streamingPlatformsFromVal(platforms_obj);
  const auto results = platforms.empty()
                           ? mastering::maximizer::streaming_preview(audio)
                           : mastering::maximizer::streaming_preview(audio, platforms);
  return mastering::maximizer::streaming_preview_to_json(results);
}

std::string js_mastering_streaming_preview_stereo(val left_samples, val right_samples,
                                                  const val& sample_rate_val, val platforms_obj) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const std::vector<float> interleaved = interleaveValidatedPair(
      left_samples, right_samples, sample_rate, "masteringStreamingPreviewStereo input");
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
  function("masteringInsertNames", &js_mastering_insert_names);
  function("masteringInsertParamNames", &js_mastering_insert_param_names);
  function("masteringInsertParamInfo", &js_mastering_insert_param_info);
  function("masteringProcessorCatalog", &js_mastering_processor_catalog);
  function("capabilityCatalog", &js_capability_catalog);
  function("masteringPairProcessorNames", &js_mastering_pair_processor_names);
  function("masteringPairAnalysisNames", &js_mastering_pair_analysis_names);
  function("masteringStereoAnalysisNames", &js_mastering_stereo_analysis_names);
  function("masteringProcess", &js_mastering_process);
  function("masteringProcessStereo", &js_mastering_process_stereo);
  function("masteringPairProcess", &js_mastering_pair_process);
  function("masteringPairAnalyze", &js_mastering_pair_analyze);
  function("masteringAbMatchLoudness", &js_mastering_ab_match_loudness);
  function("masteringStereoAnalyze", &js_mastering_stereo_analyze);
  function("masteringAssistantSuggest", &js_mastering_assistant_suggest);
  function("masteringAssistantSuggestChain", &js_mastering_assistant_suggest_chain);
  function("masteringAudioProfile", &js_mastering_audio_profile);
  function("masteringStreamingPreview", &js_mastering_streaming_preview);
  function("masteringAssistantSuggestStereo", &js_mastering_assistant_suggest_stereo);
  function("masteringAssistantSuggestChainStereo", &js_mastering_assistant_suggest_chain_stereo);
  function("masteringAudioProfileStereo", &js_mastering_audio_profile_stereo);
  function("masteringStreamingPreviewStereo", &js_mastering_streaming_preview_stereo);
  function("masteringPresetNames", &js_mastering_preset_names);
  function("masteringPlatformNames", &js_mastering_platform_names);
  function("masterAudio", &js_master_audio);
  function("masterAudioStereo", &js_master_audio_stereo);
  function("masterAudioWithProgress", &js_master_audio_with_progress);
  function("masterAudioStereoWithProgress", &js_master_audio_stereo_with_progress);
}

#endif  // __EMSCRIPTEN__
