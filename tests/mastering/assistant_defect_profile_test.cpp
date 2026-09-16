#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <random>
#include <set>
#include <vector>

#include "mastering/assistant/audio_profile.h"
#include "support/schema_paths.h"
#include "util/constants.h"
#include "util/json.h"

namespace assistant = sonare::mastering::assistant;
namespace json = sonare::util::json;

namespace {

using sonare::constants::kTwoPi;

constexpr int kSr = 22050;
constexpr float kSeconds = 1.0f;

assistant::AudioProfileConfig detecting_config() {
  assistant::AudioProfileConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;
  config.detect_defects = true;
  return config;
}

std::vector<float> tone(float hz, float amplitude) {
  std::vector<float> samples(static_cast<size_t>(kSeconds * static_cast<float>(kSr)));
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSr);
    samples[i] = amplitude * std::sin(kTwoPi * hz * t);
  }
  return samples;
}

void add_tone(std::vector<float>& samples, float hz, float amplitude) {
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSr);
    samples[i] += amplitude * std::sin(kTwoPi * hz * t);
  }
}

/// Percussive bursts: the only material whose sustain across the dereverb lag
/// says something about a tail rather than about the note.
std::vector<float> bursts() {
  std::vector<float> samples(static_cast<size_t>(kSeconds * static_cast<float>(kSr)), 0.0f);
  const size_t period = static_cast<size_t>(kSr) / 4;
  for (size_t start = 0; start < samples.size(); start += period) {
    for (size_t n = 0; n < period / 8 && start + n < samples.size(); ++n) {
      const float t = static_cast<float>(n) / static_cast<float>(kSr);
      samples[start + n] = 0.5f * std::exp(-60.0f * t) * std::sin(kTwoPi * 600.0f * t);
    }
  }
  return samples;
}

/// Feedback comb at the dereverb module's own late lag, which is what makes the
/// wet copy predictable across that lag and the dry one not.
std::vector<float> with_tail(std::vector<float> samples, float delay_sec, float feedback) {
  const size_t delay = static_cast<size_t>(delay_sec * static_cast<float>(kSr));
  for (size_t i = delay; i < samples.size(); ++i) {
    samples[i] += feedback * samples[i - delay];
  }
  return samples;
}

std::vector<float> with_spikes(std::vector<float> samples, size_t stride, float amplitude) {
  for (size_t i = stride; i < samples.size(); i += stride) {
    samples[i] += amplitude;
  }
  return samples;
}

std::vector<float> clipped(float amplitude, float ceiling) {
  auto samples = tone(220.0f, amplitude);
  for (float& sample : samples) {
    sample = std::clamp(sample, -ceiling, ceiling);
  }
  return samples;
}

std::vector<float> with_noise(std::vector<float> samples, float amplitude) {
  std::mt19937 rng(20260917u);
  std::uniform_real_distribution<float> noise(-amplitude, amplitude);
  for (float& sample : samples) {
    sample += noise(rng);
  }
  return samples;
}

assistant::DefectProfile profile_of(const std::vector<float>& samples) {
  return assistant::analyze_audio_profile(samples.data(), samples.size(), kSr, detecting_config())
      .defects;
}

}  // namespace

TEST_CASE("Defect profile click fields separate planted clicks from a clean tone",
          "[mastering][assistant][defects]") {
  const auto clean = profile_of(tone(220.0f, 0.2f));
  const auto planted = profile_of(with_spikes(tone(220.0f, 0.2f), 2048, 0.9f));

  CAPTURE(clean.click_count, planted.click_count, clean.click_per_second, planted.click_per_second);
  REQUIRE(clean.click_count == 0);
  REQUIRE(planted.click_count >= 8);
  REQUIRE(planted.click_per_second > clean.click_per_second);
  REQUIRE(planted.click_longest_run_samples > clean.click_longest_run_samples);
}

TEST_CASE("Defect profile crackle fields separate dense small deviations from a clean tone",
          "[mastering][assistant][defects]") {
  // Deviation 0.5 clears the decrackle threshold (0.4) while the peak stays
  // under the declick threshold (0.8), so this moves crackle and not clicks.
  const auto clean = profile_of(tone(220.0f, 0.2f));
  const auto planted = profile_of(with_spikes(tone(220.0f, 0.2f), 32, 0.5f));

  CAPTURE(clean.crackle_sample_count, planted.crackle_sample_count, clean.crackle_sample_fraction,
          planted.crackle_sample_fraction);
  REQUIRE(clean.crackle_sample_count == 0);
  REQUIRE(planted.crackle_sample_count >= 600);
  REQUIRE(planted.crackle_sample_fraction > clean.crackle_sample_fraction);
  REQUIRE(planted.crackle_per_second > clean.crackle_per_second);
}

