#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "core/audio.h"
#include "mastering/api/chain.h"
#include "mastering/api/presets.h"
#include "mastering/common/loudness_measure.h"
#include "util/constants.h"

namespace api = sonare::mastering::api;
using Catch::Matchers::WithinAbs;

namespace {

constexpr int kSampleRate = 24000;
using sonare::constants::kPi;

// ITU-R BS.1770-4 inter-sample true-peak measurement tolerance (dB). The
// band-limited limiter targets the ceiling exactly; this slack accounts for
// estimator error in the 4x-oversampled true-peak meter.
constexpr float kTruePeakTolerance = 0.3f;

// Generate a 220 Hz + 880 Hz sine mix at the given peak amplitude.
std::vector<float> make_tone(float amplitude) {
  constexpr float seconds = 0.75f;
  std::vector<float> samples(static_cast<size_t>(seconds * kSampleRate), 0.0f);
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
    samples[i] = amplitude * (0.66f * std::sin(2.0f * kPi * 220.0f * t) +
                              0.34f * std::sin(2.0f * kPi * 880.0f * t));
  }
  return samples;
}

// Program-like material: a sustained two-tone body plus periodic transients,
// scaled so its integrated loudness is exactly @p target_lufs. The transient
// crest is chosen so that at -14 LUFS the true peak sits just under 0 dBFS,
// which is the peak-normalized condition where the ceiling headroom vanishes.
std::vector<float> make_peak_normalized_program(float target_lufs) {
  constexpr float seconds = 2.0f;
  constexpr int transient_len = 180;
  std::vector<float> samples(static_cast<size_t>(seconds * kSampleRate), 0.0f);
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
    samples[i] = 0.62f * std::sin(2.0f * kPi * 220.0f * t) +
                 0.30f * std::sin(2.0f * kPi * 660.0f * t) +
                 0.14f * std::sin(2.0f * kPi * 1320.0f * t);
    const int local = static_cast<int>(i) % (kSampleRate / 4);
    if (local < transient_len) {
      const float decay = 1.0f - static_cast<float>(local) / static_cast<float>(transient_len);
      samples[i] += 1.9f * decay * decay * std::sin(2.0f * kPi * 3000.0f * t);
    }
  }
  const float current =
      sonare::mastering::common::measure_lufs(samples.data(), samples.size(), kSampleRate);
  const float gain = std::pow(10.0f, (target_lufs - current) / 20.0f);
  for (float& sample : samples) sample *= gain;
  return samples;
}

float program_true_peak_dbtp(const std::vector<float>& samples) {
  const sonare::Audio audio =
      sonare::Audio::from_buffer(samples.data(), samples.size(), kSampleRate);
  return sonare::mastering::common::measure_true_peak_dbtp(audio, 4);
}

float stage_gain_reduction_db(const api::MonoChainResult& result, const std::string& stage) {
  for (const auto& entry : result.stage_gain_reductions) {
    if (entry.stage == stage) return entry.gain_reduction_db;
  }
  return 0.0f;
}

}  // namespace

TEST_CASE("loud preset drives its limiter toward the target on peak-normalized material",
          "[mastering][loudness][ceiling]") {
  // presets.cpp: kpop targets -8 LUFS at a -0.5 dBTP ceiling.
  constexpr float kInputLufs = -14.0f;
  constexpr float kTargetLufs = -8.0f;
  constexpr float kCeilingDb = -0.5f;
  // A single normalization pass cannot land exactly on target: the limiter that
  // makes the target reachable takes some of the gain back, and the stage does
  // not iterate. This bounds that residual on the peakiest built-in signal.
  constexpr float kLoudnessTolerance = 2.0f;

  const auto samples = make_peak_normalized_program(kInputLufs);
  const float input_peak_dbtp = program_true_peak_dbtp(samples);
  // The stage only misbehaves when the ceiling headroom is gone, so assert the
  // material really is peak-normalized before drawing any conclusion from it.
  REQUIRE(input_peak_dbtp > kCeilingDb);

  const auto result = api::master_audio_mono(api::preset_from_string("kpop"), samples.data(),
                                             samples.size(), kSampleRate);
  CAPTURE(input_peak_dbtp, result.input_lufs, result.output_lufs, result.applied_gain_db,
          stage_gain_reduction_db(result, "loudness.optimize"), result.output_true_peak_dbtp,
          result.loudness_target_limited);

  CHECK_THAT(result.output_lufs, WithinAbs(kTargetLufs, kLoudnessTolerance));
  // Whatever the loudness outcome, the ceiling still holds.
  CHECK(result.output_true_peak_dbtp <= kCeilingDb + kTruePeakTolerance);
  // The limiter is the stage that closes the distance to the target, so it has
  // to have done real work rather than passing the signal through.
  CHECK(stage_gain_reduction_db(result, "loudness.optimize") < -1.0f);
}

