#include "mastering/assistant/repair_session.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "mastering/api/param_field_tables.h"
#include "mastering/api/repair_params.h"
#include "mastering/assistant/audio_profile.h"
#include "mastering/assistant/suggester.h"
#include "mastering/common/loudness_measure.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/json.h"
#include "util/json_schema.h"
#include "util/parameter_checks.h"
#include "util/resource_limits.h"

namespace sonare::mastering::assistant {
namespace {

namespace json = sonare::util::json;
namespace schema = sonare::util::json::schema;
using api::detail::ParamMap;

enum class Stage { Declip, Declick, Decrackle, Dehum, Denoise, Dereverb };

/// The chain's repair order, widest damage first.
constexpr std::array<Stage, 6> kStageOrder = {Stage::Declip, Stage::Declick, Stage::Decrackle,
                                              Stage::Dehum,  Stage::Denoise, Stage::Dereverb};

const char* stage_name(Stage stage) {
  switch (stage) {
    case Stage::Declip:
      return "declip";
    case Stage::Declick:
      return "declick";
    case Stage::Decrackle:
      return "decrackle";
    case Stage::Dehum:
      return "dehum";
    case Stage::Denoise:
      return "denoise";
    case Stage::Dereverb:
      return "dereverb";
  }
  return "";
}

/// The stage name the chain reports progress under.
const char* progress_name(Stage stage) {
  switch (stage) {
    case Stage::Declip:
      return "repair.declip";
    case Stage::Declick:
      return "repair.declick";
    case Stage::Decrackle:
      return "repair.decrackle";
    case Stage::Dehum:
      return "repair.dehum";
    case Stage::Denoise:
      return "repair.denoise";
    case Stage::Dereverb:
      return "repair.dereverb";
  }
  return "";
}

bool is_linked(Stage stage) { return stage == Stage::Denoise || stage == Stage::Dereverb; }

std::optional<Stage> stage_from_name(const std::string& name) {
  for (Stage stage : kStageOrder) {
    if (name == stage_name(stage)) return stage;
  }
  return std::nullopt;
}

[[noreturn]] void refuse(const std::string& message) {
  throw SonareException(ErrorCode::InvalidParameter, message);
}

/// Refuses a value of the stages argument at @p path for failing @p predicate.
[[noreturn]] void refuse_stages(const std::string& path, const std::string& predicate) {
  throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, path, predicate);
}

/// Refuses a value of the wrong JSON type at @p path of the stages argument.
[[noreturn]] void refuse_stages_type(const std::string& path, schema::Kind kind) {
  throw SonareParameterRefusal::wrong_type(ErrorCode::InvalidParameter, path,
                                           std::string("must be ") + schema::kind_text(kind));
}

/// Parses the stages argument, refusing malformed JSON and a repeated key under `stages`.
json::Value parse_stages_text(const std::string& text) {
  try {
    return json::parse_strict(text);
  } catch (const json::JsonError& e) {
    refuse_stages("stages", std::string("must be valid JSON: ") + e.what());
  }
}

void validate_channel_set(const char* name, const float* const* channels, std::size_t channel_count,
                          std::size_t length, int sample_rate) {
  if (channels == nullptr || channel_count == 0) refuse("channels must not be empty");
  if (length == 0) refuse("audio must not be empty");
  if (sample_rate <= 0) refuse("sample_rate must be positive");
  for (std::size_t c = 0; c < channel_count; ++c) {
    if (channels[c] == nullptr) refuse("channel " + std::to_string(c) + " must not be null");
    const std::string channel_name = std::string(name) + "[" + std::to_string(c) + "]";
    validate_offline_audio_input(channel_name.c_str(), channels[c], length, sample_rate);
  }
}

// ---------------------------------------------------------------------------
// Settings, through each stage's own field table
// ---------------------------------------------------------------------------

template <typename T>
json::Value setting_value(const T& value) {
  if constexpr (std::is_enum_v<T>) {
    const char* name = api::detail::enum_choice_name(value);
    return json::Value(std::string(name != nullptr ? name : ""));
  } else if constexpr (std::is_same_v<T, bool>) {
    return json::Value(value);
  } else {
    return json::Value(static_cast<double>(value));
  }
}

#define SONARE_WRITE_SETTING(key, member, meta) out.emplace(key, setting_value(config.member));

json::Object settings_of(const repair::DeclipConfig& config) {
  json::Object out;
  out.emplace(api::detail::kDeclipThresholdKey,
              setting_value(sonare::linear_to_db(config.clip_threshold)));
  SONARE_FIELDS_DECLIP(SONARE_WRITE_SETTING)
  return out;
}

json::Object settings_of(const repair::DeclickConfig& config) {
  json::Object out;
  SONARE_FIELDS_DECLICK(SONARE_WRITE_SETTING)
  return out;
}

json::Object settings_of(const repair::DecrackleConfig& config) {
  json::Object out;
  SONARE_FIELDS_DECRACKLE(SONARE_WRITE_SETTING)
  return out;
}

json::Object settings_of(const repair::DehumConfig& config) {
  json::Object out;
  SONARE_FIELDS_DEHUM(SONARE_WRITE_SETTING)
  return out;
}

json::Object settings_of(const repair::DenoiseClassicalConfig& config) {
  json::Object out;
  SONARE_FIELDS_DENOISE_CLASSICAL(SONARE_WRITE_SETTING)
  return out;
}

json::Object settings_of(const repair::DereverbClassicalConfig& config) {
  json::Object out;
  SONARE_FIELDS_DEREVERB_CLASSICAL(SONARE_WRITE_SETTING)
  return out;
}

#undef SONARE_WRITE_SETTING

/// Refuses an unknown setting by its path.
[[noreturn]] void refuse_unknown_setting(const std::string& stage_path, const std::string& key) {
  throw SonareParameterRefusal::unknown(ErrorCode::InvalidParameter,
                                        schema::child_path(stage_path, key));
}

std::string choice_names(const std::vector<api::detail::EnumChoice>& choices) {
  std::string out;
  for (const api::detail::EnumChoice& choice : choices) {
    if (!out.empty()) out += ", ";
    out += choice.name;
  }
  return out;
}

/// Reads one stage object through @p reader, the stage's catalog config builder. An empty build
/// first declares the keys and enum choices the builder reads, so an unknown key is refused by
/// name rather than ignored and a choice may be given by name.
template <typename Reader>
auto read_settings(const json::Object& object, const std::string& stage_path, Reader reader) {
  ParamMap declared;
  (void)reader(declared);
  ParamMap map;
  for (const auto& [key, value] : object) {
    if (key == "stage") continue;
    if (declared.probed_keys().count(key) == 0) refuse_unknown_setting(stage_path, key);
    if (value.is_bool()) {
      map[key] = value.as_bool() ? 1.0 : 0.0;
    } else if (value.is_number()) {
      map[key] = value.as_number();
    } else if (value.is_string()) {
      const auto choices = declared.probed_choices().find(key);
      if (choices == declared.probed_choices().end())
        refuse_stages_type(schema::child_path(stage_path, key), schema::Kind::Number);
      const std::string& name = value.as_string();
      const auto choice =
          std::find_if(choices->second.begin(), choices->second.end(),
                       [&name](const api::detail::EnumChoice& c) { return c.name == name; });
      if (choice == choices->second.end()) {
        refuse_stages(schema::child_path(stage_path, key),
                      "must be one of " + choice_names(choices->second));
      }
      map[key] = static_cast<double>(choice->value);
    } else {
      throw SonareParameterRefusal::wrong_type(ErrorCode::InvalidParameter,
                                               schema::child_path(stage_path, key),
                                               "must be a number, a boolean or a choice name");
    }
  }
  // A processor names a setting by its C field; the stage key is that name in camel case.
  try {
    return reader(map);
  } catch (const SonareParameterRefusal& e) {
    if (e.code() != ErrorCode::InvalidParameter) throw;
    throw e.respelled(
        ErrorCode::InvalidParameter,
        [&stage_path](const std::string& subject) {
          return schema::child_path(stage_path, util::check::camel_path(subject));
        },
        true);
  }
}

/// The stages one apply call runs, each present at most once.
struct StagePlan {
  std::optional<repair::DeclipConfig> declip;
  std::optional<repair::DeclickConfig> declick;
  std::optional<repair::DecrackleConfig> decrackle;
  std::optional<repair::DehumConfig> dehum;
  std::optional<repair::DenoiseClassicalConfig> denoise;
  std::optional<repair::DereverbClassicalConfig> dereverb;

