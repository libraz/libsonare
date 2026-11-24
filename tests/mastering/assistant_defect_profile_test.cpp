#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include "mastering/assistant/audio_profile.h"
#include "mastering/assistant/suggester.h"
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

std::vector<float> with_noise_off_flat(std::vector<float> samples, float amplitude) {
  std::mt19937 rng(20260917u);
  std::uniform_real_distribution<float> noise(-amplitude, amplitude);
  for (float& sample : samples) {
    if (std::abs(sample) < 0.8f) sample += noise(rng);
  }
  return samples;
}

assistant::DefectProfile profile_of(const std::vector<float>& samples) {
  return assistant::analyze_audio_profile(samples.data(), samples.size(), kSr, detecting_config())
      .defects;
}

std::vector<float> negate(const std::vector<float>& samples) {
  std::vector<float> result = samples;
  for (float& sample : result) sample = -sample;
  return result;
}

std::vector<float> interleave(const std::vector<float>& left, const std::vector<float>& right) {
  REQUIRE(left.size() == right.size());
  std::vector<float> result;
  result.reserve(left.size() * 2);
  for (size_t i = 0; i < left.size(); ++i) {
    result.push_back(left[i]);
    result.push_back(right[i]);
  }
  return result;
}

assistant::DefectProfile interleaved_profile(const std::vector<float>& samples) {
  return assistant::analyze_audio_profile_interleaved(samples.data(), samples.size() / 2, 2, kSr,
                                                      detecting_config())
      .defects;
}

// Orders a level that was not measured (NaN) below every measured one, so a peak search skips it.
bool measured_level_less(float a, float b) { return std::isnan(a) || (!std::isnan(b) && a < b); }

// Loudest of the noise detector's bands.
float band_peak_dbfs(const assistant::DefectProfile& defects) {
  return *std::max_element(std::begin(defects.noise.band_floor_dbfs),
                           std::end(defects.noise.band_floor_dbfs), measured_level_less);
}

std::ptrdiff_t band_peak_index(const assistant::DefectProfile& defects) {
  return std::max_element(std::begin(defects.noise.band_floor_dbfs),
                          std::end(defects.noise.band_floor_dbfs), measured_level_less) -
         std::begin(defects.noise.band_floor_dbfs);
}

// Loudest k*f0 of the hum series.
float peak_harmonic_dbfs(const assistant::DefectProfile& defects) {
  return *std::max_element(std::begin(defects.hum.harmonic_dbfs),
                           std::end(defects.hum.harmonic_dbfs), measured_level_less);
}

}  // namespace

TEST_CASE("Defect profile click fields separate planted clicks from a clean tone",
          "[mastering][assistant][defects]") {
  const auto clean = profile_of(tone(220.0f, 0.2f));
  const auto planted = profile_of(with_spikes(tone(220.0f, 0.2f), 2048, 0.9f));

  CAPTURE(clean.click.count, planted.click.count, clean.click.per_second, planted.click.per_second);
  REQUIRE(clean.click.count == 0);
  REQUIRE(planted.click.count >= 8);
  REQUIRE(planted.click.per_second > clean.click.per_second);
  REQUIRE(planted.click.longest_run_samples > clean.click.longest_run_samples);
}

TEST_CASE("Defect profile crackle fields separate dense small deviations from a clean tone",
          "[mastering][assistant][defects]") {
  // Deviation 0.5 clears the decrackle threshold (0.4) while the peak stays
  // under the declick threshold (0.8), so this moves crackle and not clicks.
  const auto clean = profile_of(tone(220.0f, 0.2f));
  const auto planted = profile_of(with_spikes(tone(220.0f, 0.2f), 32, 0.5f));

  CAPTURE(clean.crackle.sample_count, planted.crackle.sample_count, clean.crackle.sample_fraction,
          planted.crackle.sample_fraction);
  REQUIRE(clean.crackle.sample_count == 0);
  REQUIRE(planted.crackle.sample_count >= 600);
  REQUIRE(planted.crackle.sample_fraction > clean.crackle.sample_fraction);
  REQUIRE(planted.crackle.per_second > clean.crackle.per_second);
}