TEST_CASE("presets with different loudness targets deliver different masters",
          "[mastering][loudness][ceiling]") {
  // Same peak-normalized mix through a -8 LUFS and a -14 LUFS preset: a headroom
  // clamp collapses both to the input loudness and makes the choice inaudible.
  const auto samples = make_peak_normalized_program(-14.0f);
  const auto loud = api::master_audio_mono(api::preset_from_string("kpop"), samples.data(),
                                           samples.size(), kSampleRate);
  const auto quiet = api::master_audio_mono(api::preset_from_string("pop"), samples.data(),
                                            samples.size(), kSampleRate);
  CAPTURE(loud.output_lufs, quiet.output_lufs);
  CHECK(loud.output_lufs > quiet.output_lufs + 3.0f);
}

TEST_CASE("zero limiter allowance restores the strict headroom clamp and reports it",
          "[mastering][loudness][ceiling]") {
  const auto samples = make_peak_normalized_program(-14.0f);
  const api::Param overrides[] = {{"loudness.maxLimiterGainReductionDb", 0.0}};
  const auto result = api::master_audio_mono(api::preset_from_string("kpop"), samples.data(),
                                             samples.size(), kSampleRate, overrides, 1);
  CAPTURE(result.output_lufs, result.applied_gain_db);
  // With no allowance the static gain stops at the ceiling headroom, so the
  // target is unreachable -- which the result has to say rather than imply.
  CHECK(result.output_lufs < -14.0f);
  CHECK(result.loudness_target_limited);
}

TEST_CASE("loudness limiter allowance rejects a negative or non-finite value",
          "[mastering][loudness][ceiling]") {
  api::MasteringChainConfig config;
  config.loudness.enabled = true;
  config.loudness.max_limiter_gain_reduction_db = -1.0f;
  REQUIRE_THROWS(api::MasteringChain{config});

  config.loudness.max_limiter_gain_reduction_db = std::numeric_limits<float>::quiet_NaN();
  REQUIRE_THROWS(api::MasteringChain{config});
}

TEST_CASE("mono loudness limiter keeps output true peak at or below preset ceiling",
          "[mastering][loudness][ceiling]") {
  struct PresetCase {
    std::string name;
    float ceiling_db;  // Must match enable_loudness(...) in presets.cpp.
  };
  // Ceilings hardcoded from src/mastering/api/presets.cpp enable_loudness calls.
  const std::vector<PresetCase> presets = {
      {"pop", -1.0f},
      {"edm", -0.3f},
      {"classical", -2.0f},
  };

  struct SignalCase {
    std::string name;
    float amplitude;
  };
  const std::vector<SignalCase> signals = {
      {"quiet", 0.03f},  // far below loudness target -> requires substantial gain-up
      {"loud", 0.9f},    // requires gain-down / heavy limiting
  };

  for (const auto& preset : presets) {
    for (const auto& signal : signals) {
      const auto samples = make_tone(signal.amplitude);
      const auto result = api::master_audio_mono(api::preset_from_string(preset.name),
                                                 samples.data(), samples.size(), kSampleRate);

      CAPTURE(preset.name, preset.ceiling_db, signal.name, signal.amplitude, result.input_lufs,
              result.output_lufs, result.output_true_peak_dbtp, result.output_lra);

      // A band-limited true-peak limiter must keep output at or below ceiling
      // (within the BS.1770-4 measurement tolerance).
      CHECK(result.output_true_peak_dbtp <= preset.ceiling_db + kTruePeakTolerance);
    }
  }
}

TEST_CASE("mono loudness gains up a quiet signal toward the target LUFS",
          "[mastering][loudness][ceiling]") {
  // pop preset targets -14 LUFS. A very quiet input must be brought up close to
  // target (unlike the old gain-capped path, which under-shot).
  constexpr float kTargetLufs = -14.0f;
  constexpr float kLoudnessTolerance = 3.0f;  // generous: confirm gain-up happens

  const auto samples = make_tone(0.03f);
  const auto result = api::master_audio_mono(api::preset_from_string("pop"), samples.data(),
                                             samples.size(), kSampleRate);

  CAPTURE(result.input_lufs, result.output_lufs, result.applied_gain_db,
          result.output_true_peak_dbtp);

  CHECK(std::abs(result.output_lufs - kTargetLufs) <= kLoudnessTolerance);
}

TEST_CASE("chain output true peak follows the configured oversample factor",
          "[mastering][loudness][ceiling]") {
  const std::vector<float> samples = {0.0f, 0.99f, 0.99f, 0.0f, -0.99f, -0.99f, 0.0f};
  const sonare::Audio audio = sonare::Audio::from_buffer(samples.data(), samples.size(), 48000);

  api::MasteringChainConfig config;
  config.loudness.true_peak_oversample = 1;
  api::MasteringChain chain(config);
  const auto result = chain.process_mono(samples.data(), samples.size(), 48000);

  CHECK(result.output_true_peak_dbtp ==
        Catch::Approx(sonare::mastering::common::measure_true_peak_dbtp(audio, 1)).margin(1e-5));
  CHECK(result.output_true_peak_dbtp !=
        Catch::Approx(sonare::mastering::common::measure_true_peak_dbtp(audio, 8)).margin(0.1));
}
