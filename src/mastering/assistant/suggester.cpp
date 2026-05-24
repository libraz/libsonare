/// @file suggester.cpp
/// @brief Rule-based mastering assistant chain suggestion implementation.

#include "mastering/assistant/suggester.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "mastering/api/presets.h"

namespace sonare::mastering::assistant {
namespace {

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

  set_loudness(result.config, config.target_lufs, config.ceiling_db);
  explain(result.explanation, "target loudness and ceiling applied from AssistantConfig");

  const bool dark = profile.spectral.centroid_hz > 0.0f && profile.spectral.centroid_hz < 1500.0f;
  const bool dull_air = profile.spectral.air_rms_db < profile.spectral.mid_rms_db - 18.0f;
  if (dark || dull_air) {
    result.config.spectral.air_band.enabled = true;
    result.config.spectral.air_band.config.amount = dark ? 0.55f : 0.35f;
    result.config.spectral.air_band.config.shelf_frequency_hz = 12000.0f;
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
    result.config.stereo.mono_maker.config.amount = 1.0f;
    explain(result.explanation, "speech profile enables de-esser and mono compatibility");
  }

  if (config.enable_repair) {
    result.config.repair.declick.enabled = true;
    if (profile.spectral.flatness > 0.35f) {
      result.config.repair.denoise.enabled = true;
    }
    explain(result.explanation, "repair stages enabled by AssistantConfig");
  }

  return result;
}

}  // namespace sonare::mastering::assistant