TEST_CASE("Defect profile clip fields separate a clipped tone from the same tone below the ceiling",
          "[mastering][assistant][defects]") {
  const auto clean = profile_of(clipped(0.5f, 1.0f));
  const auto planted = profile_of(clipped(1.4f, 1.0f));

  CAPTURE(clean.clip.sample_count, planted.clip.sample_count, clean.clip.sample_fraction,
          planted.clip.sample_fraction);
  REQUIRE(clean.clip.sample_count == 0);
  REQUIRE(clean.clip.run_count == 0);
  REQUIRE(planted.clip.sample_count >= 2000);
  REQUIRE(planted.clip.run_count >= 100);
  REQUIRE(planted.clip.sample_fraction > clean.clip.sample_fraction);
  REQUIRE(planted.clip.longest_run_samples > clean.clip.longest_run_samples);
}

TEST_CASE("Defect profile still sees clipping after the file has been turned down",
          "[mastering][assistant][defects]") {
  // The four threshold fields go to zero once nothing reaches the ceiling, which
  // is the whole population of material clipped in one tool and attenuated in
  // the next. The flat fields are the ones that have to survive it.
  auto turned_down = clipped(1.4f, 1.0f);
  for (float& sample : turned_down) sample *= 0.25f;
  const auto quiet = profile_of(turned_down);
  const auto loud = profile_of(clipped(1.4f, 1.0f));

  CAPTURE(quiet.clip.sample_count, quiet.clip.flat_run_count, quiet.clip.flat_level);
  REQUIRE(quiet.clip.sample_count == 0);
  REQUIRE(quiet.clip.run_count == 0);
  REQUIRE(quiet.clip.flat_run_count == loud.clip.flat_run_count);
  REQUIRE(quiet.clip.flat_sample_count == loud.clip.flat_sample_count);
  REQUIRE(quiet.clip.longest_flat_run_samples == loud.clip.longest_flat_run_samples);
  REQUIRE(quiet.clip.flat_level < 0.3f);
  REQUIRE(quiet.clip.flat_level > 0.2f);

  // The unclipped tone is the negative side: it reaches the ceiling on every
  // cycle without a flat top, so a count alone would call it clipped.
  const auto untouched = profile_of(tone(220.0f, 1.0f));
  CAPTURE(untouched.clip.sample_count, untouched.clip.flat_run_count);
  REQUIRE(untouched.clip.sample_count > 0);
  REQUIRE(untouched.clip.flat_run_count == 0);
  REQUIRE(untouched.clip.flat_level == 0.0f);
}

TEST_CASE("Defect profile noise fields separate added white noise from a clean tone",
          "[mastering][assistant][defects]") {
  const auto clean = profile_of(tone(220.0f, 0.2f));
  const auto planted = profile_of(with_noise(tone(220.0f, 0.2f), 0.15f));

  CAPTURE(clean.noise.floor_dbfs, planted.noise.floor_dbfs, band_peak_dbfs(clean),
          band_peak_dbfs(planted));
  REQUIRE(planted.noise.floor_dbfs > clean.noise.floor_dbfs + 6.0f);
  REQUIRE(band_peak_dbfs(planted) > band_peak_dbfs(clean) + 6.0f);
}

TEST_CASE("Defect profile hum fields separate a planted mains series from a clean tone",
          "[mastering][assistant][defects]") {
  auto hummed = tone(440.0f, 0.2f);
  add_tone(hummed, 50.0f, 0.10f);
  add_tone(hummed, 100.0f, 0.05f);
  add_tone(hummed, 150.0f, 0.03f);

  const auto clean = profile_of(tone(440.0f, 0.2f));
  const auto planted = profile_of(hummed);

  CAPTURE(clean.hum.harmonics, planted.hum.harmonics, clean.hum.fundamental_prominence,
          planted.hum.fundamental_prominence, clean.hum.harmonic_dbfs[0],
          planted.hum.harmonic_dbfs[0]);
  REQUIRE(planted.hum.harmonics > clean.hum.harmonics);
  REQUIRE(planted.hum.harmonic_dbfs[0] > clean.hum.harmonic_dbfs[0] + 12.0f);
  REQUIRE(peak_harmonic_dbfs(planted) > peak_harmonic_dbfs(clean) + 12.0f);
  // The clean tone does not sit at the detector's 1.0 "found no peak" reading --
  // its search still picks a winner out of the programme material. What the
  // field has to do is separate, and an order of magnitude apart is what makes a
  // tracked mains series readable from a picked candidate.
  REQUIRE(planted.hum.fundamental_prominence > 5.0f * clean.hum.fundamental_prominence);
}

