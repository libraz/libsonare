/// @file melody_analyzer_test.cpp
/// @brief Tests for melody analyzer.

#include "analysis/melody_analyzer.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "support/audio_fixtures.h"
#include "util/constants.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
using sonare::test::generate_sine_audio;

/// @brief Creates a melody with multiple pitches.
Audio create_melody(const std::vector<float>& freqs, float note_duration, int sr = 22050) {
  int samples_per_note = static_cast<int>(sr * note_duration);
  int total_samples = samples_per_note * static_cast<int>(freqs.size());
  std::vector<float> samples(total_samples);

  for (size_t n = 0; n < freqs.size(); ++n) {
    float freq = freqs[n];
    int start = static_cast<int>(n) * samples_per_note;

    for (int i = 0; i < samples_per_note; ++i) {
      float t = static_cast<float>(i) / static_cast<float>(sr);
      // Apply envelope to reduce clicks
      float env = 1.0f;
      if (i < samples_per_note / 10) {
        env = static_cast<float>(i) / (samples_per_note / 10);
      } else if (i > samples_per_note * 9 / 10) {
        env = static_cast<float>(samples_per_note - i) / (samples_per_note / 10);
      }
      samples[start + i] = 0.8f * env * std::sin(2.0f * sonare::constants::kPiD * freq * t);
    }
  }

  return Audio::from_vector(std::move(samples), sr);
}