  bool has(Stage stage) const {
    switch (stage) {
      case Stage::Declip:
        return declip.has_value();
      case Stage::Declick:
        return declick.has_value();
      case Stage::Decrackle:
        return decrackle.has_value();
      case Stage::Dehum:
        return dehum.has_value();
      case Stage::Denoise:
        return denoise.has_value();
      case Stage::Dereverb:
        return dereverb.has_value();
    }
    return false;
  }
};

StagePlan parse_stages(const std::string& stages_json) {
  const json::Value root = parse_stages_text(stages_json.empty() ? "[]" : stages_json);
  if (!root.is_array()) refuse_stages_type("stages", schema::Kind::Array);
  StagePlan plan;
  std::size_t position = 0;
  for (const json::Value& entry : root.as_array()) {
    const std::string stage_path = schema::index_path("stages", position++);
    if (!entry.is_object()) refuse_stages_type(stage_path, schema::Kind::Object);
    const json::Object& object = entry.as_object();
    const std::string name_path = schema::child_path(stage_path, "stage");
    const auto name = object.find("stage");
    if (name == object.end()) refuse_stages(name_path, "must be given");
    if (!name->second.is_string()) {
      refuse_stages_type(name_path, schema::Kind::String);
    }
    const std::optional<Stage> stage = stage_from_name(name->second.as_string());
    if (!stage) {
      refuse_stages(name_path,
                    "must be one of declip, declick, decrackle, dehum, denoise, dereverb");
    }
    if (plan.has(*stage)) {
      refuse_stages(name_path, "must not repeat a stage given earlier");
    }
    switch (*stage) {
      case Stage::Declip:
        plan.declip = read_settings(object, stage_path, api::detail::declip_config);
        break;
      case Stage::Declick:
        plan.declick = read_settings(object, stage_path, api::detail::declick_config);
        break;
      case Stage::Decrackle:
        plan.decrackle = read_settings(object, stage_path, api::detail::decrackle_config);
        break;
      case Stage::Dehum:
        plan.dehum = read_settings(object, stage_path, api::detail::dehum_config);
        break;
      case Stage::Denoise:
        plan.denoise = read_settings(object, stage_path, api::detail::denoise_classical_config);
        break;
      case Stage::Dereverb:
        plan.dereverb = read_settings(object, stage_path, api::detail::dereverb_classical_config);
        break;
    }
  }
  return plan;
}

// ---------------------------------------------------------------------------
// Reports, in the field names every surface already uses for them
// ---------------------------------------------------------------------------

double count(std::size_t value) { return static_cast<double>(value); }

json::Array changes_of(const std::vector<Audio>& input, const std::vector<Audio>& output) {
  json::Array changes;
  changes.reserve(input.size());
  for (std::size_t channel = 0; channel < input.size(); ++channel) {
    std::size_t changed_samples = 0;
    double squared_difference_sum = 0.0;
    const float* const before_samples = input[channel].data();
    const float* const after_samples = output[channel].data();
    const std::size_t length = input[channel].size();
    for (std::size_t sample = 0; sample < length; ++sample) {
      const float before = before_samples[sample];
      const float after = after_samples[sample];
      if (before != after) ++changed_samples;
      const double difference = static_cast<double>(after) - static_cast<double>(before);
      squared_difference_sum += difference * difference;
    }
    const double mean_squared_difference = squared_difference_sum / static_cast<double>(length);
    json::Object change;
    json::put(change, "changedSamples", count(changed_samples));
    json::put(change, "changeLevelDb", sonare::power_to_level_db(mean_squared_difference));
    changes.emplace_back(std::move(change));
  }
  return changes;
}

json::Object report_of(const repair::DeclipReport& report) {
  json::Object out;
  json::put(out, "detected", detection_to_json(report.detected));
  json::put(out, "lpcReconstructedRuns", count(report.lpc_reconstructed_runs));
  json::put(out, "unreconstructedRuns", count(report.unreconstructed_runs));
  json::put(out, "repairedSamples", count(report.repaired_samples));
  json::put(out, "linkedRuns", count(report.linked_runs));
  return out;
}

json::Object report_of(const repair::DeclickReport& report) {
  json::Object out;
  json::put(out, "detected", detection_to_json(report.detected));
  json::put(out, "repairedRuns", count(report.repaired_runs));
  json::put(out, "repairedSamples", count(report.repaired_samples));
  json::put(out, "linkedRuns", count(report.linked_runs));
  json::put(out, "lpcModelUsed", report.lpc_model_used);
  return out;
}

json::Object report_of(const repair::DecrackleReport& report) {
  json::Object out;
  json::put(out, "detected", detection_to_json(report.detected));
  json::put(out, "replacedSamples", count(report.replaced_samples));
  json::put(out, "detailCoefficients", count(report.detail_coefficients));
  json::put(out, "shrunkCoefficients", count(report.shrunk_coefficients));
  json::put(out, "noiseSigma", report.noise_sigma);
  return out;
}

json::Object report_of(const repair::DehumReport& report) {
  json::Object out;
  json::put(out, "detected", detection_to_json(report.detected));
  json::put(out, "notchedHarmonics", report.notched_harmonics);
  json::put(out, "appliedFundamentalHz", report.applied_fundamental_hz);
  json::put(out, "fundamentalDriftHz", report.fundamental_drift_hz);
  return out;
}

json::Object report_of(const repair::DenoiseReport& report) {
  json::Object out;
  json::put(out, "detected", detection_to_json(report.detected));
  json::put(out, "meanReductionDb", report.mean_reduction_db);
  json::put(out, "maxReductionDb", report.max_reduction_db);
  json::put(out, "floorLimitedFraction", report.floor_limited_fraction);
  return out;
}

json::Object report_of(const repair::DereverbReport& report) {
  json::Object out;
  json::put(out, "detected", detection_to_json(report.detected));
  json::put(out, "meanReductionDb", report.mean_reduction_db);
  json::put(out, "suppressedFraction", report.suppressed_fraction);
  json::put(out, "wpePredictorNorm", report.wpe_predictor_norm);
  return out;
}

template <typename Report>
json::Value stage_entry(Stage stage, const std::vector<Report>& reports) {
  json::Array list;
  for (const Report& report : reports) list.emplace_back(report_of(report));
  json::Object out;
  json::put(out, "stage", stage_name(stage));
  json::put(out, "scope", is_linked(stage) ? "linked" : "channel");
  json::put(out, "reports", std::move(list));
  return json::Value(std::move(out));
}

/// Runs @p stage over @p working in place and returns its report entry.
json::Value run_stage(Stage stage, const StagePlan& plan, std::vector<Audio>& working) {
  std::vector<const Audio*> inputs;
  for (const Audio& channel : working) inputs.push_back(&channel);
  std::vector<Audio> out;
  json::Value entry;
  switch (stage) {
    case Stage::Declip:
      entry = stage_entry(stage,
                          repair::declip_linked(inputs.data(), inputs.size(), &out, *plan.declip));
      break;
    case Stage::Declick:
      entry = stage_entry(
          stage, repair::declick_linked(inputs.data(), inputs.size(), &out, *plan.declick));
      break;
    case Stage::Decrackle: {
      // Crackle is surface damage with no common event across channels, so each runs alone.
      std::vector<repair::DecrackleReport> reports(working.size());
      for (std::size_t c = 0; c < working.size(); ++c) {
        out.push_back(repair::decrackle(working[c], *plan.decrackle, &reports[c]));
      }
      entry = stage_entry(stage, reports);
      break;
    }
    case Stage::Dehum:
      entry =
          stage_entry(stage, repair::dehum_linked(inputs.data(), inputs.size(), &out, *plan.dehum));
      break;
    case Stage::Denoise:
      entry =
          stage_entry(stage, std::vector<repair::DenoiseReport>{repair::denoise_classical_linked(
                                 inputs.data(), inputs.size(), &out, *plan.denoise)});
      break;
    case Stage::Dereverb:
      entry =
          stage_entry(stage, std::vector<repair::DereverbReport>{repair::dereverb_classical_linked(
                                 inputs.data(), inputs.size(), &out, *plan.dereverb)});
      break;
  }
  json::put(entry.as_object(), "changes", changes_of(working, out));
  working = std::move(out);
  return entry;
}

// ---------------------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------------------

AssistantConfig read_analyze_request(const std::string& request_json) {
  AssistantConfig config;
  if (request_json.empty()) return config;
  constexpr const char* kRoot = "request";
  json::Value root;
  try {
    root = json::parse_strict(request_json);
  } catch (const json::JsonError& e) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, kRoot,
                                            std::string("must be valid JSON: ") + e.what());
  }
  if (!root.is_object()) {
    throw SonareParameterRefusal::wrong_type(
        ErrorCode::InvalidParameter, kRoot,
        std::string("must be ") + schema::kind_text(schema::Kind::Object));
  }
  for (const auto& [key, value] : root.as_object()) {
    const std::string path = schema::child_path(kRoot, key);
    if (key != "preferStreamingSafe") {
      throw SonareParameterRefusal::unknown(ErrorCode::InvalidParameter, path, std::string(),
                                            "preferStreamingSafe");
    }
    if (!value.is_bool()) {
      throw SonareParameterRefusal::wrong_type(
          ErrorCode::InvalidParameter, path,
          std::string("must be ") + schema::kind_text(schema::Kind::Boolean));
    }
    config.prefer_streaming_safe = value.as_bool();
  }
  return config;
}

