/// @file suggester.cpp
/// @brief Rule-based mastering assistant chain suggestion implementation.

#include "mastering/assistant/suggester.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>

#include "mastering/api/presets.h"
#include "util/json_escape.h"

namespace sonare::mastering::assistant {
namespace {

// Shelf frequency used when enabling the "air" band on dark/dull material. This
// is the conventional high-shelf corner for adding presence/air to a master.
constexpr float kAirShelfFrequencyHz = 12000.0f;

std::string primary_genre(const AudioProfile& profile) {
  if (profile.genre_candidates.empty()) return "pop";
  return profile.genre_candidates.front().name;
}

api::Preset preset_for_genre(const std::string& genre) {
  if (genre == "edm") return api::Preset::EDM;
  if (genre == "hipHop") return api::Preset::HipHop;
  if (genre == "classical") return api::Preset::Classical;
  if (genre == "speech") return api::Preset::Speech;
  if (genre == "ambient") return api::Preset::Ambient;
  return api::Preset::Pop;
}

void set_loudness(api::MasteringChainConfig& chain, float target_lufs, float ceiling_db) {
  chain.maximizer.true_peak_limiter.enabled = true;
  chain.maximizer.true_peak_limiter.config.ceiling_db = ceiling_db;
  chain.loudness.enabled = true;
  chain.loudness.target_lufs = target_lufs;
  chain.loudness.ceiling_db = ceiling_db;
}

void explain(std::vector<std::string>& out, std::string text) { out.push_back(std::move(text)); }

void resolve_platform_loudness(const AssistantConfig& config, float* target_lufs,
                               float* ceiling_db) {
  *target_lufs = config.target_lufs;
  *ceiling_db = config.ceiling_db;

  const AssistantConfig defaults;
  const bool loudness_is_default = config.target_lufs == defaults.target_lufs;
  const bool ceiling_is_default = config.ceiling_db == defaults.ceiling_db;
  if (!loudness_is_default && !ceiling_is_default) {
    return;
  }

  if (config.target_platform == "broadcast") {
    if (loudness_is_default) *target_lufs = -23.0f;
    if (ceiling_is_default) *ceiling_db = -1.0f;
  } else if (config.target_platform == "podcast") {
    if (loudness_is_default) *target_lufs = -16.0f;
    if (ceiling_is_default) *ceiling_db = -1.0f;
  } else if (config.target_platform == "club" || config.target_platform == "cd") {
    if (loudness_is_default) *target_lufs = -9.0f;
    if (ceiling_is_default) *ceiling_db = -0.3f;
  }
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
  return suggest_chain(analyze_audio_profile(audio), config);
}

AssistantResult suggest_chain(const AudioProfile& profile, const AssistantConfig& config) {
  AssistantResult result;
  result.profile = profile;
  result.genre_candidates = profile.genre_candidates;

  const std::string genre = primary_genre(profile);
  result.config = api::preset_config(preset_for_genre(genre));
  explain(result.explanation, "base preset selected from top genre candidate: " + genre);

  float target_lufs = config.target_lufs;
  float ceiling_db = config.ceiling_db;
  resolve_platform_loudness(config, &target_lufs, &ceiling_db);
  set_loudness(result.config, target_lufs, ceiling_db);
  explain(result.explanation, "target loudness and ceiling applied from AssistantConfig");

  const bool dark = profile.spectral.centroid_hz > 0.0f && profile.spectral.centroid_hz < 1500.0f;
  const bool dull_air = profile.spectral.air_rms_db < profile.spectral.mid_rms_db - 18.0f;
  if (dark || dull_air) {
    result.config.spectral.air_band.enabled = true;
    result.config.spectral.air_band.config.amount = dark ? 0.55f : 0.35f;
    result.config.spectral.air_band.config.shelf_frequency_hz = kAirShelfFrequencyHz;
    explain(result.explanation, "air band enabled because the spectral profile is dark");
  }

  const bool dynamic =
      profile.loudness.lra_lu > 12.0f || profile.dynamics.short_term_lufs_std > 4.0f;
  if (dynamic) {
    result.config.dynamics.compressor.enabled = true;
    result.config.dynamics.compressor.config.threshold_db = -22.0f;
    result.config.dynamics.compressor.config.ratio =
        genre == "classical" ? 1.25f : (genre == "speech" ? 2.5f : 2.0f);
    result.config.dynamics.compressor.config.attack_ms = 15.0f;
    result.config.dynamics.compressor.config.release_ms = 120.0f;
    explain(result.explanation, "compressor adjusted because loudness range is high");
  }

  const bool bass_heavy = profile.spectral.low_rms_db > profile.spectral.mid_rms_db - 4.0f;
  if (bass_heavy && profile.bpm > 120.0f) {
    result.config.eq.tilt.enabled = true;
    result.config.eq.tilt.tilt_db = 0.4f;
    result.config.saturation.tape.enabled = true;
    result.config.saturation.tape.config.drive_db =
        std::min(2.5f, std::max(0.5f, profile.bpm / 70.0f));
    explain(result.explanation, "bass-heavy fast material gets mild tilt and tape drive");
  }

  if (profile.dynamics.attack_density > 2.0f && genre != "classical") {
    result.config.dynamics.transient_shaper.enabled = true;
    result.config.dynamics.transient_shaper.config.attack_gain_db = 1.5f;
    result.config.dynamics.transient_shaper.config.sustain_gain_db = -0.3f;
    explain(result.explanation, "transient shaper enabled for dense attacks");
  }

  if (genre == "speech") {
    result.config.dynamics.deesser.enabled = true;
    result.config.stereo.mono_maker.enabled = true;
    result.config.stereo.mono_maker.config.amount =
        std::clamp(config.speech_mono_amount, 0.0f, 1.0f);
    explain(result.explanation, "speech profile enables de-esser and mono compatibility");
  }

  if (config.enable_repair) {
    result.config.repair.declick.enabled = true;
    if (profile.spectral.flatness > 0.35f && !config.prefer_streaming_safe) {
      result.config.repair.denoise.enabled = true;
    }
    explain(result.explanation, config.prefer_streaming_safe
                                    ? "streaming-safe repair enabled by AssistantConfig"
                                    : "repair stages enabled by AssistantConfig");
  }

  return result;
}

std::string assistant_result_to_json(const AssistantResult& result) {
  std::ostringstream json;
  json << "{\"chainConfig\":" << api::chain_config_to_json(result.config) << ",\"explanation\":[";
  for (size_t index = 0; index < result.explanation.size(); ++index) {
    if (index > 0) json << ',';
    json << '"' << sonare::util::escape_json_string(result.explanation[index]) << '"';
  }
  json << "],\"genreCandidates\":[";
  for (size_t index = 0; index < result.genre_candidates.size(); ++index) {
    if (index > 0) json << ',';
    const auto& candidate = result.genre_candidates[index];
    json << "{\"name\":\"" << sonare::util::escape_json_string(candidate.name)
         << "\",\"score\":" << candidate.score << '}';
  }
  json << "],\"profile\":{\"durationSec\":" << result.profile.duration_sec
       << ",\"bpm\":" << result.profile.bpm
       << ",\"bpmConfidence\":" << result.profile.bpm_confidence
       << ",\"integratedLufs\":" << result.profile.loudness.integrated_lufs
       << ",\"lraLu\":" << result.profile.loudness.lra_lu
       << ",\"truePeakDb\":" << result.profile.loudness.true_peak_db
       << ",\"crestFactorDb\":" << result.profile.loudness.crest_factor_db
       << ",\"spectralCentroidHz\":" << result.profile.spectral.centroid_hz
       << ",\"spectralFlatness\":" << result.profile.spectral.flatness
       << ",\"attackDensity\":" << result.profile.dynamics.attack_density
       << ",\"sustainRatio\":" << result.profile.dynamics.sustain_ratio << "}}";
  return json.str();
}

}  // namespace sonare::mastering::assistant
