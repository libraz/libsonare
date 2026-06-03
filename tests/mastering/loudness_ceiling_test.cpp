#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "mastering/api/presets.h"
#include "util/constants.h"

namespace api = sonare::mastering::api;

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

}  // namespace

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
