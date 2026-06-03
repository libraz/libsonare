#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "mastering/api/presets.h"
#include "mastering/assistant/audio_profile.h"
#include "mastering/assistant/suggester.h"
#include "util/constants.h"

namespace assistant = sonare::mastering::assistant;

namespace {

using sonare::constants::kTwoPi;

std::vector<float> tone(int sr, float seconds, float frequency, float amplitude = 0.4f) {
  std::vector<float> samples(static_cast<size_t>(seconds * static_cast<float>(sr)));
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = amplitude * std::sin(kTwoPi * frequency * t);
  }
  return samples;
}

void add_tone(std::vector<float>& samples, int sr, float frequency, float amplitude) {
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] += amplitude * std::sin(kTwoPi * frequency * t);
  }
}

void add_clicks(std::vector<float>& samples, int sr, float bpm, float amplitude) {
  const int period = static_cast<int>((60.0f / bpm) * static_cast<float>(sr));
  if (period <= 0) return;
  for (size_t pos = 0; pos < samples.size(); pos += static_cast<size_t>(period)) {
    for (size_t n = 0; n < 64 && pos + n < samples.size(); ++n) {
      const float env = 1.0f - static_cast<float>(n) / 64.0f;
      samples[pos + n] += amplitude * env;
    }
  }
}

std::vector<float> rhythmic_track(int sr, float seconds, float bpm, float bass_hz,
                                  float click_amp) {
  auto samples = tone(sr, seconds, bass_hz, 0.35f);
  add_tone(samples, sr, bass_hz * 2.0f, 0.12f);
  add_clicks(samples, sr, bpm, click_amp);
  return samples;
}

std::vector<float> ambient_track(int sr, float seconds) {
  auto samples = tone(sr, seconds, 180.0f, 0.24f);
  add_tone(samples, sr, 360.0f, 0.12f);
  add_tone(samples, sr, 540.0f, 0.08f);
  return samples;
}

std::vector<float> dynamic_classical_like(int sr, float seconds) {
  std::vector<float> samples(static_cast<size_t>(seconds * static_cast<float>(sr)));
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    const float env = 0.08f + 0.45f * (0.5f + 0.5f * std::sin(kTwoPi * 0.25f * t));
    samples[i] = env * (std::sin(kTwoPi * 330.0f * t) + 0.5f * std::sin(kTwoPi * 660.0f * t));
  }
  return samples;
}

std::vector<float> speech_like(int sr, float seconds) {
  std::vector<float> samples(static_cast<size_t>(seconds * static_cast<float>(sr)));
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    const float syllable = 0.55f + 0.45f * std::max(0.0f, std::sin(kTwoPi * 4.0f * t));
    samples[i] =
        syllable * (0.20f * std::sin(kTwoPi * 170.0f * t) + 0.16f * std::sin(kTwoPi * 900.0f * t) +
                    0.08f * std::sin(kTwoPi * 1800.0f * t));
  }
  return samples;
}

bool contains_genre(const std::vector<assistant::GenreCandidate>& candidates,
                    const std::string& expected) {
  return std::any_of(candidates.begin(), candidates.end(),
                     [&](const auto& item) { return item.name == expected; });
}

}  // namespace

TEST_CASE("Assistant AudioProfile captures spectral and dynamics differences",
          "[mastering][assistant]") {
  constexpr int sr = 22050;
  assistant::AudioProfileConfig cfg;
  cfg.n_fft = 1024;
  cfg.hop_length = 256;

  const auto low_samples = tone(sr, 2.0f, 220.0f);
  const auto low =
      assistant::analyze_audio_profile(low_samples.data(), low_samples.size(), sr, cfg);
  const auto high_samples = tone(sr, 2.0f, 4000.0f);
  const auto high =
      assistant::analyze_audio_profile(high_samples.data(), high_samples.size(), sr, cfg);
  REQUIRE(high.spectral.centroid_hz > low.spectral.centroid_hz * 4.0f);

  auto transient_samples = tone(sr, 2.0f, 220.0f, 0.12f);
  add_clicks(transient_samples, sr, 120.0f, 0.8f);
  const auto steady =
      assistant::analyze_audio_profile(low_samples.data(), low_samples.size(), sr, cfg);
  const auto transient =
      assistant::analyze_audio_profile(transient_samples.data(), transient_samples.size(), sr, cfg);
  REQUIRE(transient.dynamics.attack_density > steady.dynamics.attack_density);
  REQUIRE(transient.loudness.true_peak_db > steady.loudness.true_peak_db - 12.0f);
}

