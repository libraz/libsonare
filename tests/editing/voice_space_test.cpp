/// @file voice_space_test.cpp
/// @brief Voice changer space stage: reverb passthrough and echo-family delays.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "editing/voice_changer/space_stage.h"
#include "support/alloc_guard.h"

using namespace sonare::editing::voice_changer;
using sonare::test::AllocationGuard;

namespace {

StreamingReverbConfig reverb_with_mix(float mix) {
  StreamingReverbConfig config;
  config.mix = mix;
  return config;
}

ReverbVariationConfig echo_config(ReverbVariation kind, float delay_ms, float feedback) {
  ReverbVariationConfig v;
  v.variation = kind;
  v.delay_ms = delay_ms;
  v.feedback = feedback;
  return v;
}

std::vector<float> run_impulse(SpaceStage& stage, int length) {
  std::vector<float> out(static_cast<std::size_t>(length));
  for (int i = 0; i < length; ++i) out[static_cast<std::size_t>(i)] = stage.process_sample(i == 0);
  return out;
}

// Index of the largest echo after the dry sample.
int first_echo_index(const std::vector<float>& out) {
  int best = 1;
  for (std::size_t i = 1; i < out.size(); ++i) {
    if (std::fabs(out[i]) > std::fabs(out[static_cast<std::size_t>(best)])) {
      best = static_cast<int>(i);
    }
  }
  return best;
}

std::vector<float> noise(int length, std::uint32_t seed, float amplitude) {
  std::vector<float> v(static_cast<std::size_t>(length));
  for (auto& s : v) {
    seed = seed * 1664525u + 1013904223u;
    s = amplitude * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
  }
  return v;
}

double rms(const std::vector<float>& v, int begin, int end) {
  double sum = 0.0;
  for (int i = begin; i < end; ++i)
    sum += static_cast<double>(v[static_cast<std::size_t>(i)]) * v[static_cast<std::size_t>(i)];
  return std::sqrt(sum / (end - begin));
}

int measured_delay_samples(double sr, const ReverbVariationConfig& variation) {
  SpaceStage stage;
  stage.prepare(sr);
  stage.set_config(reverb_with_mix(0.3f), variation);
  const double expected_ms = space_delay_ms(variation);
  const int length = static_cast<int>(expected_ms * 0.001 * sr) + 200;
  return first_echo_index(run_impulse(stage, length));
}

}  // namespace

TEST_CASE("Echo delay lands within one sample of delay_ms", "[voice_space]") {
  for (double sr : {44100.0, 48000.0}) {
    const auto v = echo_config(ReverbVariation::Echo, 123.4f, 0.4f);
    const double expected = 123.4 * 0.001 * sr;
    REQUIRE(std::fabs(measured_delay_samples(sr, v) - expected) <= 1.0);
  }
}

TEST_CASE("Tempo delay time follows bpm and note value", "[voice_space]") {
  ReverbVariationConfig v;
  v.variation = ReverbVariation::TempoDelay;
  v.tempo_bpm = 137.0f;
  const struct {
    NoteDivision division;
    double value;
  } cases[] = {{NoteDivision::Quarter, 1.0},
               {NoteDivision::Eighth, 0.5},
               {NoteDivision::DottedEighth, 0.75},
               {NoteDivision::Sixteenth, 0.25}};
  for (const auto& c : cases) {
    v.division = c.division;
    const double expected_ms = 60.0 * 1000.0 / 137.0 * c.value;
    REQUIRE(std::fabs(space_delay_ms(v) - expected_ms) < 1e-3);
    const double sr = 48000.0;
    REQUIRE(std::fabs(measured_delay_samples(sr, v) - expected_ms * 0.001 * sr) <= 1.0);
  }
}

TEST_CASE("Echo delay holds the same time at 44.1 and 48 kHz", "[voice_space]") {
  const auto v = echo_config(ReverbVariation::Echo, 200.0f, 0.4f);
  double seconds[2];
  int i = 0;
  for (double sr : {44100.0, 48000.0}) {
    const int n = measured_delay_samples(sr, v);
    // Against the requested value, then against the other rate.
    REQUIRE(std::fabs(n / sr - 0.2) <= 1.0 / sr);
    seconds[i++] = n / sr;
  }
  REQUIRE(std::fabs(seconds[0] - seconds[1]) <= 1.0 / 44100.0 + 1.0 / 48000.0);
}