TEST_CASE("Defect profile finds a 60 Hz mains series, not just a 50 Hz one",
          "[mastering][assistant][defects]") {
  auto hummed = tone(440.0f, 0.2f);
  add_tone(hummed, 60.0f, 0.10f);
  add_tone(hummed, 120.0f, 0.05f);
  add_tone(hummed, 180.0f, 0.03f);

  const auto clean = profile_of(tone(440.0f, 0.2f));
  const auto planted = profile_of(hummed);

  CAPTURE(clean.hum.fundamental_hz, planted.hum.fundamental_hz, clean.hum.fundamental_prominence,
          planted.hum.fundamental_prominence, planted.hum.harmonics);
  // One detector search reaches a couple of Hz either side of its configured
  // fundamental, so it can only ever find one of the two mains frequencies; the
  // other lands on the search boundary at a prominence barely above clean
  // material, which reads as "no hum" and is not. The profile searches both.
  REQUIRE(std::abs(planted.hum.fundamental_hz - 60.0f) <= 0.25f);
  REQUIRE(planted.hum.fundamental_prominence > 5.0f * clean.hum.fundamental_prominence);
  REQUIRE(planted.hum.harmonics >= 2);
}

TEST_CASE("Defect profile late decay separates a reverberant copy from the dry bursts",
          "[mastering][assistant][defects]") {
  const auto dry = bursts();
  const auto clean = profile_of(dry);
  const auto planted = profile_of(with_tail(dry, 0.05f, 0.7f));

  CAPTURE(clean.reverb.late_decay_ratio_db, planted.reverb.late_decay_ratio_db);
  // Less negative means the material sustains across the module's late lag, so
  // the reverberant copy reads HIGHER here. This is not an RT60 and does not
  // invert like one.
  REQUIRE(planted.reverb.late_decay_ratio_db > clean.reverb.late_decay_ratio_db);
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
  REQUIRE(defects.click.count == untouched.click.count);
  REQUIRE(defects.click.rejected == untouched.click.rejected);
  REQUIRE(defects.click.longest_run_samples == untouched.click.longest_run_samples);
  REQUIRE(defects.click.per_second == untouched.click.per_second);
  REQUIRE(defects.crackle.sample_count == untouched.crackle.sample_count);
  REQUIRE(defects.crackle.sample_fraction == untouched.crackle.sample_fraction);
  REQUIRE(defects.crackle.per_second == untouched.crackle.per_second);
  REQUIRE(defects.clip.sample_count == untouched.clip.sample_count);
  REQUIRE(defects.clip.run_count == untouched.clip.run_count);
  REQUIRE(defects.clip.longest_run_samples == untouched.clip.longest_run_samples);
  REQUIRE(defects.clip.sample_fraction == untouched.clip.sample_fraction);
  REQUIRE(defects.noise.floor_dbfs == untouched.noise.floor_dbfs);
  REQUIRE(std::equal(std::begin(defects.noise.band_floor_dbfs),
                     std::end(defects.noise.band_floor_dbfs),
                     std::begin(untouched.noise.band_floor_dbfs)));
  REQUIRE(defects.hum.fundamental_hz == untouched.hum.fundamental_hz);
  REQUIRE(defects.hum.fundamental_prominence == untouched.hum.fundamental_prominence);
  REQUIRE(defects.hum.harmonics == untouched.hum.harmonics);
  REQUIRE(std::equal(std::begin(defects.hum.harmonic_dbfs), std::end(defects.hum.harmonic_dbfs),
                     std::begin(untouched.hum.harmonic_dbfs)));
  REQUIRE(defects.reverb.late_decay_ratio_db == untouched.reverb.late_decay_ratio_db);
}

TEST_CASE("Defect profile JSON leaves the block out when it was not measured",
          "[mastering][assistant][defects]") {
  const auto samples = with_spikes(tone(220.0f, 0.2f), 2048, 0.9f);

  assistant::AudioProfileConfig off;
  off.n_fft = 1024;
  off.hop_length = 256;
  const json::Value unmeasured = json::parse(assistant::audio_profile_to_json(
      assistant::analyze_audio_profile(samples.data(), samples.size(), kSr, off)));
  const json::Value measured = json::parse(assistant::audio_profile_to_json(
      assistant::analyze_audio_profile(samples.data(), samples.size(), kSr, detecting_config())));

  // An unmeasured profile leaves the block out rather than writing zeros a clean recording would.
  REQUIRE_FALSE(unmeasured.contains("defects"));
  REQUIRE(measured.contains("defects"));
  REQUIRE(measured["defects"]["click"]["count"].as_int() >= 8);
  const std::size_t bands = measured["defects"]["noise"]["bandFloorDbfs"].size();
  REQUIRE(bands > 0);
  REQUIRE(bands <= sonare::mastering::repair::kRepairNoiseBandCount);
  REQUIRE(measured["defects"]["noise"]["bandFloorBand"].size() == bands);
  // Every harmonic of a 50 Hz series sits below Nyquist at this rate, so all are measured.
  REQUIRE(measured["defects"]["hum"]["harmonicDbfs"].size() ==
          static_cast<std::size_t>(sonare::mastering::repair::kDehumMaxHarmonics));
  // The profile path never runs WPE, so the predictability is absent, not 0.
  REQUIRE_FALSE(measured["defects"]["reverb"].contains("latePredictability"));
}