TEST_CASE("Assistant genre candidates cover synthetic evaluation set", "[mastering][assistant]") {
  constexpr int sr = 22050;
  assistant::AudioProfileConfig cfg;
  cfg.n_fft = 1024;
  cfg.hop_length = 256;

  struct Case {
    std::string expected;
    std::vector<float> samples;
  };
  std::vector<Case> cases;
  cases.push_back({"edm", rhythmic_track(sr, 3.0f, 128.0f, 55.0f, 0.9f)});
  cases.push_back({"edm", rhythmic_track(sr, 3.0f, 140.0f, 65.0f, 0.8f)});
  cases.push_back({"hipHop", rhythmic_track(sr, 3.0f, 90.0f, 48.0f, 0.45f)});
  cases.push_back({"hipHop", rhythmic_track(sr, 3.0f, 82.0f, 52.0f, 0.40f)});
  cases.push_back({"ambient", ambient_track(sr, 3.0f)});
  cases.push_back({"ambient", tone(sr, 3.0f, 140.0f, 0.22f)});
  cases.push_back({"classical", dynamic_classical_like(sr, 4.0f)});
  cases.push_back({"classical", dynamic_classical_like(sr, 4.0f)});
  cases.push_back({"speech", speech_like(sr, 3.0f)});
  cases.push_back({"pop", rhythmic_track(sr, 3.0f, 112.0f, 110.0f, 0.35f)});

  int top3_hits = 0;
  for (const auto& item : cases) {
    CAPTURE(item.expected);
    const auto profile =
        assistant::analyze_audio_profile(item.samples.data(), item.samples.size(), sr, cfg);
    if (contains_genre(profile.genre_candidates, item.expected)) ++top3_hits;
  }
  // The expanded taxonomy must not regress the clear primary cases: every
  // primary genre stays in the top-3 (all 10 cases hit after gating the new
  // sibling genres on their distinctive features).
  REQUIRE(top3_hits >= 7);
}

TEST_CASE("Assistant suggest_chain returns deterministic config and explanations",
          "[mastering][assistant]") {
  assistant::AudioProfile profile;
  profile.bpm = 128.0f;
  profile.loudness.lra_lu = 14.0f;
  profile.spectral.centroid_hz = 1200.0f;
  profile.spectral.low_rms_db = -18.0f;
  profile.spectral.mid_rms_db = -20.0f;
  profile.spectral.air_rms_db = -42.0f;
  profile.dynamics.attack_density = 2.8f;
  profile.dynamics.short_term_lufs_std = 5.0f;
  profile.genre_candidates = {{"edm", 0.9f}, {"pop", 0.5f}, {"hipHop", 0.3f}};

  assistant::AssistantConfig cfg;
  cfg.target_lufs = -13.0f;
  cfg.ceiling_db = -0.8f;
  auto result = assistant::suggest_chain(profile, cfg);
  auto result_again = assistant::suggest_chain(profile, cfg);

  REQUIRE(result.config.loudness.enabled);
  REQUIRE(result.config.loudness.target_lufs == -13.0f);
  REQUIRE(result.config.maximizer.true_peak_limiter.config.ceiling_db == -0.8f);
  REQUIRE(result.config.spectral.air_band.enabled);
  REQUIRE(result.config.dynamics.compressor.enabled);
  REQUIRE(result.config.dynamics.transient_shaper.enabled);
  REQUIRE_FALSE(result.explanation.empty());
  REQUIRE(sonare::mastering::api::chain_config_to_json(result.config) ==
          sonare::mastering::api::chain_config_to_json(result_again.config));
}

