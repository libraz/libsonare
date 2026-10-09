#include "mastering/assistant/repair_session.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "mastering/api/param_field_tables.h"
#include "mastering/api/processor_params.h"
#include "mastering/assistant/audio_profile.h"
#include "mastering/assistant/suggester.h"
#include "mastering/common/loudness_measure.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "util/exception.h"
#include "util/json.h"

namespace sonare::mastering::assistant {
namespace {

namespace json = sonare::util::json;
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

/// Parses @p text, refusing malformed JSON and a repeated key as an invalid request.
json::Value parse_request(const std::string& text, const char* subject) {
  try {
    return json::parse_strict(text);
  } catch (const json::JsonError& e) {
    refuse(std::string(subject) + " is not valid JSON: " + e.what());
  }
}

void validate_channel_set(const float* const* channels, std::size_t channel_count,
                          std::size_t length, int sample_rate) {
  if (channels == nullptr || channel_count == 0) refuse("channels must not be empty");
  if (length == 0) refuse("audio must not be empty");
  if (sample_rate <= 0) refuse("sample_rate must be positive");
  for (std::size_t c = 0; c < channel_count; ++c) {
    if (channels[c] == nullptr) refuse("channel " + std::to_string(c) + " must not be null");
    validate_offline_audio_input(channels[c], length, sample_rate);
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

/// Reads one stage object through @p reader, the stage's catalog config builder. An empty build
/// first declares the keys and enum choices the builder reads, so an unknown key is refused by
/// name rather than ignored and a choice may be given by name.
template <typename Reader>
auto read_settings(const json::Object& object, Stage stage, Reader reader) {
  const std::string subject = std::string(stage_name(stage)) + ": ";
  ParamMap declared;
  (void)reader(declared);
  ParamMap map;
  for (const auto& [key, value] : object) {
    if (key == "stage") continue;
    if (declared.probed_keys().count(key) == 0) refuse(subject + "unknown setting '" + key + "'");
    if (value.is_bool()) {
      map[key] = value.as_bool() ? 1.0 : 0.0;
    } else if (value.is_number()) {
      map[key] = value.as_number();
    } else if (value.is_string()) {
      const auto choices = declared.probed_choices().find(key);
      if (choices == declared.probed_choices().end()) refuse(subject + key + " takes a number");
      const std::string& name = value.as_string();
      const auto choice =
          std::find_if(choices->second.begin(), choices->second.end(),
                       [&name](const api::detail::EnumChoice& c) { return c.name == name; });
      if (choice == choices->second.end()) {
        refuse(subject + key + " has no choice '" + name + "'");
      }
      map[key] = static_cast<double>(choice->value);
    } else {
      refuse(subject + key + " must be a number, a boolean or a choice name");
    }
  }
  return reader(map);
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
  const json::Value root = parse_request(stages_json.empty() ? "[]" : stages_json, "stages");
  if (!root.is_array()) refuse("stages must be an array");
  StagePlan plan;
  for (const json::Value& entry : root.as_array()) {
    if (!entry.is_object()) refuse("each stage must be an object");
    const json::Object& object = entry.as_object();
    const auto name = object.find("stage");
    if (name == object.end() || !name->second.is_string()) refuse("each stage must name its stage");
    const std::optional<Stage> stage = stage_from_name(name->second.as_string());
    if (!stage) {
      refuse("unknown stage '" + name->second.as_string() +
             "'; expected declip, declick, decrackle, dehum, denoise or dereverb");
    }
    if (plan.has(*stage)) refuse(std::string("stage '") + stage_name(*stage) + "' appears twice");
    switch (*stage) {
      case Stage::Declip:
        plan.declip = read_settings(object, *stage, api::detail::declip_config);
        break;
      case Stage::Declick:
        plan.declick = read_settings(object, *stage, api::detail::declick_config);
        break;
      case Stage::Decrackle:
        plan.decrackle = read_settings(object, *stage, api::detail::decrackle_config);
        break;
      case Stage::Dehum:
        plan.dehum = read_settings(object, *stage, api::detail::dehum_config);
        break;
      case Stage::Denoise:
        plan.denoise = read_settings(object, *stage, api::detail::denoise_classical_config);
        break;
      case Stage::Dereverb:
        plan.dereverb = read_settings(object, *stage, api::detail::dereverb_classical_config);
        break;
    }
  }
  return plan;
}

// ---------------------------------------------------------------------------
// Reports, in the field names every surface already uses for them
// ---------------------------------------------------------------------------

double count(std::size_t value) { return static_cast<double>(value); }

json::Array float_array(const float* values, std::size_t size) {
  json::Array out;
  out.reserve(size);
  for (std::size_t i = 0; i < size; ++i) out.emplace_back(values[i]);
  return out;
}

json::Object report_of(const repair::DeclipReport& report) {
  json::Object detected;
  json::put(detected, "sampleCount", count(report.detected.sample_count));
  json::put(detected, "sampleFraction", report.detected.sample_fraction);
  json::put(detected, "runCount", count(report.detected.run_count));
  json::put(detected, "longestRunSamples", count(report.detected.longest_run_samples));
  json::put(detected, "flatRunCount", count(report.detected.flat_run_count));
  json::put(detected, "longestFlatRunSamples", count(report.detected.longest_flat_run_samples));
  json::put(detected, "flatSampleCount", count(report.detected.flat_sample_count));
  json::put(detected, "flatLevel", report.detected.flat_level);
  json::Object out;
  json::put(out, "detected", std::move(detected));
  json::put(out, "lpcReconstructedRuns", count(report.lpc_reconstructed_runs));
  json::put(out, "interpolatedRuns", count(report.interpolated_runs));
  json::put(out, "repairedSamples", count(report.repaired_samples));
  json::put(out, "linkedRuns", count(report.linked_runs));
  return out;
}

json::Object report_of(const repair::DeclickReport& report) {
  json::Object detected;
  json::put(detected, "count", count(report.detected.count));
  json::put(detected, "rejected", count(report.detected.rejected));
  json::put(detected, "longestRunSamples", count(report.detected.longest_run_samples));
  json::put(detected, "perSecond", report.detected.per_second);
  json::Object out;
  json::put(out, "detected", std::move(detected));
  json::put(out, "repairedRuns", count(report.repaired_runs));
  json::put(out, "repairedSamples", count(report.repaired_samples));
  json::put(out, "linkedRuns", count(report.linked_runs));
  json::put(out, "lpcModelUsed", report.lpc_model_used);
  return out;
}

json::Object report_of(const repair::DecrackleReport& report) {
  json::Object detected;
  json::put(detected, "sampleCount", count(report.detected.sample_count));
  json::put(detected, "sampleFraction", report.detected.sample_fraction);
  json::put(detected, "perSecond", report.detected.per_second);
  json::Object out;
  json::put(out, "detected", std::move(detected));
  json::put(out, "replacedSamples", count(report.replaced_samples));
  json::put(out, "detailCoefficients", count(report.detail_coefficients));
  json::put(out, "shrunkCoefficients", count(report.shrunk_coefficients));
  json::put(out, "noiseSigma", report.noise_sigma);
  return out;
}

json::Object report_of(const repair::DehumReport& report) {
  json::Object detected;
  json::put(detected, "fundamentalHz", report.detected.fundamental_hz);
  json::put(detected, "fundamentalProminence", report.detected.fundamental_prominence);
  json::put(detected, "harmonics", report.detected.harmonics);
  json::put(detected, "harmonicDbfs",
            float_array(report.detected.harmonic_dbfs,
                        static_cast<std::size_t>(repair::kDehumMaxHarmonics)));
  json::Object out;
  json::put(out, "detected", std::move(detected));
  json::put(out, "notchedHarmonics", report.notched_harmonics);
  json::put(out, "appliedFundamentalHz", report.applied_fundamental_hz);
  json::put(out, "fundamentalDriftHz", report.fundamental_drift_hz);
  return out;
}

json::Object report_of(const repair::DenoiseReport& report) {
  json::Object detected;
  json::put(detected, "floorDbfs", report.detected.floor_dbfs);
  json::put(detected, "bandFloorDbfs",
            float_array(report.detected.band_floor_dbfs, repair::kRepairNoiseBandCount));
  json::Object out;
  json::put(out, "detected", std::move(detected));
  json::put(out, "meanReductionDb", report.mean_reduction_db);
  json::put(out, "maxReductionDb", report.max_reduction_db);
  json::put(out, "floorLimitedFraction", report.floor_limited_fraction);
  return out;
}

json::Object report_of(const repair::DereverbReport& report) {
  json::Object detected;
  json::put(detected, "lateDecayRatioDb", report.detected.late_decay_ratio_db);
  json::put(detected, "latePredictability", report.detected.late_predictability);
  json::Object out;
  json::put(out, "detected", std::move(detected));
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
  working = std::move(out);
  return entry;
}

// ---------------------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------------------

AssistantConfig read_analyze_request(const std::string& request_json) {
  AssistantConfig config;
  if (request_json.empty()) return config;
  const json::Value root = parse_request(request_json, "the analysis request");
  if (!root.is_object()) refuse("the analysis request must be an object");
  for (const auto& [key, value] : root.as_object()) {
    if (key != "preferStreamingSafe") refuse("unknown analysis option '" + key + "'");
    if (!value.is_bool()) refuse("preferStreamingSafe must be a boolean");
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
  validate_channel_set(channels, channel_count, length, sample_rate);
  const AssistantConfig config = read_analyze_request(request_json);

  const ChannelSetDefects defects =
      measure_defects_planar(channels, channel_count, length, sample_rate);
  std::vector<float> interleaved(length * channel_count);
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

  json::Array per_channel;
  for (const DefectProfile& channel : defects.channels) {
    per_channel.emplace_back(defects_to_json(channel));
  }
  json::Array reasons;
  for (const std::string& reason : explanation) reasons.emplace_back(reason);

  json::Object root;
  json::put(root, "defects", defects_to_json(defects.aggregate));
  json::put(root, "channels", std::move(per_channel));
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
  validate_channel_set(channels, channel_count, length, sample_rate);
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
      "channels[].clickCount",
      "channels[].clickLongestRunSamples",
      "channels[].clickPerSecond",
      "channels[].clickRejected",
      "channels[].clipFlatLevel",
      "channels[].clipFlatRunCount",
      "channels[].clipFlatSampleCount",
      "channels[].clipLongestFlatRunSamples",
      "channels[].clipLongestRunSamples",
      "channels[].clipRunCount",
      "channels[].clipSampleCount",
      "channels[].clipSampleFraction",
      "channels[].cracklePerSecond",
      "channels[].crackleSampleCount",
      "channels[].crackleSampleFraction",
      "channels[].humFundamentalDbfs",
      "channels[].humFundamentalHz",
      "channels[].humFundamentalProminence",
      "channels[].humHarmonics",
      "channels[].humPeakHarmonicDbfs",
      "channels[].lateDecayRatioDb",
      "channels[].measured",
      "channels[].noiseBandPeakDbfs",
      "channels[].noiseBandPeakIndex",
      "channels[].noiseFloorDbfs",
      "declipThresholdSafe",
      "defects",
      "defects.clickCount",
      "defects.clickLongestRunSamples",
      "defects.clickPerSecond",
      "defects.clickRejected",
      "defects.clipFlatLevel",
      "defects.clipFlatRunCount",
      "defects.clipFlatSampleCount",
      "defects.clipLongestFlatRunSamples",
      "defects.clipLongestRunSamples",
      "defects.clipRunCount",
      "defects.clipSampleCount",
      "defects.clipSampleFraction",
      "defects.cracklePerSecond",
      "defects.crackleSampleCount",
      "defects.crackleSampleFraction",
      "defects.humFundamentalDbfs",
      "defects.humFundamentalHz",
      "defects.humFundamentalProminence",
      "defects.humHarmonics",
      "defects.humPeakHarmonicDbfs",
      "defects.lateDecayRatioDb",
      "defects.measured",
      "defects.noiseBandPeakDbfs",
      "defects.noiseBandPeakIndex",
      "defects.noiseFloorDbfs",
      "explanation",
      "integratedLufs",
      "recommended",
      "recommended[].stage",
  };
  return paths;
}

}  // namespace sonare::mastering::assistant