TEST_CASE("Defect JSON level arrays never spell not measured as a slot",
          "[mastering][assistant][defects]") {
  namespace repair = sonare::mastering::repair;
  constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
  constexpr float kInf = std::numeric_limits<float>::infinity();

  SECTION("noise bands travel as measured levels with their grid index") {
    repair::NoiseDetection detected;
    std::fill(std::begin(detected.band_floor_dbfs), std::end(detected.band_floor_dbfs), -70.0f);
    detected.floor_dbfs = -60.0f;
    detected.band_floor_dbfs[0] = kNaN;   // a band no bin reaches, at the bottom
    detected.band_floor_dbfs[2] = kNaN;   // and between two measured ones
    detected.band_floor_dbfs[5] = -kInf;  // a band with no energy is measured
    const json::Value out =
        json::parse(json::dump(json::Value(assistant::detection_to_json(detected))));
    const json::Array& levels = out["bandFloorDbfs"].as_array();
    const json::Array& index = out["bandFloorBand"].as_array();
    REQUIRE(levels.size() == repair::kRepairNoiseBandCount - 2);
    REQUIRE(index.size() == levels.size());
    CHECK(index[0].as_int() == 1);
    CHECK(index[1].as_int() == 3);
    CHECK(index[3].as_int() == 5);
    CHECK(levels[3].is_null());  // -inf, the level's infinite end
    CHECK(levels[0].as_number() == -70.0);
    for (std::size_t i = 1; i < index.size(); ++i) CHECK(index[i].as_int() > index[i - 1].as_int());
  }

  SECTION("hum harmonics end at the last one below Nyquist") {
    // 100 Hz at a 1 kHz rate: harmonics 5 and up sit at or past Nyquist.
    constexpr int kRate = 1000;
    std::vector<float> hum(static_cast<std::size_t>(kRate) * 4);
    for (std::size_t i = 0; i < hum.size(); ++i) {
      hum[i] = 0.1f * std::sin(kTwoPi * 100.0f * static_cast<float>(i) / kRate);
    }
    repair::DehumConfig config;
    config.fundamental_hz = 100.0f;
    const repair::HumDetection detected = repair::detect_hum(hum.data(), hum.size(), kRate, config);
    REQUIRE(detected.measured_harmonics == 4);
    for (int k = 0; k < repair::kDehumMaxHarmonics; ++k) {
      CAPTURE(k);
      CHECK(std::isnan(detected.harmonic_dbfs[k]) == (k >= 4));
    }
    const json::Value out =
        json::parse(json::dump(json::Value(assistant::detection_to_json(detected))));
    CHECK(out["harmonicDbfs"].size() == 4);

    const std::vector<float> short_silence(32, 0.0f);
    const float* short_channel[] = {short_silence.data()};
    const assistant::ChannelSetDefects unmeasured =
        assistant::measure_defects_planar(short_channel, 1, short_silence.size(), kSr);
    REQUIRE_FALSE(unmeasured.aggregate.measured);
    REQUIRE(unmeasured.aggregate.hum.measured_harmonics == 0);
    CHECK(json::Value(assistant::detection_to_json(unmeasured.aggregate.hum))["harmonicDbfs"]
              .size() == 0);
  }
}

