/// @file suggester.cpp
/// @brief Rule-based mastering assistant chain suggestion implementation.

#include "mastering/assistant/suggester.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "mastering/api/presets.h"
#include "mastering/assistant/platform_targets.h"
#include "util/json.h"

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
  // Every genre label produced by infer_genres() maps to a distinct catalogue
  // preset; the genre label is the camelCase preset identifier wherever a 1:1
  // preset exists. Genres that share a preset (no dedicated voicing) fall
  // through to the closest catalogue entry. Any preset reachable here is also
  // reachable as a candidate from infer_genres(), so the suggester can in
  // principle recommend the full catalogue rather than collapsing to Pop.
  if (genre == "pop") return api::Preset::Pop;
  if (genre == "edm") return api::Preset::EDM;
  if (genre == "acoustic") return api::Preset::Acoustic;
  if (genre == "hipHop") return api::Preset::HipHop;
  if (genre == "aiMusic") return api::Preset::AIMusic;
  if (genre == "speech") return api::Preset::Speech;
  if (genre == "ambient") return api::Preset::Ambient;
  if (genre == "lofi") return api::Preset::Lofi;
  if (genre == "classical") return api::Preset::Classical;
  if (genre == "drumAndBass") return api::Preset::DrumAndBass;
  if (genre == "techno") return api::Preset::Techno;
  if (genre == "metal") return api::Preset::Metal;
  if (genre == "trap") return api::Preset::Trap;
  if (genre == "rnb") return api::Preset::RnB;
  if (genre == "jazz") return api::Preset::Jazz;
  if (genre == "jpop") return api::Preset::JPop;
  if (genre == "kpop") return api::Preset::KPop;
  if (genre == "trance") return api::Preset::Trance;
  if (genre == "gameOst") return api::Preset::GameOst;
  // Platform / delivery targets are reachable via AssistantConfig.target_platform
  // rather than genre inference, but accept their identifiers too so a caller
  // who injects them as a candidate gets the matching preset.
  if (genre == "streaming") return api::Preset::Streaming;
  if (genre == "youtube") return api::Preset::YouTube;
  if (genre == "broadcast") return api::Preset::Broadcast;
  if (genre == "podcast") return api::Preset::Podcast;
  if (genre == "audiobook") return api::Preset::Audiobook;
  if (genre == "cinema") return api::Preset::Cinema;
  return api::Preset::Pop;
}

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

/// Turns on the repair stages the measurement supports, and only those.
/// @details Three stages are selected on a count the detector either found or
///   did not; two more are selected against a threshold measured on real
///   material. Dereverb is absent on purpose: its statistic reads *higher* on a
///   sustaining dry signal than on a short reverberant one, so no threshold over
///   it separates the two, and it stays under caller control until one does.
///   The noise estimator is left at its default, which outperforms the adaptive
///   trackers on every kind of material measured so far.
void select_repair_stages(const AudioProfile& profile, const AssistantConfig& config,
                          api::MasteringChainConfig& out, std::vector<std::string>& explanation) {
  const DefectProfile& defects = profile.defects;
  if (!defects.measured) {
    explain(explanation,
            "repair requested but nothing measured the recording, so no repair "
            "stage was selected");
    return;
  }

  if (defects.clip_sample_count > 0) {
    out.repair.declip.enabled = true;
    explain(explanation, "declip: samples reach the clipping threshold");
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
  if (floor_over_programme > kNoiseFloorOverProgrammeDb) {
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
  AssistantResult result;
  result.profile = profile;
  result.genre_candidates = profile.genre_candidates;

  const std::string genre = primary_genre(profile);
  result.config = api::preset_config(preset_for_genre(genre));
  explain(result.explanation, "base preset selected from top genre candidate: " + genre);

  float target_lufs = config.target_lufs;
  float ceiling_db = config.ceiling_db;
  resolve_platform_loudness(config, &target_lufs, &ceiling_db);
  api::enable_loudness(result.config, target_lufs, ceiling_db);
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

  json::Array genre_candidates;
  genre_candidates.reserve(result.genre_candidates.size());
  for (const auto& candidate : result.genre_candidates) {
    json::Object entry;
    entry.emplace("name", json::Value(candidate.name));
    entry.emplace("score", json::Value(candidate.score));
    genre_candidates.emplace_back(json::Value(std::move(entry)));
  }

  // Flat "profile" object: mirrors the previous schema (no nested loudness /
  // spectral / dynamics groups — that nesting only exists in audio_profile_to_json).
  json::Object profile;
  profile.emplace("durationSec", json::Value(result.profile.duration_sec));
  profile.emplace("bpm", json::Value(result.profile.bpm));
  profile.emplace("bpmConfidence", json::Value(result.profile.bpm_confidence));
  profile.emplace("integratedLufs", json::Value(result.profile.loudness.integrated_lufs));
  profile.emplace("lraLu", json::Value(result.profile.loudness.lra_lu));
  profile.emplace("truePeakDb", json::Value(result.profile.loudness.true_peak_db));
  profile.emplace("crestFactorDb", json::Value(result.profile.loudness.crest_factor_db));
  profile.emplace("spectralCentroidHz", json::Value(result.profile.spectral.centroid_hz));
  profile.emplace("spectralFlatness", json::Value(result.profile.spectral.flatness));
  profile.emplace("attackDensity", json::Value(result.profile.dynamics.attack_density));
  profile.emplace("sustainRatio", json::Value(result.profile.dynamics.sustain_ratio));

  json::Object root;
  root.emplace("chainConfig", std::move(chain_config));
  root.emplace("explanation", json::Value(std::move(explanation)));
  root.emplace("genreCandidates", json::Value(std::move(genre_candidates)));
  root.emplace("profile", json::Value(std::move(profile)));
  return json::dump(json::Value(std::move(root)));
}

}  // namespace sonare::mastering::assistant