TEST_CASE("Changing the delay glides without a jump", "[voice_space]") {
  constexpr double kSr = 48000.0;
  SpaceStage stage;
  stage.prepare(kSr);
  stage.set_config(reverb_with_mix(0.4f), echo_config(ReverbVariation::Echo, 100.0f, 0.5f));
  const int n = 48000;
  std::vector<float> out(static_cast<std::size_t>(n));
  auto sine = [&](int i) { return 0.5f * std::sin(2.0f * 3.14159265f * 220.0f * i / 48000.0f); };
  float steady = 0.0f;
  for (int i = 0; i < 12000; ++i) {
    const float y = stage.process_sample(sine(i));
    out[static_cast<std::size_t>(i)] = y;
    if (i > 8000) steady = std::max(steady, std::fabs(y - out[static_cast<std::size_t>(i - 1)]));
  }
  std::size_t allocations = 0;
  {
    AllocationGuard guard;
    stage.set_config(reverb_with_mix(0.4f), echo_config(ReverbVariation::Echo, 130.0f, 0.5f));
    for (int i = 12000; i < n; ++i) {
      out[static_cast<std::size_t>(i)] = stage.process_sample(sine(i));
    }
    allocations = guard.count();
  }
  float moved = 0.0f;
  for (int i = 12000; i < n; ++i) {
    moved = std::max(
        moved, std::fabs(out[static_cast<std::size_t>(i)] - out[static_cast<std::size_t>(i - 1)]));
  }
  REQUIRE(allocations == 0);
  REQUIRE(steady > 0.01f);
  REQUIRE(moved < 2.0f * steady);
}

TEST_CASE("Dub echo decays faster than echo at equal feedback", "[voice_space]") {
  constexpr double kSr = 48000.0;
  const int burst = 960;
  const int delay = 2400;  // 50 ms
  const auto input = noise(burst, 7u, 0.5f);
  double level[2];
  int k = 0;
  for (ReverbVariation kind : {ReverbVariation::Echo, ReverbVariation::DubEcho}) {
    SpaceStage stage;
    stage.prepare(kSr);
    stage.set_config(reverb_with_mix(0.45f), echo_config(kind, 50.0f, 0.8f));
    std::vector<float> out(static_cast<std::size_t>(delay * 5 + burst));
    for (std::size_t i = 0; i < out.size(); ++i) {
      out[i] = stage.process_sample(i < input.size() ? input[i] : 0.0f);
    }
    level[k++] = rms(out, delay * 4, delay * 4 + burst);
  }
  REQUIRE(level[0] > 0.01);
  REQUIRE(level[1] < 0.5 * level[0]);
}

TEST_CASE("Reverb variation matches StreamingReverb alone", "[voice_space]") {
  constexpr double kSr = 48000.0;
  const StreamingReverbConfig config = reverb_with_mix(0.3f);
  StreamingReverb reference;
  reference.prepare(kSr, 512);
  reference.set_config(config);
  SpaceStage stage;
  stage.prepare(kSr);
  stage.set_config(config, ReverbVariationConfig{});
  const auto input = noise(4000, 3u, 0.4f);
  float worst = 0.0f;
  for (float x : input) {
    worst = std::max(worst, std::fabs(stage.process_sample(x) - reference.process_sample(x)));
  }
  REQUIRE(worst < 1e-6f);
}

TEST_CASE("Delays beyond 3000 ms are refused", "[voice_space]") {
  REQUIRE_FALSE(space_delay_exceeds_limit(echo_config(ReverbVariation::Echo, 3000.0f, 0.3f)));
  REQUIRE(space_delay_exceeds_limit(echo_config(ReverbVariation::DubEcho, 3001.0f, 0.3f)));
  ReverbVariationConfig tempo;
  tempo.variation = ReverbVariation::TempoDelay;
  tempo.division = NoteDivision::Quarter;
  tempo.tempo_bpm = 30.0f;  // 2000 ms
  REQUIRE_FALSE(space_delay_exceeds_limit(tempo));
  tempo.tempo_bpm = 19.0f;  // 3158 ms
  REQUIRE(space_delay_exceeds_limit(tempo));

  // set_config ignores a refused setting and keeps the previous delay.
  constexpr double kSr = 48000.0;
  SpaceStage stage;
  stage.prepare(kSr);
  stage.set_config(reverb_with_mix(0.3f), echo_config(ReverbVariation::Echo, 100.0f, 0.3f));
  stage.set_config(reverb_with_mix(0.3f), echo_config(ReverbVariation::Echo, 4000.0f, 0.3f));
  REQUIRE(std::abs(first_echo_index(run_impulse(stage, 6000)) - 4800) <= 1);
}

TEST_CASE("Space stage performs no allocation after prepare", "[voice_space]") {
  constexpr double kSr = 48000.0;
  SpaceStage stage;
  stage.prepare(kSr);
  stage.set_config(reverb_with_mix(0.3f), ReverbVariationConfig{});
  for (int i = 0; i < 256; ++i) stage.process_sample(0.1f);
  std::size_t allocations = 0;
  float sink = 0.0f;
  {
    AllocationGuard guard;
    for (ReverbVariation kind : {ReverbVariation::Echo, ReverbVariation::TempoDelay,
                                 ReverbVariation::DubEcho, ReverbVariation::Reverb}) {
      ReverbVariationConfig v = echo_config(kind, 80.0f, 0.5f);
      stage.set_config(reverb_with_mix(0.3f), v);
      for (int i = 0; i < 512; ++i) sink += stage.process_sample(0.1f);
      stage.reset();
    }
    allocations = guard.count();
  }
  REQUIRE(allocations == 0);
  REQUIRE(std::isfinite(sink));
}