json::Value recommended_entry(Stage stage, json::Object settings) {
  json::put(settings, "stage", stage_name(stage));
  return json::Value(std::move(settings));
}

json::Array recommended_stages(const api::RepairChainConfig& repair) {
  json::Array out;
  for (Stage stage : kStageOrder) {
    switch (stage) {
      case Stage::Declip:
        if (repair.declip.enabled)
          out.push_back(recommended_entry(stage, settings_of(repair.declip.config)));
        break;
      case Stage::Declick:
        if (repair.declick.enabled)
          out.push_back(recommended_entry(stage, settings_of(repair.declick.config)));
        break;
      case Stage::Decrackle:
        if (repair.decrackle.enabled) {
          out.push_back(recommended_entry(stage, settings_of(repair.decrackle.config)));
        }
        break;
      case Stage::Dehum:
        if (repair.dehum.enabled)
          out.push_back(recommended_entry(stage, settings_of(repair.dehum.config)));
        break;
      case Stage::Denoise:
        if (repair.denoise.enabled)
          out.push_back(recommended_entry(stage, settings_of(repair.denoise.config)));
        break;
      case Stage::Dereverb:
        if (repair.dereverb.enabled) {
          out.push_back(recommended_entry(stage, settings_of(repair.dereverb.config)));
        }
        break;
    }
  }
  return out;
}

}  // namespace

