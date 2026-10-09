/// @file suggester.cpp
/// @brief Rule-based mastering assistant chain suggestion implementation.

#include "mastering/assistant/suggester.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "mastering/api/presets.h"
#include "mastering/assistant/platform_targets.h"
#include "util/exception.h"
#include "util/json.h"

namespace sonare::mastering::assistant {
namespace {

void explain(std::vector<std::string>& out, std::string text) { out.push_back(std::move(text)); }

// Each threshold is the highest value reached by any corpus draw WITHOUT that
// defect, so neither rule fires on clean material. Recall is partial and that is
// the intended trade: a false alarm processes a clean recording, a miss leaves
// it alone.
constexpr float kNoiseFloorOverProgrammeDb = -12.0f;  // loudest clean draw: -12.34
constexpr float kHumProminence = 10.0f;               // most prominent non-hum draw: 4.14
constexpr float kMainsToleranceHz = 0.25f;

/// The profiling the suggestion needs, which is more than the profiling a caller
/// asking only for a chain needs: the six detectors run only when a repair
/// decision will read them.
AudioProfileConfig profile_config_for(const AssistantConfig& config) {
  AudioProfileConfig profile_config;
  profile_config.detect_defects = config.enable_repair;
  return profile_config;
}

/// Whether the measured series is mains hum rather than programme content.
/// @details Prominence alone cannot tell them apart: a sustained bass note is a
///   prominent low peak too, and clean material reaches a prominence of 4 on
///   this corpus. Real hum sits on the mains frequency to well under a hertz,
///   so the frequency is what decides and the prominence only sizes it.
bool hum_is_mains(const DefectProfile& defects) {
  const float distance = std::min(std::abs(defects.hum_fundamental_hz - 50.0f),
                                  std::abs(defects.hum_fundamental_hz - 60.0f));
  return distance <= kMainsToleranceHz && defects.hum_fundamental_prominence > kHumProminence;
}

}  // namespace

/// Turns on the repair stages the measurement supports, and only those.
/// @details Three stages are selected on a count the detector either found or
///   did not; two more are selected against a threshold measured on real
///   material. Two of the five also take a parameter from the measurement rather
///   than the default, because for those the default would select the stage and
///   then give it nothing to act on. Dereverb is absent on purpose: its statistic reads *higher* on
///   a sustaining dry signal than on a short reverberant one, so no threshold over it separates the
///   two, and it stays under caller control until one does. Declip is withheld when the shared
///   threshold would reach audio louder than the pinned level.
void select_repair_stages(const AudioProfile& profile, const AssistantConfig& config,
                          api::MasteringChainConfig& out, std::vector<std::string>& explanation) {
  const DefectProfile& defects = profile.defects;
  if (!defects.measured) {
    explain(explanation,
            "repair requested but nothing measured the recording, so no repair "
            "stage was selected");
    return;
  }

  if (defects.clip_flat_run_count > 0 && defects.declip_threshold_safe) {
    out.repair.declip.enabled = true;
    // The measured plateau, not the default ceiling. Material clipped before it
    // was attenuated has no sample left at the default, so enabling the stage
    // without moving the threshold reconstructs nothing while the explanation
    // says it repaired. Clamped because float audio may sit over unity.
    out.repair.declip.config.clip_threshold = std::min(defects.clip_flat_level, 1.0f);
    explain(explanation, "declip: runs of samples sit pinned at one level");
  } else if (defects.clip_flat_run_count > 0) {
    explain(explanation,
            "declip withheld: audio louder than the pinned level reaches the threshold, "
            "so repairing at it would rewrite unclipped audio");
  }
  if (defects.click_count > 0) {
    out.repair.declick.enabled = true;
    explain(explanation, "declick: impulsive runs stand out from their neighbours");
  }
  if (defects.crackle_sample_count > 0) {
    out.repair.decrackle.enabled = true;
    explain(explanation, "decrackle: samples depart from the local median");
  }
  if (hum_is_mains(defects)) {
    out.repair.dehum.enabled = true;
    // The tracked frequency, not the default: the detector searched both mains
    // frequencies and this is the one it found.
    out.repair.dehum.config.fundamental_hz = defects.hum_fundamental_hz;
    explain(explanation, "dehum: a prominent harmonic series sits on a mains frequency");
  }
  // Referred to the programme rather than to full scale, so a quiet recording
  // with an inaudible floor is not treated like a loud one with the same floor.
  const float floor_over_programme = defects.noise_floor_dbfs - profile.loudness.integrated_lufs;
  if (std::isfinite(defects.noise_floor_dbfs) && std::isfinite(profile.loudness.integrated_lufs) &&
      floor_over_programme > kNoiseFloorOverProgrammeDb) {
    out.repair.denoise.enabled = true;
    if (config.prefer_streaming_safe) {
      // The default estimator ranks every frame of the whole signal by energy, so
      // it is the one thing in this stage a stream cannot have. The tracker
      // estimators are recursive in time; speech-presence probability is the one
      // that carries neither a minimum window nor its bias compensation.
      out.repair.denoise.config.noise_estimator = repair::DenoiseNoiseEstimator::Spp;
      explain(explanation,
              "denoise: the noise floor is loud under the programme, tracking it "
              "frame by frame because streaming-safe was asked for");
    } else {
      explain(explanation, "denoise: the noise floor is loud under the programme");
    }
  }
}

namespace {

void resolve_platform_loudness(const AssistantConfig& config, float* target_lufs,
                               float* ceiling_db) {
  *target_lufs = config.target_lufs;
  *ceiling_db = config.ceiling_db;

  const AssistantConfig defaults;
  // "The caller did not ask for one" is a presence question, not a value
  // question: comparing against the default made an explicit -14 -- the very
  // value the default happens to hold -- indistinguishable from silence, so a
  // host UI parked at its default slider position lost that one position to the
  // platform target while every other position won.
  const bool loudness_is_default =
      !config.target_lufs_explicit && config.target_lufs == defaults.target_lufs;
  const bool ceiling_is_default =
      !config.ceiling_db_explicit && config.ceiling_db == defaults.ceiling_db;
  if (!loudness_is_default && !ceiling_is_default) {
    return;
  }

  // The delivery-target vocabulary and its loudness live in one table
  // (platform_targets.h) that every surface resolves against. A target that
  // adds nothing to the caller's request is a row with no override, not an
  // absent name.
  const PlatformTarget* target = platform_target_from_name(config.target_platform.c_str());
  if (target == nullptr || !target->overrides_loudness) return;
  if (loudness_is_default) *target_lufs = target->target_lufs;
  if (ceiling_is_default) *ceiling_db = target->ceiling_db;
}

void validate_suggestion_config(const AssistantConfig& config) {
  SONARE_CHECK_MSG(std::isfinite(config.target_lufs), ErrorCode::InvalidParameter,
                   "assistant target_lufs must be finite");
  SONARE_CHECK_MSG(std::isfinite(config.ceiling_db), ErrorCode::InvalidParameter,
                   "assistant ceiling_db must be finite");
  SONARE_CHECK_MSG(std::isfinite(config.speech_mono_amount), ErrorCode::InvalidParameter,
                   "assistant speech_mono_amount must be finite");
  SONARE_CHECK_MSG(platform_target_from_name(config.target_platform.c_str()) != nullptr,
                   ErrorCode::InvalidParameter,
                   "unknown mastering target platform '" + config.target_platform +
                       "'; expected one of: " + platform_names_joined());
  SONARE_CHECK_MSG(std::string(api::preset_to_string(config.preset)) != "unknown",
                   ErrorCode::InvalidParameter, "unknown assistant preset");
}

}  // namespace

AssistantResult suggest_chain(const float* samples, std::size_t length, int sample_rate,
                              const AssistantConfig& config) {
  if (samples == nullptr || length == 0 || sample_rate <= 0) {
    return suggest_chain(AudioProfile{}, config);
  }
  return suggest_chain(Audio::from_buffer(samples, length, sample_rate), config);
}

AssistantResult suggest_chain(const Audio& audio, const AssistantConfig& config) {
  return suggest_chain(analyze_audio_profile(audio, profile_config_for(config)), config);
}

AssistantResult suggest_chain_interleaved(const float* samples, std::size_t frames, int channels,
                                          int sample_rate, const AssistantConfig& config) {
  if (samples == nullptr || frames == 0 || channels <= 0 || sample_rate <= 0) {
    return suggest_chain(AudioProfile{}, config);
  }
  return suggest_chain(analyze_audio_profile_interleaved(samples, frames, channels, sample_rate,
                                                         profile_config_for(config)),
                       config);
}

AssistantResult suggest_chain(const AudioProfile& profile, const AssistantConfig& config) {
  validate_suggestion_config(config);
  AssistantResult result;
  result.profile = profile;
  SONARE_CHECK_MSG(api::preset_kind(config.preset) == api::PresetKind::Mastering,
                   ErrorCode::InvalidParameter,
                   std::string("assistant preset must be a mastering preset, not '") +
                       api::preset_to_string(config.preset) + "'");
  result.config = api::preset_config(config.preset);
  explain(result.explanation, std::string("base preset: ") + api::preset_to_string(config.preset));

  float target_lufs = config.target_lufs;
  float ceiling_db = config.ceiling_db;
  resolve_platform_loudness(config, &target_lufs, &ceiling_db);
  api::enable_loudness(result.config, target_lufs, ceiling_db);
  explain(result.explanation, "target loudness and ceiling applied from AssistantConfig");

  if (config.preset == api::Preset::Speech) {
    result.config.dynamics.deesser.enabled = true;
    result.config.stereo.mono_maker.enabled = true;
    result.config.stereo.mono_maker.config.amount =
        std::clamp(config.speech_mono_amount, 0.0f, 1.0f);
    explain(result.explanation, "speech preset enables de-esser and mono compatibility");
  }

  if (config.enable_repair) {
    select_repair_stages(profile, config, result.config, result.explanation);
  }

  return result;
}

std::string assistant_result_to_json(const AssistantResult& result) {
  namespace json = sonare::util::json;

  // Parse the chain-config JSON back into a tree so it nests as a real object
  // (instead of an opaque string). chain_config_to_json itself uses util::json
  // so the round-trip is lossless and locale-safe.
  json::Value chain_config = json::parse(api::chain_config_to_json(result.config));

  json::Array explanation;
  explanation.reserve(result.explanation.size());
  for (const auto& line : result.explanation) {
    explanation.emplace_back(json::Value(line));
  }

  // Flat "profile" object: mirrors the previous schema (no nested loudness /
  // spectral / dynamics groups — that nesting only exists in audio_profile_to_json).
  json::Object profile;
  util::json::put(profile, "durationSec", result.profile.duration_sec);
  util::json::put(profile, "bpm", result.profile.bpm);
  util::json::put(profile, "bpmConfidence", result.profile.bpm_confidence);
  util::json::put(profile, "integratedLufs", result.profile.loudness.integrated_lufs);
  util::json::put(profile, "lraLu", result.profile.loudness.lra_lu);
  util::json::put(profile, "truePeakDb", result.profile.loudness.true_peak_db);
  util::json::put(profile, "crestFactorDb", result.profile.loudness.crest_factor_db);
  util::json::put(profile, "spectralCentroidHz", result.profile.spectral.centroid_hz);
  util::json::put(profile, "spectralFlatness", result.profile.spectral.flatness);
  util::json::put(profile, "attackDensity", result.profile.dynamics.attack_density);
  util::json::put(profile, "sustainRatio", result.profile.dynamics.sustain_ratio);

  json::Object root;
  util::json::put(root, "chainConfig", std::move(chain_config));
  util::json::put(root, "explanation", std::move(explanation));
  util::json::put(root, "profile", std::move(profile));
  return json::dump(json::Value(std::move(root)));
}

const std::vector<std::string>& assistant_result_schema_paths() {
  static const std::vector<std::string> paths = {
      "chainConfig",
      "chainConfig.params",
      "chainConfig.version",
      "explanation",
      "profile",
      "profile.attackDensity",
      "profile.bpm",
      "profile.bpmConfidence",
      "profile.crestFactorDb",
      "profile.durationSec",
      "profile.integratedLufs",
      "profile.lraLu",
      "profile.spectralCentroidHz",
      "profile.spectralFlatness",
      "profile.sustainRatio",
      "profile.truePeakDb",
  };
  return paths;
}

}  // namespace sonare::mastering::assistant
