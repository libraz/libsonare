#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "mastering/assistant/audio_profile.h"
#include "mastering/assistant/suggester.h"

namespace assistant = sonare::mastering::assistant;

namespace {

constexpr float kPi = 3.14159265358979323846f;

std::vector<float> tone(int sr, float seconds, float frequency, float amplitude = 0.4f) {
  std::vector<float> samples(static_cast<size_t>(seconds * static_cast<float>(sr)));
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = amplitude * std::sin(2.0f * kPi * frequency * t);
  }
  return samples;
}

void add_tone(std::vector<float>& samples, int sr, float frequency, float amplitude) {
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] += amplitude * std::sin(2.0f * kPi * frequency * t);
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
    const float env = 0.08f + 0.45f * (0.5f + 0.5f * std::sin(2.0f * kPi * 0.25f * t));
    samples[i] =
        env * (std::sin(2.0f * kPi * 330.0f * t) + 0.5f * std::sin(2.0f * kPi * 660.0f * t));
  }
  return samples;
}

std::vector<float> speech_like(int sr, float seconds) {
  std::vector<float> samples(static_cast<size_t>(seconds * static_cast<float>(sr)));
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    const float syllable = 0.55f + 0.45f * std::max(0.0f, std::sin(2.0f * kPi * 4.0f * t));
    samples[i] = syllable * (0.20f * std::sin(2.0f * kPi * 170.0f * t) +
                             0.16f * std::sin(2.0f * kPi * 900.0f * t) +
                             0.08f * std::sin(2.0f * kPi * 1800.0f * t));
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