std::string repair_analyze_json(const float* const* channels, std::size_t channel_count,
                                std::size_t length, int sample_rate,
                                const std::string& request_json) {
  validate_channel_set("channels", channels, channel_count, length, sample_rate);
  const std::size_t interleaved_count = util::check::element_count(
      static_cast<std::uint64_t>(length), static_cast<std::uint64_t>(channel_count),
      resource::kMaxOfflineAudioSamples, "num_channels", "the interleaved audio");
  const AssistantConfig config = read_analyze_request(request_json);

  const ChannelSetDefects defects =
      measure_defects_planar(channels, channel_count, length, sample_rate);
  std::vector<float> interleaved(interleaved_count);
  for (std::size_t frame = 0; frame < length; ++frame) {
    for (std::size_t c = 0; c < channel_count; ++c) {
      interleaved[frame * channel_count + c] = channels[c][frame];
    }
  }
  AudioProfile profile;
  profile.loudness.integrated_lufs =
      common::measure_loudness_summary_interleaved(interleaved.data(), length,
                                                   static_cast<int>(channel_count), sample_rate)
          .integrated_lufs;
  profile.defects = defects.aggregate;

  api::MasteringChainConfig chain;
  std::vector<std::string> explanation;
  select_repair_stages(profile, config, chain, explanation);

  json::Array reasons;
  for (const std::string& reason : explanation) reasons.emplace_back(reason);

  json::Object root;
  // Every channel has the same length, so the detectors ran on all of them or on none; when on
  // none, both blocks are left out.
  if (defects.aggregate.measured) {
    json::Array per_channel;
    for (const DefectProfile& channel : defects.channels) {
      per_channel.emplace_back(defects_to_json(channel));
    }
    json::put(root, "defects", defects_to_json(defects.aggregate));
    json::put(root, "channels", std::move(per_channel));
  }
  json::put(root, "declipThresholdSafe", defects.aggregate.declip_threshold_safe);
  json::put(root, "integratedLufs", profile.loudness.integrated_lufs);
  json::put(root, "recommended", recommended_stages(chain.repair));
  json::put(root, "explanation", std::move(reasons));
  return json::dump(json::Value(std::move(root)));
}