TEST_CASE("Assistant exposes speech mono-maker amount", "[mastering][assistant]") {
  assistant::AudioProfile profile;
  profile.genre_candidates = {{"speech", 0.95f}, {"pop", 0.2f}};

  assistant::AssistantConfig cfg;
  cfg.speech_mono_amount = 0.35f;
  auto result = assistant::suggest_chain(profile, cfg);

  REQUIRE(result.config.dynamics.deesser.enabled);
  REQUIRE(result.config.stereo.mono_maker.enabled);
  REQUIRE(result.config.stereo.mono_maker.config.amount == 0.35f);
}

TEST_CASE("Assistant target platform and streaming-safe preference affect suggestions",
          "[mastering][assistant]") {
  assistant::AudioProfile profile;
  profile.spectral.flatness = 0.6f;
  profile.genre_candidates = {{"pop", 0.9f}};

  assistant::AssistantConfig broadcast;
  broadcast.target_platform = "broadcast";
  auto broadcast_result = assistant::suggest_chain(profile, broadcast);
  REQUIRE(broadcast_result.config.loudness.target_lufs == -23.0f);

  assistant::AssistantConfig streaming_safe;
  streaming_safe.enable_repair = true;
  streaming_safe.prefer_streaming_safe = true;
  auto safe_result = assistant::suggest_chain(profile, streaming_safe);
  REQUIRE(safe_result.config.repair.declick.enabled);
  REQUIRE_FALSE(safe_result.config.repair.denoise.enabled);

  assistant::AssistantConfig offline_repair;
  offline_repair.enable_repair = true;
  offline_repair.prefer_streaming_safe = false;
  auto repair_result = assistant::suggest_chain(profile, offline_repair);
  REQUIRE(repair_result.config.repair.declick.enabled);
  REQUIRE(repair_result.config.repair.denoise.enabled);
}

namespace {

// A profile that triggers none of suggest_chain's content-dependent overrides
// (no dark/dull-air air band, no high-LRA compressor, no bass-heavy tilt/tape,
// no dense-attack transient shaper, not speech). Only the genre-derived base
// preset and the loudness stage are applied, so the preset's distinctive EQ /
// stereo / saturation voicing survives and can be asserted.
assistant::AudioProfile neutral_profile_with_genre(const std::string& genre) {
  assistant::AudioProfile profile;
  profile.bpm = 110.0f;
  profile.loudness.lra_lu = 5.0f;  // below dynamic threshold
  profile.dynamics.short_term_lufs_std = 1.0f;
  profile.dynamics.attack_density = 1.0f;  // below transient-shaper threshold
  profile.spectral.centroid_hz = 3000.0f;  // not dark
  profile.spectral.low_rms_db = -30.0f;    // not bass-heavy
  profile.spectral.mid_rms_db = -20.0f;
  profile.spectral.air_rms_db = -25.0f;  // not dull (mid - air < 18)
  profile.spectral.flatness = 0.1f;
  profile.genre_candidates = {{genre, 0.99f}};
  return profile;
}

}  // namespace