TEST_CASE("Interleaved anti-phase clipping retains flat evidence for the repair suggestion",
          "[mastering][assistant][defects]") {
  const auto left = clipped(1.4f, 1.0f);
  const auto interleaved = interleave(left, negate(left));
  const auto profile = interleaved_profile(interleaved);

  CAPTURE(profile.clip.flat_run_count, profile.clip.flat_sample_count, profile.clip.flat_level);
  REQUIRE(profile.measured);
  REQUIRE(profile.clip.flat_run_count > 0);
  REQUIRE(profile.clip.flat_sample_count > 0);
  REQUIRE(profile.clip.flat_level > 0.9f);

  assistant::AssistantConfig config;
  config.enable_repair = true;
  const auto result =
      assistant::suggest_chain_interleaved(interleaved.data(), left.size(), 2, kSr, config);
  REQUIRE(result.config.repair.declip.enabled);
  REQUIRE(result.config.repair.declip.config.clip_threshold > 0.9f);
}

TEST_CASE("Interleaved flat-top aggregation uses the highest evidenced plateau",
          "[mastering][assistant][defects]") {
  auto left = clipped(1.4f, 1.0f);
  auto right = clipped(1.4f, 1.0f);
  for (float& sample : left) sample *= 0.25f;
  for (float& sample : right) sample *= 0.5f;

  const auto left_profile = profile_of(left);
  const auto right_profile = profile_of(right);
  const auto interleaved = interleave(left, right);
  const auto profile = interleaved_profile(interleaved);

  CAPTURE(left_profile.clip.flat_level, right_profile.clip.flat_level, profile.clip.flat_level);
  REQUIRE(left_profile.clip.flat_level > 0.2f);
  REQUIRE(right_profile.clip.flat_level > left_profile.clip.flat_level);
  REQUIRE(std::abs(profile.clip.flat_level - right_profile.clip.flat_level) < 1.0e-6f);
  REQUIRE(profile.declip_threshold_safe);
}

TEST_CASE("Interleaved flat evidence is unsafe when a clean channel reaches the threshold",
          "[mastering][assistant][defects]") {
  auto clipped_quiet = clipped(1.4f, 1.0f);
  for (float& sample : clipped_quiet) sample *= 0.25f;
  const auto clean_loud = tone(220.0f, 0.5f);
  const auto profile = interleaved_profile(interleave(clipped_quiet, clean_loud));

  CAPTURE(profile.clip.flat_level, profile.declip_threshold_safe);
  REQUIRE(profile.clip.flat_level > 0.2f);
  REQUIRE_FALSE(profile.declip_threshold_safe);
}

TEST_CASE("Interleaved safety clamps an above-unity plateau before checking clean peaks",
          "[mastering][assistant][defects]") {
  auto above_unity = clipped(1.4f, 1.0f);
  for (float& sample : above_unity) sample *= 1.5f;
  const auto clean_loud = tone(220.0f, 1.2f);
  const auto profile = interleaved_profile(interleave(above_unity, clean_loud));

  CAPTURE(profile.clip.flat_level, profile.declip_threshold_safe);
  REQUIRE(profile.clip.flat_level > 1.0f);
  REQUIRE_FALSE(profile.declip_threshold_safe);
}

TEST_CASE("Interleaved anti-phase noise survives the noise detector",
          "[mastering][assistant][defects]") {
  const auto left = with_noise(tone(220.0f, 0.2f), 0.15f);
  const auto interleaved = interleave(left, negate(left));
  const auto profile = interleaved_profile(interleaved);

  CAPTURE(profile.noise.floor_dbfs, band_peak_dbfs(profile), band_peak_index(profile));
  REQUIRE(profile.noise.floor_dbfs > -80.0f);
  REQUIRE(band_peak_dbfs(profile) > -80.0f);
}