bool repair_apply_json(const float* const* channels, std::size_t channel_count, std::size_t length,
                       int sample_rate, const std::string& stages_json, float* const* out_channels,
                       std::string* reports_json, const RepairProgressCallback& progress,
                       const RepairCancelCallback& cancel) {
  validate_channel_set("channels", channels, channel_count, length, sample_rate);
  if (out_channels == nullptr) refuse("output channels must not be null");
  for (std::size_t c = 0; c < channel_count; ++c) {
    if (out_channels[c] == nullptr)
      refuse("output channel " + std::to_string(c) + " must not be null");
  }
  if (reports_json == nullptr) refuse("reports output must not be null");
  const StagePlan plan = parse_stages(stages_json);

  // Copied before the first callback, so a callback that releases the caller's input is harmless.
  std::vector<Audio> working;
  working.reserve(channel_count);
  for (std::size_t c = 0; c < channel_count; ++c) {
    working.push_back(Audio::from_buffer(channels[c], length, sample_rate));
  }
  const auto total = static_cast<float>(
      std::count_if(kStageOrder.begin(), kStageOrder.end(), [&](Stage s) { return plan.has(s); }));
  float done = 0.0f;
  json::Array reports;
  for (Stage stage : kStageOrder) {
    if (!plan.has(stage)) continue;
    reports.push_back(run_stage(stage, plan, working));
    done += 1.0f;
    if (progress) progress(done / total, progress_name(stage));
    if (cancel && cancel()) return false;
  }

  for (std::size_t c = 0; c < channel_count; ++c) {
    std::copy(working[c].data(), working[c].data() + length, out_channels[c]);
  }
  *reports_json = json::dump(json::Value(std::move(reports)));
  return true;
}