TEST_CASE("Defect profile clip fields separate a clipped tone from the same tone below the ceiling",
          "[mastering][assistant][defects]") {
  const auto clean = profile_of(clipped(0.5f, 1.0f));
  const auto planted = profile_of(clipped(1.4f, 1.0f));

  CAPTURE(clean.clip_sample_count, planted.clip_sample_count, clean.clip_sample_fraction,
          planted.clip_sample_fraction);
  REQUIRE(clean.clip_sample_count == 0);
  REQUIRE(clean.clip_run_count == 0);
  REQUIRE(planted.clip_sample_count >= 2000);
  REQUIRE(planted.clip_run_count >= 100);
  REQUIRE(planted.clip_sample_fraction > clean.clip_sample_fraction);
  REQUIRE(planted.clip_longest_run_samples > clean.clip_longest_run_samples);
}

TEST_CASE("Defect profile noise fields separate added white noise from a clean tone",
          "[mastering][assistant][defects]") {
  const auto clean = profile_of(tone(220.0f, 0.2f));
  const auto planted = profile_of(with_noise(tone(220.0f, 0.2f), 0.15f));

  CAPTURE(clean.noise_floor_dbfs, planted.noise_floor_dbfs, clean.noise_band_peak_dbfs,
          planted.noise_band_peak_dbfs);
  REQUIRE(planted.noise_floor_dbfs > clean.noise_floor_dbfs + 6.0f);
  REQUIRE(planted.noise_band_peak_dbfs > clean.noise_band_peak_dbfs + 6.0f);
  // A measured band was picked on both sides, so the peak is a reading rather
  // than the "no band" sentinel.
  REQUIRE(clean.noise_band_peak_index >= 0);
  REQUIRE(planted.noise_band_peak_index >= 0);
}

TEST_CASE("Defect profile hum fields separate a planted mains series from a clean tone",
          "[mastering][assistant][defects]") {
  auto hummed = tone(440.0f, 0.2f);
  add_tone(hummed, 50.0f, 0.10f);
  add_tone(hummed, 100.0f, 0.05f);
  add_tone(hummed, 150.0f, 0.03f);

  const auto clean = profile_of(tone(440.0f, 0.2f));
  const auto planted = profile_of(hummed);

  CAPTURE(clean.hum_harmonics, planted.hum_harmonics, clean.hum_fundamental_prominence,
          planted.hum_fundamental_prominence, clean.hum_fundamental_dbfs,
          planted.hum_fundamental_dbfs);
  REQUIRE(planted.hum_harmonics > clean.hum_harmonics);
  REQUIRE(planted.hum_fundamental_dbfs > clean.hum_fundamental_dbfs + 12.0f);
  REQUIRE(planted.hum_peak_harmonic_dbfs > clean.hum_peak_harmonic_dbfs + 12.0f);
  // The clean tone does not sit at the detector's 1.0 "found no peak" reading --
  // its search still picks a winner out of the programme material. What the
  // field has to do is separate, and an order of magnitude apart is what makes a
  // tracked mains series readable from a picked candidate.
  REQUIRE(planted.hum_fundamental_prominence > 5.0f * clean.hum_fundamental_prominence);
}

TEST_CASE("Defect profile finds a 60 Hz mains series, not just a 50 Hz one",
          "[mastering][assistant][defects]") {
  auto hummed = tone(440.0f, 0.2f);
  add_tone(hummed, 60.0f, 0.10f);
  add_tone(hummed, 120.0f, 0.05f);
  add_tone(hummed, 180.0f, 0.03f);

  const auto clean = profile_of(tone(440.0f, 0.2f));
  const auto planted = profile_of(hummed);

  CAPTURE(clean.hum_fundamental_hz, planted.hum_fundamental_hz, clean.hum_fundamental_prominence,
          planted.hum_fundamental_prominence, planted.hum_harmonics);
  // One detector search reaches a couple of Hz either side of its configured
  // fundamental, so it can only ever find one of the two mains frequencies; the
  // other lands on the search boundary at a prominence barely above clean
  // material, which reads as "no hum" and is not. The profile searches both.
  REQUIRE(std::abs(planted.hum_fundamental_hz - 60.0f) <= 0.25f);
  REQUIRE(planted.hum_fundamental_prominence > 5.0f * clean.hum_fundamental_prominence);
  REQUIRE(planted.hum_harmonics >= 2);
}

TEST_CASE("Defect profile late decay separates a reverberant copy from the dry bursts",
          "[mastering][assistant][defects]") {
  const auto dry = bursts();
  const auto clean = profile_of(dry);
  const auto planted = profile_of(with_tail(dry, 0.05f, 0.7f));

  CAPTURE(clean.late_decay_ratio_db, planted.late_decay_ratio_db);
  // Less negative means the material sustains across the module's late lag, so
  // the reverberant copy reads HIGHER here. This is not an RT60 and does not
  // invert like one.
  REQUIRE(planted.late_decay_ratio_db > clean.late_decay_ratio_db);
}