TEST_CASE("Duplicated mono defects preserve rates and fractions and double noise power",
          "[mastering][assistant][defects]") {
  auto source = with_noise_off_flat(clipped(1.4f, 1.0f), 0.05f);
  source = with_spikes(std::move(source), 2048, 0.9f);
  const auto mono = profile_of(source);
  const auto interleaved = interleave(source, source);
  const auto stereo = interleaved_profile(interleaved);
  const float duplicate_power_db = 10.0f * std::log10(2.0f);

  CAPTURE(mono.click.count, stereo.click.count, mono.crackle.sample_count,
          stereo.crackle.sample_count, mono.clip.sample_count, stereo.clip.sample_count,
          mono.noise.floor_dbfs, stereo.noise.floor_dbfs);
  REQUIRE(stereo.click.count == mono.click.count * 2);
  REQUIRE(stereo.click.rejected == mono.click.rejected * 2);
  REQUIRE(stereo.click.longest_run_samples == mono.click.longest_run_samples);
  REQUIRE(std::abs(stereo.click.per_second - mono.click.per_second) < 1.0e-6f);
  REQUIRE(stereo.crackle.sample_count == mono.crackle.sample_count * 2);
  REQUIRE(std::abs(stereo.crackle.sample_fraction - mono.crackle.sample_fraction) < 1.0e-6f);
  REQUIRE(std::abs(stereo.crackle.per_second - mono.crackle.per_second) < 1.0e-6f);
  REQUIRE(stereo.clip.sample_count == mono.clip.sample_count * 2);
  REQUIRE(stereo.clip.run_count == mono.clip.run_count * 2);
  REQUIRE(stereo.clip.longest_run_samples == mono.clip.longest_run_samples);
  REQUIRE(std::abs(stereo.clip.sample_fraction - mono.clip.sample_fraction) < 1.0e-6f);
  REQUIRE(stereo.clip.flat_run_count == mono.clip.flat_run_count * 2);
  REQUIRE(stereo.clip.flat_sample_count == mono.clip.flat_sample_count * 2);
  REQUIRE(stereo.clip.longest_flat_run_samples == mono.clip.longest_flat_run_samples);
  REQUIRE(std::abs(stereo.clip.flat_level - mono.clip.flat_level) < 1.0e-6f);
  REQUIRE(std::abs(stereo.noise.floor_dbfs - mono.noise.floor_dbfs - duplicate_power_db) < 0.01f);
  REQUIRE(std::abs(band_peak_dbfs(stereo) - band_peak_dbfs(mono) - duplicate_power_db) < 0.01f);
  REQUIRE(band_peak_index(stereo) == band_peak_index(mono));

  assistant::AssistantConfig config;
  config.enable_repair = true;
  const auto mono_result = assistant::suggest_chain(source.data(), source.size(), kSr, config);
  const auto stereo_result =
      assistant::suggest_chain_interleaved(interleaved.data(), source.size(), 2, kSr, config);
  REQUIRE(stereo_result.config.repair.declick.enabled == mono_result.config.repair.declick.enabled);
  REQUIRE(stereo_result.config.repair.declip.enabled == mono_result.config.repair.declip.enabled);
  REQUIRE(stereo_result.config.repair.decrackle.enabled ==
          mono_result.config.repair.decrackle.enabled);
  REQUIRE(stereo_result.config.repair.dehum.enabled == mono_result.config.repair.dehum.enabled);
  REQUIRE(stereo_result.config.repair.dereverb.enabled ==
          mono_result.config.repair.dereverb.enabled);
  REQUIRE(stereo_result.config.repair.denoise.enabled == mono_result.config.repair.denoise.enabled);
}

TEST_CASE("the audio profile schema list matches what the writer emits",
          "[mastering][assistant][defects]") {
  auto signal = with_spikes(tone(220.0f, 0.2f), 2048, 0.9f);
  add_tone(signal, 50.0f, 0.05f);
  const auto profile =
      assistant::analyze_audio_profile(signal.data(), signal.size(), kSr, detecting_config());
  REQUIRE(profile.defects.measured);

  const auto actual = sonare::test::schema_paths_of(assistant::audio_profile_to_json(profile));
  const auto& listed = assistant::audio_profile_schema_paths();
  std::set<std::string> expected(listed.begin(), listed.end());
  // A level that was not measured is an absent member; every other listed path is written.
  const auto& spectral = profile.spectral;
  const std::pair<const char*, float> levels[] = {
      {"defects.noise.floorDbfs", profile.defects.noise.floor_dbfs},
      {"loudness.loudnessRangeLu", profile.loudness.lra_lu},
      {"loudness.truePeakDbtp", profile.loudness.true_peak_db},
      {"loudness.crestFactorDb", profile.loudness.crest_factor_db},
      {"spectral.subRmsDb", spectral.sub_rms_db},
      {"spectral.lowRmsDb", spectral.low_rms_db},
      {"spectral.lowMidRmsDb", spectral.low_mid_rms_db},
      {"spectral.midRmsDb", spectral.mid_rms_db},
      {"spectral.highMidRmsDb", spectral.high_mid_rms_db},
      {"spectral.highRmsDb", spectral.high_rms_db},
      {"spectral.airRmsDb", spectral.air_rms_db}};
  for (const auto& [path, level] : levels) {
    if (std::isnan(level)) expected.erase(path);
  }
  REQUIRE_FALSE(actual.empty());
  REQUIRE(actual == expected);
}