const std::vector<std::string>& repair_analysis_schema_paths() {
  static const std::vector<std::string> paths = {
      "channels",
      "channels[].click",
      "channels[].click.count",
      "channels[].click.longestRunSamples",
      "channels[].click.perSecond",
      "channels[].click.rejected",
      "channels[].clip",
      "channels[].clip.clippedRatio",
      "channels[].clip.clippedSamples",
      "channels[].clip.flatLevel",
      "channels[].clip.flatRunCount",
      "channels[].clip.flatSampleCount",
      "channels[].clip.longestFlatRunSamples",
      "channels[].clip.longestRunSamples",
      "channels[].clip.runCount",
      "channels[].crackle",
      "channels[].crackle.perSecond",
      "channels[].crackle.sampleCount",
      "channels[].crackle.sampleFraction",
      "channels[].hum",
      "channels[].hum.fundamentalHz",
      "channels[].hum.fundamentalProminence",
      "channels[].hum.harmonicDbfs",
      "channels[].hum.harmonics",
      "channels[].noise",
      "channels[].noise.bandFloorBand",
      "channels[].noise.bandFloorDbfs",
      "channels[].noise.floorDbfs",
      "channels[].reverb",
      "channels[].reverb.lateDecayRatioDb",
      "declipThresholdSafe",
      "defects",
      "defects.click",
      "defects.click.count",
      "defects.click.longestRunSamples",
      "defects.click.perSecond",
      "defects.click.rejected",
      "defects.clip",
      "defects.clip.clippedRatio",
      "defects.clip.clippedSamples",
      "defects.clip.flatLevel",
      "defects.clip.flatRunCount",
      "defects.clip.flatSampleCount",
      "defects.clip.longestFlatRunSamples",
      "defects.clip.longestRunSamples",
      "defects.clip.runCount",
      "defects.crackle",
      "defects.crackle.perSecond",
      "defects.crackle.sampleCount",
      "defects.crackle.sampleFraction",
      "defects.hum",
      "defects.hum.fundamentalHz",
      "defects.hum.fundamentalProminence",
      "defects.hum.harmonicDbfs",
      "defects.hum.harmonics",
      "defects.noise",
      "defects.noise.bandFloorBand",
      "defects.noise.bandFloorDbfs",
      "defects.noise.floorDbfs",
      "defects.reverb",
      "defects.reverb.lateDecayRatioDb",
      "explanation",
      "integratedLufs",
      "recommended",
      "recommended[].stage",
  };
  return paths;
}