Audio create_gapped_vibrato(float carrier_hz, float vibrato_hz, float depth_cents,
                            float voiced_duration, float gap_duration, int sr = 22050) {
  const int voiced_samples = static_cast<int>(sr * voiced_duration);
  const int gap_samples = static_cast<int>(sr * gap_duration);
  std::vector<float> samples(static_cast<size_t>(2 * voiced_samples + gap_samples), 0.0f);
  const float depth_octaves = depth_cents / 1200.0f;
  double phase = 0.0;

  auto render_run = [&](int start_sample) {
    for (int i = 0; i < voiced_samples; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(sr);
      const float vibrato = std::sin(2.0f * sonare::constants::kPiD * vibrato_hz * t);
      const float freq = carrier_hz * std::pow(2.0f, depth_octaves * vibrato);
      phase += 2.0 * sonare::constants::kPiD * static_cast<double>(freq) / static_cast<double>(sr);
      samples[static_cast<size_t>(start_sample + i)] = 0.7f * static_cast<float>(std::sin(phase));
    }
  };

  render_run(0);
  render_run(voiced_samples + gap_samples);
  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("MelodyAnalyzer basic", "[melody_analyzer]") {
  Audio audio = generate_sine_audio(440.0f, 22050, 1.0f, 0.8f);

  MelodyConfig config;
  MelodyAnalyzer analyzer(audio, config);

  REQUIRE(analyzer.count() > 0);
}

TEST_CASE("MelodyAnalyzer A440 detection", "[melody_analyzer]") {
  Audio audio = generate_sine_audio(440.0f, 22050, 1.0f, 0.8f);

  MelodyConfig config;
  config.threshold = 0.15f;
  MelodyAnalyzer analyzer(audio, config);

  REQUIRE(analyzer.has_melody());

  // Mean frequency should be close to 440 Hz
  if (analyzer.mean_frequency() > 0.0f) {
    REQUIRE_THAT(analyzer.mean_frequency(), WithinRel(440.0f, 0.1f));
  }
}

TEST_CASE("MelodyAnalyzer pitch times", "[melody_analyzer]") {
  Audio audio = generate_sine_audio(440.0f, 22050, 1.0f, 0.8f);

  MelodyAnalyzer analyzer(audio);

  auto times = analyzer.pitch_times();

  REQUIRE(!times.empty());
  REQUIRE(times.size() == analyzer.count());

  // Times should be monotonically increasing
  for (size_t i = 1; i < times.size(); ++i) {
    REQUIRE(times[i] > times[i - 1]);
  }
}

TEST_CASE("MelodyAnalyzer pitch frequencies", "[melody_analyzer]") {
  Audio audio = generate_sine_audio(440.0f, 22050, 1.0f, 0.8f);

  MelodyAnalyzer analyzer(audio);

  auto frequencies = analyzer.pitch_frequencies();

  REQUIRE(frequencies.size() == analyzer.count());
}

TEST_CASE("MelodyAnalyzer pitch confidences", "[melody_analyzer]") {
  Audio audio = generate_sine_audio(440.0f, 22050, 1.0f, 0.8f);

  MelodyAnalyzer analyzer(audio);

  auto confidences = analyzer.pitch_confidences();

  REQUIRE(confidences.size() == analyzer.count());

  for (float c : confidences) {
    REQUIRE(c >= 0.0f);
    REQUIRE(c <= 1.0f);
  }
}

TEST_CASE("MelodyAnalyzer contour features", "[melody_analyzer]") {
  Audio audio = generate_sine_audio(440.0f, 22050, 1.0f, 0.8f);

  MelodyAnalyzer analyzer(audio);

  const auto& contour = analyzer.contour();

  REQUIRE(contour.pitch_range_octaves >= 0.0f);
  REQUIRE(contour.pitch_stability >= 0.0f);
  REQUIRE(contour.pitch_stability <= 1.0f);
}

TEST_CASE("MelodyAnalyzer stability for pure tone", "[melody_analyzer]") {
  Audio audio = generate_sine_audio(440.0f, 22050, 2.0f, 0.8f);

  MelodyConfig config;
  config.threshold = 0.15f;
  MelodyAnalyzer analyzer(audio, config);

  // Pure tone should have high stability
  if (analyzer.has_melody()) {
    REQUIRE(analyzer.stability() >= 0.5f);
  }
}

TEST_CASE("MelodyAnalyzer vibrato rate ignores unvoiced gaps", "[melody_analyzer]") {
  Audio audio = create_gapped_vibrato(440.0f, 5.0f, 35.0f, 1.0f, 2.0f);

  MelodyConfig config;
  config.frame_length = 1024;
  config.hop_length = 128;
  config.threshold = 0.2f;
  MelodyAnalyzer analyzer(audio, config);

  REQUIRE(analyzer.has_melody());
  REQUIRE(analyzer.contour().vibrato_rate > 3.5f);
  REQUIRE(analyzer.contour().vibrato_rate < 7.0f);
}

TEST_CASE("MelodyAnalyzer melody with multiple pitches", "[melody_analyzer]") {
  // C-D-E-F-G melody
  std::vector<float> freqs = {261.63f, 293.66f, 329.63f, 349.23f, 392.00f};
  Audio audio = create_melody(freqs, 0.5f);

  MelodyConfig config;
  config.threshold = 0.15f;
  MelodyAnalyzer analyzer(audio, config);

  // Should detect pitch points
  REQUIRE(analyzer.count() > 0);

  // Pitch range should span multiple notes
  if (analyzer.has_melody()) {
    REQUIRE(analyzer.pitch_range() > 0.0f);
  }
}

TEST_CASE("MelodyAnalyzer frequency range config", "[melody_analyzer]") {
  Audio audio = generate_sine_audio(440.0f, 22050, 1.0f, 0.8f);

  MelodyConfig config;
  config.fmin = 400.0f;
  config.fmax = 500.0f;

  MelodyAnalyzer analyzer(audio, config);

  // Should still detect pitch within range
  auto frequencies = analyzer.pitch_frequencies();
  for (float f : frequencies) {
    if (f > 0.0f) {
      REQUIRE(f >= config.fmin);
      REQUIRE(f <= config.fmax);
    }
  }
}

TEST_CASE("MelodyAnalyzer frame/hop config", "[melody_analyzer]") {
  Audio audio = generate_sine_audio(440.0f, 22050, 1.0f, 0.8f);

  MelodyConfig config1;
  config1.frame_length = 2048;
  config1.hop_length = 256;
  MelodyAnalyzer analyzer1(audio, config1);

  MelodyConfig config2;
  config2.frame_length = 1024;
  config2.hop_length = 128;
  MelodyAnalyzer analyzer2(audio, config2);

  // Smaller hop should produce more frames
  REQUIRE(analyzer2.count() > analyzer1.count());
}

TEST_CASE("MelodyAnalyzer short audio", "[melody_analyzer]") {
  Audio audio = generate_sine_audio(440.0f, 22050, 0.2f, 0.8f);

  MelodyConfig config;
  MelodyAnalyzer analyzer(audio, config);

  // Plain YIN frames left-aligned windows that fit entirely: 4410 samples,
  // 2048-sample frames, 256-sample hop.
  const size_t expected =
      (audio.size() - static_cast<size_t>(config.frame_length)) / config.hop_length + 1;
  REQUIRE(analyzer.count() == expected);
  std::vector<float> voiced;
  for (size_t i = 0; i < analyzer.count(); ++i) {
    const PitchPoint& point = analyzer.contour().pitches[i];
    CAPTURE(i, point.time, point.frequency, point.confidence);
    CHECK_THAT(point.time, WithinAbs(static_cast<float>(i * config.hop_length) / 22050.0f, 1e-6f));
    CHECK(std::isfinite(point.frequency));
    CHECK((point.frequency == 0.0f ||
           (point.frequency >= config.fmin && point.frequency <= config.fmax)));
    CHECK(point.confidence >= 0.0f);
    CHECK(point.confidence <= 1.0f);
    if (point.frequency > 0.0f) voiced.push_back(point.frequency);
  }
  // Every frame of a steady tone is voiced at its pitch.
  REQUIRE(voiced.size() == analyzer.count());
  CHECK_THAT(analyzer.mean_frequency(), WithinRel(440.0f, 0.01f));
}

TEST_CASE("MelodyAnalyzer audio shorter than one frame yields no frames", "[melody_analyzer]") {
  Audio audio = generate_sine_audio(440.0f, 22050, 0.05f, 0.8f);  // 1102 < 2048 samples
  MelodyAnalyzer analyzer(audio, MelodyConfig());
  CHECK(analyzer.count() == 0);
  CHECK_FALSE(analyzer.has_melody());
}

TEST_CASE("MelodyAnalyzer different frequencies", "[melody_analyzer]") {
  // Test at different frequencies
  std::vector<float> test_freqs = {200.0f, 300.0f, 500.0f, 800.0f};

  for (float freq : test_freqs) {
    Audio audio = generate_sine_audio(freq, 22050, 1.0f, 0.8f);

    MelodyConfig config;
    config.threshold = 0.2f;
    MelodyAnalyzer analyzer(audio, config);

    if (analyzer.has_melody() && analyzer.mean_frequency() > 0.0f) {
      // Mean frequency should be within 15% of actual
      REQUIRE_THAT(analyzer.mean_frequency(), WithinRel(freq, 0.15f));
    }
  }
}

TEST_CASE("MelodyAnalyzer has_melody", "[melody_analyzer]") {
  Audio sine = generate_sine_audio(440.0f, 22050, 1.0f, 0.8f);

  MelodyAnalyzer analyzer(sine);

  // Sine wave should have melody
  // (Note: may vary based on threshold)
  REQUIRE(analyzer.count() > 0);
}

TEST_CASE("MelodyAnalyzer reports no vibrato on a steady tone", "[melody_analyzer]") {
  // Frame-to-frame tracker jitter reverses direction constantly on a held tone;
  // none of it is a pitch oscillation.
  MelodyAnalyzer analyzer(generate_sine_audio(440.0f, 22050, 3.0f, 0.5f));
  REQUIRE(analyzer.has_melody());
  CHECK(analyzer.contour().vibrato_rate < 1.0f);
}

TEST_CASE("MelodyAnalyzer onset time and pitch agree across input rates", "[melody_analyzer]") {
  constexpr float kSilenceSec = 0.5f;
  constexpr float kToneSec = 1.5f;
  constexpr float kToneHz = 440.0f;
  constexpr int kHop = 128;
  const int rates[] = {22050, 44100, 48000};

  float first_voiced[3] = {};
  for (int r = 0; r < 3; ++r) {
    const int sr = rates[r];
    const size_t n_silence = static_cast<size_t>(kSilenceSec * sr);
    const size_t n_tone = static_cast<size_t>(kToneSec * sr);
    std::vector<float> samples(n_silence + n_tone, 0.0f);
    for (size_t i = 0; i < n_tone; ++i) {
      samples[n_silence + i] =
          0.5f * std::sin(2.0f * sonare::constants::kPi * kToneHz * static_cast<float>(i) / sr);
    }
    MelodyConfig config;
    config.hop_length = kHop;
    MelodyAnalyzer analyzer(Audio::from_vector(std::move(samples), sr), config);

    std::vector<float> voiced;
    first_voiced[r] = -1.0f;
    for (const auto& p : analyzer.contour().pitches) {
      if (p.frequency <= 0.0f) continue;
      if (first_voiced[r] < 0.0f) first_voiced[r] = p.time;
      voiced.push_back(p.frequency);
    }
    INFO("sr=" << sr << " first voiced=" << first_voiced[r]);
    REQUIRE_FALSE(voiced.empty());
    std::sort(voiced.begin(), voiced.end());
    float median = voiced[voiced.size() / 2];
    float cents = sonare::constants::kCentsPerOctave * std::log2(median / kToneHz);
    CHECK(std::abs(cents) <= 10.0f);
  }
  for (int a = 0; a < 3; ++a) {
    for (int b = a + 1; b < 3; ++b) {
      INFO("rates " << rates[a] << " vs " << rates[b]);
      CHECK(std::abs(first_voiced[a] - first_voiced[b]) <=
            static_cast<float>(kHop) / rates[a] + static_cast<float>(kHop) / rates[b]);
    }
  }
}
