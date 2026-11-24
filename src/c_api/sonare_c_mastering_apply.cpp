#include <sonare/sonare_c.h>

#include <array>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "core/audio.h"
#include "core/audio_encode.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "mastering/common/loudness_measure.h"
#include "mastering/match/ab_switcher.h"
#include "mastering/maximizer/loudness_optimize.h"
#if defined(SONARE_WITH_PLAYBACK)
#include "playback/config.h"
#endif
#include "sonare_c_internal.h"
#include "sonare_c_mastering_helpers.h"
#include "util/json.h"
#include "util/parameter_checks.h"

using namespace sonare;
using namespace sonare_c_detail;
using namespace sonare_c_mastering_detail;

namespace {

const std::vector<std::string>& insert_names() {
  static const auto names = mastering::api::insert_factory_names();
  return names;
}

sonare::util::json::Array newline_name_array(const char* names) {
  sonare::util::json::Array values;
  // These lists fail only on allocation, and a partial catalog must never be cached.
  if (names == nullptr) throw std::bad_alloc();

  for (const char* line = names; *line != '\0';) {
    const char* end = line;
    while (*end != '\0' && *end != '\n') ++end;
    if (end != line) values.emplace_back(std::string(line, static_cast<size_t>(end - line)));
    line = (*end == '\0') ? end : end + 1;
  }
  return values;
}

void set_stereo_result(const sonare::mastering::api::StereoResult& result,
                       SonareMasteringStereoResult* out) {
  const std::size_t length = result.left.size();
  std::unique_ptr<float[]> left;
  std::unique_ptr<float[]> right;
  if (length != 0) {
    // Allocate before publishing any field so a bad_alloc leaves the result cleared.
    left = std::make_unique<float[]>(length);
    right = std::make_unique<float[]>(length);
    std::memcpy(left.get(), result.left.data(), length * sizeof(float));
    std::memcpy(right.get(), result.right.data(), length * sizeof(float));
  }

  out->length = length;
  out->sample_rate = result.sample_rate;
  out->input_lufs = result.input_lufs;
  out->output_lufs = result.output_lufs;
  out->applied_gain_db = result.applied_gain_db;
  out->latency_samples = result.latency_samples;
  out->loudness_target_limited = result.loudness_target_limited ? 1 : 0;
  out->non_finite_substitution_count = result.non_finite_substitution_count;
  set_stage_counts(result, out);
  out->left = release_array(left);
  out->right = release_array(right);
}

// The core's loudness config member names, as SonareMasteringConfig spells them.
const std::array<CName, 4> kMasteringConfigNames{
    {{"target_lufs", "config.target_lufs"},
     {"ceiling_dbtp", "config.ceiling_dbtp"},
     {"release_ms", "config.release_ms"},
     {"true_peak_oversample", "config.true_peak_oversample"}}};

}  // namespace

void sonare_mastering_config_init(SonareMasteringConfig* config) {
  if (config == nullptr) return;
  const sonare::mastering::maximizer::LoudnessOptimizeConfig cpp;
  *config = {};
  config->struct_size = sizeof(*config);
  config->target_lufs = cpp.target_lufs;
  config->ceiling_dbtp = cpp.ceiling_dbtp;
  config->true_peak_oversample = cpp.true_peak_oversample;
  config->release_ms = cpp.release_ms;
  config->apply_gain_at_input_rate = cpp.apply_gain_at_input_rate ? 1 : 0;
}

SonareError sonare_mastering_process(const float* samples, size_t length, int sample_rate,
                                     const SonareMasteringConfig* config,
                                     SonareMasteringResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  if (out != nullptr) out->sample_rate = sample_rate;

  if (const SonareError err = validate_audio_params("samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);

  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        const auto cpp_config = to_cpp_config(config);
        auto result = with_c_names(kMasteringConfigNames, [&] {
          return sonare::mastering::maximizer::loudness_optimize(audio, cpp_config);
        });

        out->length = result.audio.size();
        out->sample_rate = result.audio.sample_rate();
        out->input_lufs = result.input_lufs;
        out->output_lufs = result.output_lufs;
        out->applied_gain_db = result.applied_gain_db;
        out->latency_samples = result.latency_samples;
        out->loudness_target_limited = result.loudness_target_limited ? 1 : 0;
        out->non_finite_substitution_count = result.non_finite_substitution_count;

        return copy_audio_result(result.audio, &out->samples, &out->length);
      });
}