const std::vector<std::pair<std::string, std::string>>& repair_analysis_null_means() {
  static const std::vector<std::pair<std::string, std::string>> means = {
      {"channels[].hum.harmonicDbfs[]", "-Infinity"},
      {"channels[].noise.bandFloorDbfs[]", "-Infinity"},
      {"channels[].noise.floorDbfs", "-Infinity"},
      {"defects.hum.harmonicDbfs[]", "-Infinity"},
      {"defects.noise.bandFloorDbfs[]", "-Infinity"},
      {"defects.noise.floorDbfs", "-Infinity"},
      {"integratedLufs", "-Infinity"},
  };
  return means;
}

const std::vector<std::string>& repair_apply_schema_paths() {
  static const std::vector<std::string> paths = {
      "[].changes",
      "[].changes[].changeLevelDb",
      "[].changes[].changedSamples",
      "[].reports",
      "[].reports[].detected",
      "[].reports[].detected.bandFloorBand",
      "[].reports[].detected.bandFloorDbfs",
      "[].reports[].detected.clippedRatio",
      "[].reports[].detected.clippedSamples",
      "[].reports[].detected.count",
      "[].reports[].detected.flatLevel",
      "[].reports[].detected.flatRunCount",
      "[].reports[].detected.flatSampleCount",
      "[].reports[].detected.fundamentalHz",
      "[].reports[].detected.fundamentalProminence",
      "[].reports[].detected.harmonicDbfs",
      "[].reports[].detected.harmonics",
      "[].reports[].detected.lateDecayRatioDb",
      "[].reports[].detected.latePredictability",
      "[].reports[].detected.longestFlatRunSamples",
      "[].reports[].detected.longestRunSamples",
      "[].reports[].detected.perSecond",
      "[].reports[].detected.rejected",
      "[].reports[].detected.runCount",
      "[].reports[].detected.sampleCount",
      "[].reports[].detected.sampleFraction",
      "[].reports[].detected.floorDbfs",
      "[].reports[].appliedFundamentalHz",
      "[].reports[].detailCoefficients",
      "[].reports[].floorLimitedFraction",
      "[].reports[].fundamentalDriftHz",
      "[].reports[].linkedRuns",
      "[].reports[].lpcModelUsed",
      "[].reports[].lpcReconstructedRuns",
      "[].reports[].meanReductionDb",
      "[].reports[].maxReductionDb",
      "[].reports[].noiseSigma",
      "[].reports[].notchedHarmonics",
      "[].reports[].repairedRuns",
      "[].reports[].repairedSamples",
      "[].reports[].replacedSamples",
      "[].reports[].shrunkCoefficients",
      "[].reports[].suppressedFraction",
      "[].reports[].unreconstructedRuns",
      "[].reports[].wpePredictorNorm",
      "[].scope",
      "[].stage",
  };
  return paths;
}

const std::vector<std::pair<std::string, std::string>>& repair_apply_null_means() {
  static const std::vector<std::pair<std::string, std::string>> means = {
      {"[].changes[].changeLevelDb", "-Infinity"},
      {"[].reports[].detected.bandFloorDbfs[]", "-Infinity"},
      {"[].reports[].detected.floorDbfs", "-Infinity"},
      {"[].reports[].detected.harmonicDbfs[]", "-Infinity"},
  };
  return means;
}

}  // namespace sonare::mastering::assistant