TEST_CASE("Defect profile stays unmeasured until the config asks for it",
          "[mastering][assistant][defects]") {
  const auto samples = with_spikes(with_noise(tone(220.0f, 0.2f), 0.05f), 2048, 0.9f);

  assistant::AudioProfileConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;
  const auto profile =
      assistant::analyze_audio_profile(samples.data(), samples.size(), kSr, config);
  const assistant::DefectProfile& defects = profile.defects;
  const assistant::DefectProfile untouched{};

  // The same signal measured: every field below is one the detectors do move, so
  // a default read here is the config being honoured rather than a clean input.
  REQUIRE(profile_of(samples).measured);

  REQUIRE_FALSE(defects.measured);
  REQUIRE(defects.click_count == untouched.click_count);
  REQUIRE(defects.click_rejected == untouched.click_rejected);
  REQUIRE(defects.click_longest_run_samples == untouched.click_longest_run_samples);
  REQUIRE(defects.click_per_second == untouched.click_per_second);
  REQUIRE(defects.crackle_sample_count == untouched.crackle_sample_count);
  REQUIRE(defects.crackle_sample_fraction == untouched.crackle_sample_fraction);
  REQUIRE(defects.crackle_per_second == untouched.crackle_per_second);
  REQUIRE(defects.clip_sample_count == untouched.clip_sample_count);
  REQUIRE(defects.clip_run_count == untouched.clip_run_count);
  REQUIRE(defects.clip_longest_run_samples == untouched.clip_longest_run_samples);
  REQUIRE(defects.clip_sample_fraction == untouched.clip_sample_fraction);
  REQUIRE(defects.noise_floor_dbfs == untouched.noise_floor_dbfs);
  REQUIRE(defects.noise_band_peak_dbfs == untouched.noise_band_peak_dbfs);
  REQUIRE(defects.noise_band_peak_index == untouched.noise_band_peak_index);
  REQUIRE(defects.hum_fundamental_hz == untouched.hum_fundamental_hz);
  REQUIRE(defects.hum_fundamental_prominence == untouched.hum_fundamental_prominence);
  REQUIRE(defects.hum_harmonics == untouched.hum_harmonics);
  REQUIRE(defects.hum_fundamental_dbfs == untouched.hum_fundamental_dbfs);
  REQUIRE(defects.hum_peak_harmonic_dbfs == untouched.hum_peak_harmonic_dbfs);
  REQUIRE(defects.late_decay_ratio_db == untouched.late_decay_ratio_db);
}

TEST_CASE("Defect profile JSON carries the block whether or not it was measured",
          "[mastering][assistant][defects]") {
  const auto samples = with_spikes(tone(220.0f, 0.2f), 2048, 0.9f);

  assistant::AudioProfileConfig off;
  off.n_fft = 1024;
  off.hop_length = 256;
  const json::Value unmeasured = json::parse(assistant::audio_profile_to_json(
      assistant::analyze_audio_profile(samples.data(), samples.size(), kSr, off)));
  const json::Value measured = json::parse(assistant::audio_profile_to_json(
      assistant::analyze_audio_profile(samples.data(), samples.size(), kSr, detecting_config())));

  // Same shape either way: the flag is what a consumer reads, not the presence
  // of the block.
  REQUIRE(unmeasured.contains("defects"));
  REQUIRE(measured.contains("defects"));
  REQUIRE(unmeasured["defects"].size() == measured["defects"].size());

  REQUIRE_FALSE(unmeasured["defects"]["measured"].as_bool());
  REQUIRE(unmeasured["defects"]["clickCount"].as_int() == 0);
  REQUIRE(unmeasured["defects"]["noiseBandPeakIndex"].as_int() == -1);
  REQUIRE(unmeasured["defects"]["humFundamentalProminence"].as_float() == 1.0f);

  REQUIRE(measured["defects"]["measured"].as_bool());
  REQUIRE(measured["defects"]["clickCount"].as_int() >= 8);
  REQUIRE(measured["defects"]["noiseBandPeakIndex"].as_int() >= 0);
}

TEST_CASE("the audio profile schema list matches what the writer emits",
          "[mastering][assistant][defects]") {
  // An array contributes nothing of its own, so the fixture has to populate
  // genreCandidates or two of the listed paths could never be reached and the
  // set equality would fail for a reason that is not drift. A tone long enough
  // to be profiled always yields candidates; the REQUIRE below says so rather
  // than leaving it to luck.
  auto signal = with_spikes(tone(220.0f, 0.2f), 2048, 0.9f);
  add_tone(signal, 50.0f, 0.05f);
  const auto profile =
      assistant::analyze_audio_profile(signal.data(), signal.size(), kSr, detecting_config());
  REQUIRE_FALSE(profile.genre_candidates.empty());
  REQUIRE(profile.defects.measured);

  const auto actual = sonare::test::schema_paths_of(assistant::audio_profile_to_json(profile));
  const auto& listed = assistant::audio_profile_schema_paths();
  const std::set<std::string> expected(listed.begin(), listed.end());
  REQUIRE_FALSE(actual.empty());
  REQUIRE(actual == expected);
}