SonareError sonare_mastering_apply_processor(const char* processor, const float* samples,
                                             size_t length, int sample_rate,
                                             const SonareMasteringParam* params, size_t param_count,
                                             SonareMasteringResult* out) {
  SONARE_C_API_ENTRY;
  // Define the result before any validation return, so a rejected call hands
  // back a defined struct rather than the caller's untouched stack slot.
  SONARE_C_ZERO_OUTS(out);
  if (out != nullptr) out->sample_rate = sample_rate;

  SONARE_C_REQUIRE_NONNULL(processor);
  if (const SonareError empty_err =
          run_validation([&] { util::check::non_empty(processor, "processor"); });
      empty_err != SONARE_OK) {
    return empty_err;
  }
  SonareError err = validate_audio_params("samples", samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(params, param_count);
  SONARE_C_REQUIRE_NONNULL(out);

  SONARE_C_TRY
  auto result = sonare::util::check::under_member("params", [&] {
    return sonare::mastering::api::apply_named_processor(processor, samples, length, sample_rate,
                                                         to_params(params, param_count));
  });
  set_mastering_result(result, out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_apply_processor_stereo(
    const char* processor, const float* left, const float* right, size_t length, int sample_rate,
    const SonareMasteringParam* params, size_t param_count, SonareMasteringStereoResult* out) {
  SONARE_C_API_ENTRY;
  // Define the result before any validation return, so a rejected call hands
  // back a defined struct rather than the caller's untouched stack slot.
  SONARE_C_ZERO_OUTS(out);
  if (out != nullptr) out->sample_rate = sample_rate;

  SONARE_C_REQUIRE_NONNULL(processor);
  if (const SonareError empty_err =
          run_validation([&] { util::check::non_empty(processor, "processor"); });
      empty_err != SONARE_OK) {
    return empty_err;
  }
  SonareError err = validate_stereo_audio_params("left", "right", left, right, length, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(params, param_count);
  SONARE_C_REQUIRE_NONNULL(out);

  SONARE_C_TRY
  auto result = sonare::util::check::under_member("params", [&] {
    return sonare::mastering::api::apply_named_processor_stereo(
        processor, left, right, length, sample_rate, to_params(params, param_count));
  });
  out->sample_rate = result.sample_rate;
  out->input_lufs = result.input_lufs;
  out->output_lufs = result.output_lufs;
  out->applied_gain_db = result.applied_gain_db;
  out->latency_samples = result.latency_samples;
  out->loudness_target_limited = result.loudness_target_limited ? 1 : 0;
  out->non_finite_substitution_count = result.non_finite_substitution_count;
  set_stage_counts(result, out);
  copy_stereo_channels(result.left.data(), result.right.data(), result.left.size(), &out->left,
                       &out->right, &out->length);
  return SONARE_OK;
  SONARE_C_CATCH
}

const char* sonare_mastering_processor_names(void) {
  SONARE_C_TRY
  static thread_local const std::string names =
      join_names(sonare::mastering::api::processor_names());
  return names.c_str();
  SONARE_C_CATCH_RETURN(nullptr)
}

const char* sonare_mastering_pair_processor_names(void) {
  SONARE_C_TRY
  static thread_local const std::string names =
      join_names(sonare::mastering::api::pair_processor_names());
  return names.c_str();
  SONARE_C_CATCH_RETURN(nullptr)
}

const char* sonare_mastering_pair_analysis_names(void) {
  SONARE_C_TRY
  static thread_local const std::string names =
      join_names(sonare::mastering::api::pair_analysis_names());
  return names.c_str();
  SONARE_C_CATCH_RETURN(nullptr)
}

const char* sonare_mastering_stereo_analysis_names(void) {
  SONARE_C_TRY
  static thread_local const std::string names =
      join_names(sonare::mastering::api::stereo_analysis_names());
  return names.c_str();
  SONARE_C_CATCH_RETURN(nullptr)
}

const char* sonare_mastering_insert_names(void) {
  SONARE_C_TRY
  static thread_local const std::string names = join_names(insert_names());
  return names.c_str();
  SONARE_C_CATCH_RETURN(nullptr)
}

SonareError sonare_mastering_processor_catalog(char** out_json) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_json);
  SONARE_C_REQUIRE_NONNULL(out_json);
  SONARE_C_TRY
  *out_json = copy_string(sonare::mastering::api::processor_catalog_json());
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_amp_preset_catalog(char** out_json) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_json);
  SONARE_C_REQUIRE_NONNULL(out_json);
  SONARE_C_TRY
  *out_json = copy_string(sonare::mastering::api::amp_preset_catalog_json());
  return SONARE_OK;
  SONARE_C_CATCH
}

namespace {

std::string build_capability_catalog_json() {
  namespace json = sonare::util::json;

  json::Object catalog;
  catalog["version"] = sonare_version();
  json::Object abi;
  abi["project"] = static_cast<int>(SONARE_PROJECT_ABI_VERSION);
  abi["engine"] = static_cast<int>(sonare_engine_abi_version());
  catalog["abi"] = std::move(abi);
  json::Array encode_builtin;
  encode_builtin.emplace_back("wav");
  json::Array encode_ffmpeg;
  for (const std::string_view format : sonare::kOptionalAudioEncodeFormats) {
    if (sonare::audio_encode_format_available(format)) {
      encode_ffmpeg.emplace_back(std::string(format));
    }
  }
  json::Object encode;
  encode["builtin"] = std::move(encode_builtin);
  encode["ffmpeg"] = std::move(encode_ffmpeg);
  catalog["encode"] = std::move(encode);
  catalog["processors"] = json::parse_strict(sonare::mastering::api::processor_catalog_json());
  catalog["chain"] = json::parse_strict(sonare::mastering::api::chain_catalog_json());

  json::Object presets;
  presets["mastering"] = newline_name_array(sonare_mastering_preset_names());
#if defined(SONARE_WITH_ARRANGEMENT)
  presets["synth"] = newline_name_array(sonare_synth_preset_names());
#else
  presets["synth"] = json::Array{};
#endif
#if defined(SONARE_WITH_MIXING)
  presets["mixingScene"] = newline_name_array(sonare_mixing_scene_preset_names());
#else
  presets["mixingScene"] = json::Array{};
#endif
#if defined(SONARE_WITH_VOICE_CHANGER)
  presets["voiceChanger"] = newline_name_array(sonare_realtime_voice_changer_preset_names());
#else
  presets["voiceChanger"] = json::Array{};
#endif
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

  json::Array mastering_presets;
  for (const auto& name : sonare::mastering::api::preset_names()) {
    const auto preset = sonare::mastering::api::preset_from_string(name);
    const auto config = sonare::mastering::api::preset_config(preset);
    const bool is_mastering = sonare::mastering::api::preset_kind(preset) ==
                              sonare::mastering::api::PresetKind::Mastering;

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

SonareError sonare_capability_catalog_json(char** out_json) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_json);
  SONARE_C_REQUIRE_NONNULL(out_json);
  SONARE_C_TRY
  // A pure function of the build, so one serialization serves every call.
  static const std::string serialized = build_capability_catalog_json();
  *out_json = copy_string(serialized);
  return SONARE_OK;
  SONARE_C_CATCH
}

namespace sonare_c_mastering_detail {

const std::vector<std::string>& capability_catalog_schema_paths() {
  static const std::vector<std::string> paths = {
      "version",
      "abi",
      "abi.project",
      "abi.engine",
      "encode",
      "encode.builtin",
      "encode.ffmpeg",
      "presets",
      "presets.mastering",
      "presets.synth",
      "presets.mixingScene",
      "presets.voiceChanger",
      "presets.playbackRoom",
      "masteringPresets",
      "masteringPresets[].name",
      "masteringPresets[].kind",
      "masteringPresets[].targetLufs",
      "masteringPresets[].truePeakCeilingDbtp",
      "masteringPresets[].maxLimiterGainReductionDb",
      "processors",
      "processors[].id",
      "processors[].description",
      "processors[].kind",
      "processors[].realtimeInsertable",
      "processors[].forms",
      "processors[].stereoOnly",
      "processors[].latencySamples",
      "processors[].tailSamples",
      "processors[].realtimeCost",
      "processors[].channelPolicy",
      "processors[].category",
      "processors[].causal",
      "processors[].params",
      "processors[].params[].name",
      "processors[].params[].id",
      "processors[].params[].rtSafe",
      "processors[].params[].type",
      "processors[].params[].min",
      "processors[].params[].max",
      "processors[].params[].minExclusive",
      "processors[].params[].maxExclusive",
      "processors[].params[].maxRelativeTo",
      "processors[].params[].domainMin",
      "processors[].params[].domainMinExclusive",
      "processors[].params[].domainMax",
      "processors[].params[].domainMaxExclusive",
      "processors[].params[].default",
      "processors[].params[].unit",
      "processors[].params[].uiMin",
      "processors[].params[].uiMax",
      "processors[].params[].scale",
      "processors[].params[].choices",
      "processors[].params[].choices[].name",
      "processors[].params[].choices[].value",
      "processors[].params[].slot",
      "processors[].params[].dependsOn",
      "processors[].params[].dependsOn[].key",
      "processors[].params[].dependsOn[].relation",
      "processors[].params[].dependsOn[].factor",
      "processors[].params[].coupledWith",
      "processors[].offlineParams",
      "processors[].offlineParams[].name",
      "processors[].offlineParams[].id",
      "processors[].offlineParams[].rtSafe",
      "processors[].offlineParams[].type",
      "processors[].offlineParams[].min",
      "processors[].offlineParams[].max",
      "processors[].offlineParams[].minExclusive",
      "processors[].offlineParams[].maxExclusive",
      "processors[].offlineParams[].maxRelativeTo",
      "processors[].offlineParams[].default",
      "processors[].offlineParams[].unit",
      "processors[].offlineParams[].uiMin",
      "processors[].offlineParams[].uiMax",
      "processors[].offlineParams[].scale",
      "processors[].offlineParams[].choices",
      "processors[].offlineParams[].choices[].name",
      "processors[].offlineParams[].choices[].value",
      "processors[].offlineParams[].slot",
      "processors[].offlineParams[].dependsOn",
      "processors[].offlineParams[].dependsOn[].key",
      "processors[].offlineParams[].dependsOn[].relation",
      "processors[].offlineParams[].dependsOn[].factor",
      "processors[].offlineParams[].coupledWith",
      "processors[].slots",
      "processors[].slots[].name",
      "processors[].slots[].parent",
      "processors[].slots[].activation",
      "processors[].slots[].minCrossoverCutoffs",
      "chain",
      "chain.stages",
      "chain.stages[].prefix",
      "chain.stages[].processor",
      "chain.stages[].form",
      "chain.stages[].enabledKey",
      "chain.lists",
      "chain.lists[].key",
      "chain.lists[].processor",
      "chain.lists[].form",
      "chain.lists[].minItems",
      "chain.lists[].maxItems",
      "chain.params",
      "chain.params[].name",
      "chain.params[].id",
      "chain.params[].rtSafe",
      "chain.params[].type",
      "chain.params[].min",
      "chain.params[].max",
      "chain.params[].minExclusive",
      "chain.params[].maxExclusive",
      "chain.params[].maxRelativeTo",
      "chain.params[].default",
      "chain.params[].unit",
      "chain.params[].uiMin",
      "chain.params[].uiMax",
      "chain.params[].scale",
      "chain.params[].choices",
      "chain.params[].choices[].name",
      "chain.params[].choices[].value",
      "chain.params[].slot",
      "chain.params[].dependsOn",
      "chain.params[].coupledWith",
  };
  return paths;
}

}  // namespace sonare_c_mastering_detail

const char* sonare_mastering_insert_param_names(const char* insert) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  // Unlike the no-arg *_names getters this depends on the argument, so it cannot
  // use the program-lifetime cache: recompute into a thread-local each call
  // (valid until the next call on the same thread, like sonare_last_error_message).
  static thread_local std::string names;
  names.clear();
  if (insert == nullptr) {
    refuse_null("insert");
    return nullptr;
  }
  const auto list = sonare::mastering::api::insert_param_names(insert);
  for (size_t index = 0; index < list.size(); ++index) {
    if (index > 0) names += '\n';
    names += list[index];
  }
  return names.c_str();
  SONARE_C_CATCH_RETURN(nullptr)
}

SonareError sonare_mastering_insert_param_info(const char* insert, int sample_rate,
                                               char** out_json) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_json);
  SONARE_C_REQUIRE_NONNULL(insert);
  SONARE_C_TRY
  const std::string name = insert;
  // Rate 0 is the rate-less query; the core refuses an undeclared name, then any other rate.
  std::string info =
      sample_rate == 0 ? sonare::mastering::api::insert_param_info_json(name)
                       : sonare::mastering::api::insert_param_info_json_at_rate(name, sample_rate);
  SONARE_C_REQUIRE_NONNULL(out_json);
  *out_json = copy_string(info);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_insert_timing(const char* insert, const char* params_json,
                                           int sample_rate, int* out_latency_samples,
                                           int* out_tail_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_latency_samples, out_tail_samples);
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(out_latency_samples);
  SONARE_C_REQUIRE_NONNULL(out_tail_samples);
  const auto timing = sonare::mastering::api::insert_timing(
      insert != nullptr ? insert : "", params_json != nullptr ? params_json : "{}", sample_rate);
  *out_latency_samples = timing.latency_samples;
  *out_tail_samples = timing.tail_samples;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_enum_value(const char* processor, const char* key, const char* name,
                                        int* out_is_enum, double* out_value) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_is_enum, out_value);
  SONARE_C_REQUIRE_NONNULL(key);
  SONARE_C_REQUIRE_NONNULL(name);
  SONARE_C_REQUIRE_NONNULL(out_is_enum);
  SONARE_C_REQUIRE_NONNULL(out_value);
  SONARE_C_TRY
  const auto value = sonare::mastering::api::mastering_enum_value(
      processor != nullptr ? processor : "", key, name);
  if (value.has_value()) {
    *out_is_enum = 1;
    *out_value = *value;
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_apply_pair_processor(const char* processor, const float* source,
                                                  size_t source_length, const float* reference,
                                                  size_t reference_length,
                                                  int reference_sample_rate, int sample_rate,
                                                  const SonareMasteringParam* params,
                                                  size_t param_count, SonareMasteringResult* out) {
  SONARE_C_API_ENTRY;
  // Define the result before any validation return, so a rejected call hands
  // back a defined struct rather than the caller's untouched stack slot.
  SONARE_C_ZERO_OUTS(out);
  if (out != nullptr) out->sample_rate = sample_rate;

  SONARE_C_REQUIRE_NONNULL(processor);
  if (const SonareError empty_err =
          run_validation([&] { util::check::non_empty(processor, "processor"); });
      empty_err != SONARE_OK) {
    return empty_err;
  }
  SonareError err = validate_audio_params("source", source, source_length, sample_rate);
  if (err != SONARE_OK) return err;
  err = validate_audio_params("reference", reference, reference_length, reference_sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(params, param_count);
  SONARE_C_REQUIRE_NONNULL(out);

  SONARE_C_TRY
  auto result = sonare::mastering::api::apply_named_pair_processor(
      processor, source, reference, source_length, reference_length, sample_rate,
      reference_sample_rate, to_params(params, param_count));
  set_mastering_result(result, out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_apply_pair_processor_stereo(
    const char* processor, const float* source_left, const float* source_right,
    size_t source_length, const float* reference_left, const float* reference_right,
    size_t reference_length, int reference_sample_rate, int sample_rate,
    const SonareMasteringParam* params, size_t param_count, SonareMasteringStereoResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  if (out != nullptr) out->sample_rate = sample_rate;
  SONARE_C_REQUIRE_NONNULL(processor);
  if (const SonareError empty_err =
          run_validation([&] { util::check::non_empty(processor, "processor"); });
      empty_err != SONARE_OK) {
    return empty_err;
  }

  SonareError err = validate_stereo_audio_params("source_left", "source_right", source_left,
                                                 source_right, source_length, sample_rate);
  if (err != SONARE_OK) return err;
  err = validate_stereo_audio_params("reference_left", "reference_right", reference_left,
                                     reference_right, reference_length, reference_sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(params, param_count);
  SONARE_C_REQUIRE_NONNULL(out);

  SONARE_C_TRY
  const auto result = sonare::mastering::api::apply_named_pair_processor_stereo(
      processor, source_left, source_right, source_length, reference_left, reference_right,
      reference_length, sample_rate, reference_sample_rate, to_params(params, param_count));
  set_stereo_result(result, out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_analyze_pair(const char* analysis, const float* source,
                                          size_t source_length, const float* reference,
                                          size_t reference_length, int reference_sample_rate,
                                          int sample_rate, const SonareMasteringParam* params,
                                          size_t param_count, char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
  SONARE_C_REQUIRE_NONNULL(analysis);
  SonareError err = validate_audio_params("source", source, source_length, sample_rate);
  if (err != SONARE_OK) return err;
  err = validate_audio_params("reference", reference, reference_length, reference_sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(params, param_count);
  SONARE_C_REQUIRE_NONNULL(json_out);
  SONARE_C_TRY
  auto json = sonare::mastering::api::analyze_named_pair(
      analysis, source, reference, source_length, reference_length, sample_rate,
      reference_sample_rate, to_params(params, param_count));
  *json_out = copy_string(json);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_analyze_pair_stereo(
    const char* analysis, const float* source_left, const float* source_right, size_t source_length,
    const float* reference_left, const float* reference_right, size_t reference_length,
    int reference_sample_rate, int sample_rate, const SonareMasteringParam* params,
    size_t param_count, char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
  SONARE_C_REQUIRE_NONNULL(analysis);
  SonareError err = validate_stereo_audio_params("source_left", "source_right", source_left,
                                                 source_right, source_length, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_TRY
  constexpr std::array<CName, 1> kReferenceRateNames{{{"sample_rate", "reference_sample_rate"}}};
  with_c_names(kReferenceRateNames, [&] {
    validate_offline_audio_input("reference_left", reference_left, reference_length,
                                 reference_sample_rate);
    validate_offline_audio_input("reference_right", reference_right, reference_length,
                                 reference_sample_rate);
  });
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(params, param_count);
  SONARE_C_REQUIRE_NONNULL(json_out);
  auto json = sonare::mastering::api::analyze_named_pair(
      analysis, source_left, source_right, source_length, reference_left, reference_right,
      reference_length, sample_rate, reference_sample_rate, to_params(params, param_count));
  *json_out = copy_string(json);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_validate_mastering_stereo_analysis_params(const char* analysis,
                                                             const SonareMasteringParam* params,
                                                             size_t param_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(analysis);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(params, param_count);
  SONARE_C_TRY
  const auto cpp_params = to_params(params, param_count);
  sonare::mastering::api::validate_named_stereo_analysis_params(analysis, cpp_params);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_analyze_stereo(const char* analysis, const float* left,
                                            const float* right, size_t length, int sample_rate,
                                            const SonareMasteringParam* params, size_t param_count,
                                            char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
  SONARE_C_REQUIRE_NONNULL(analysis);
  SonareError err = validate_stereo_audio_params("left", "right", left, right, length, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(params, param_count);
  SONARE_C_REQUIRE_NONNULL(json_out);
  SONARE_C_TRY
  const auto cpp_params = to_params(params, param_count);
  const auto json = sonare::mastering::api::analyze_named_stereo(analysis, left, right, length,
                                                                 sample_rate, cpp_params);
  *json_out = copy_string(json);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_ab_match_loudness(const float* source, size_t source_length,
                                               const float* reference, size_t reference_length,
                                               int reference_sample_rate, int sample_rate,
                                               float** out, size_t* out_length,
                                               SonareLoudnessMatch* out_match) {
  SONARE_C_API_ENTRY;
  // Cleared before the argument checks, not after: a caller that ignores the
  // error code must not read the scalars left in the struct by whatever wrote
  // it last as though this call had measured them.
  SONARE_C_ZERO_OUTS(out, out_length, out_match);
  SonareError err = validate_audio_params("source", source, source_length, sample_rate);
  if (err != SONARE_OK) return err;
  err = validate_audio_params("reference", reference, reference_length, reference_sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);

  SONARE_C_TRY
  const Audio reference_audio =
      Audio::from_buffer(reference, reference_length, reference_sample_rate);
  const Audio source_audio = Audio::from_buffer(source, source_length, sample_rate);
  // The core names the reference `a` and the matched side `b`.
  const std::array<CName, 2> names{{{"a", "reference"}, {"b", "source"}}};
  const auto matched = with_c_names(names, [&] {
    return sonare::mastering::match::ab_match_loudness(reference_audio, source_audio);
  });
  if (out_match) {
    *out_match = SonareLoudnessMatch{matched.reference_lufs, matched.source_lufs,
                                     matched.applied_gain_db, matched.matched_true_peak_dbtp};
  }
  return copy_audio_result(matched.b, out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_mastering_ab_match_loudness_stereo(
    const float* source_left, const float* source_right, size_t source_length,
    const float* reference_left, const float* reference_right, size_t reference_length,
    int reference_sample_rate, int sample_rate, float** out_left, float** out_right,
    size_t* out_length, SonareLoudnessMatch* out_match) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_left, out_right, out_length, out_match);

  SonareError err = validate_stereo_audio_params("source_left", "source_right", source_left,
                                                 source_right, source_length, sample_rate);
  if (err != SONARE_OK) return err;
  err = validate_stereo_audio_params("reference_left", "reference_right", reference_left,
                                     reference_right, reference_length, reference_sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out_left);
  SONARE_C_REQUIRE_NONNULL(out_right);
  SONARE_C_REQUIRE_NONNULL(out_length);

  SONARE_C_TRY
  const sonare::mastering::match::StereoAudioPair reference{
      Audio::from_buffer(reference_left, reference_length, reference_sample_rate),
      Audio::from_buffer(reference_right, reference_length, reference_sample_rate)};
  const sonare::mastering::match::StereoAudioPair source{
      Audio::from_buffer(source_left, source_length, sample_rate),
      Audio::from_buffer(source_right, source_length, sample_rate)};
  const std::array<CPairName, 2> pairs{{{"reference", "reference_left", "reference_right"},
                                        {"source", "source_left", "source_right"}}};
  const auto matched = with_c_pair_names(
      pairs, [&] { return sonare::mastering::match::ab_match_loudness_stereo(reference, source); });
  const std::size_t length = matched.b.left.size();
  std::unique_ptr<float[]> left;
  std::unique_ptr<float[]> right;
  if (length != 0) {
    // Allocate before publishing any field.
    left = std::make_unique<float[]>(length);
    right = std::make_unique<float[]>(length);
    std::memcpy(left.get(), matched.b.left.data(), length * sizeof(float));
    std::memcpy(right.get(), matched.b.right.data(), length * sizeof(float));
  }

  const SonareLoudnessMatch match{matched.reference_lufs, matched.source_lufs,
                                  matched.applied_gain_db, matched.matched_true_peak_dbtp};
  *out_length = length;
  *out_left = release_array(left);
  *out_right = release_array(right);
  if (out_match) *out_match = match;
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_free_mastering_result(SonareMasteringResult* result) {
  if (!result) return;
  delete[] result->samples;
  *result = {};
}

void sonare_free_mastering_stereo_result(SonareMasteringStereoResult* result) {
  if (!result) return;
  delete[] result->left;
  delete[] result->right;
  *result = {};
}