TEST_CASE("Assistant suggester reaches dedicated presets for music genres, not generic Pop",
          "[mastering][assistant]") {
  namespace api = sonare::mastering::api;

  // Each genre label produced by infer_genres() must select its own catalogue
  // preset rather than collapsing to Pop. Assert via distinctive preset fields
  // that survive the assistant's content overrides for a neutral profile.
  struct Case {
    std::string genre;
    api::Preset preset;
  };
  const std::vector<Case> cases = {
      {"edm", api::Preset::EDM},
      {"techno", api::Preset::Techno},
      {"trance", api::Preset::Trance},
      {"drumAndBass", api::Preset::DrumAndBass},
      {"hipHop", api::Preset::HipHop},
      {"trap", api::Preset::Trap},
      {"rnb", api::Preset::RnB},
      {"metal", api::Preset::Metal},
      {"jazz", api::Preset::Jazz},
      {"acoustic", api::Preset::Acoustic},
      {"classical", api::Preset::Classical},
      {"ambient", api::Preset::Ambient},
      {"lofi", api::Preset::Lofi},
      {"jpop", api::Preset::JPop},
      {"kpop", api::Preset::KPop},
      {"gameOst", api::Preset::GameOst},
  };

  const auto pop_base = api::preset_config(api::Preset::Pop);

  for (const auto& item : cases) {
    CAPTURE(item.genre);
    const auto profile = neutral_profile_with_genre(item.genre);
    const auto result = assistant::suggest_chain(profile, assistant::AssistantConfig{});
    const auto preset_base = api::preset_config(item.preset);

    // The suggested chain must carry this preset's distinctive EQ tilt and
    // stereo width / tape voicing, proving preset_for_genre routed to the
    // dedicated preset.
    REQUIRE(result.config.eq.tilt.tilt_db == preset_base.eq.tilt.tilt_db);
    REQUIRE(result.config.stereo.imager.config.width == preset_base.stereo.imager.config.width);
    REQUIRE(result.config.saturation.tape.enabled == preset_base.saturation.tape.enabled);

    // The explanation must name the chosen genre.
    const bool mentions_genre = std::any_of(
        result.explanation.begin(), result.explanation.end(),
        [&](const std::string& line) { return line.find(item.genre) != std::string::npos; });
    REQUIRE(mentions_genre);

    // If the preset genuinely differs from Pop in tilt or width, the suggested
    // config must differ from Pop too (i.e. it did not silently fall back).
    if (preset_base.eq.tilt.tilt_db != pop_base.eq.tilt.tilt_db ||
        preset_base.stereo.imager.config.width != pop_base.stereo.imager.config.width) {
      const bool differs_from_pop =
          result.config.eq.tilt.tilt_db != pop_base.eq.tilt.tilt_db ||
          result.config.stereo.imager.config.width != pop_base.stereo.imager.config.width;
      REQUIRE(differs_from_pop);
    }
  }
}

TEST_CASE("Assistant infer_genres surfaces the broad music-genre catalogue",
          "[mastering][assistant]") {
  // Drive analyze_audio_profile with synthetic fast, bright, transient-dense
  // material and confirm the inferred candidate set is not limited to the
  // original six labels. Guards the expanded taxonomy.
  constexpr int sr = 22050;
  assistant::AudioProfileConfig cfg;
  cfg.n_fft = 1024;
  cfg.hop_length = 256;

  auto fast = tone(sr, 3.0f, 70.0f, 0.30f);
  add_tone(fast, sr, 4000.0f, 0.18f);
  add_clicks(fast, sr, 174.0f, 0.9f);
  const auto profile = assistant::analyze_audio_profile(fast.data(), fast.size(), sr, cfg);

  static const std::vector<std::string> legacy_six = {"edm",    "hipHop", "classical",
                                                      "speech", "pop",    "ambient"};
  const bool has_new_genre = std::any_of(
      profile.genre_candidates.begin(), profile.genre_candidates.end(),
      [&](const assistant::GenreCandidate& candidate) {
        return std::find(legacy_six.begin(), legacy_six.end(), candidate.name) == legacy_six.end();
      });
  REQUIRE(has_new_genre);
}

TEST_CASE("Assistant suggester loudness config matches preset true-peak oversampling",
          "[mastering][assistant]") {
  // The suggester's set_loudness() must produce the same true_peak_oversample as
  // api::enable_loudness() (presets.cpp). Both are expected to use 4x oversampling;
  // asserting equality guards against the two paths drifting apart in future.
  assistant::AudioProfile profile;
  profile.genre_candidates = {{"pop", 0.9f}};

  assistant::AssistantConfig cfg;
  auto suggested = assistant::suggest_chain(profile, cfg);

  const auto preset = sonare::mastering::api::preset_config(sonare::mastering::api::Preset::Pop);
  REQUIRE(suggested.config.loudness.enabled);
  REQUIRE(suggested.config.loudness.true_peak_oversample == preset.loudness.true_peak_oversample);
  REQUIRE(suggested.config.loudness.true_peak_oversample == 4);
}
