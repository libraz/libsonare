#include "mastering/maximizer/maximizer.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "analysis/meter/lufs.h"
#include "analysis/meter/true_peak.h"
#include "mastering/maximizer/adaptive_release.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "mastering/maximizer/soft_knee_max.h"
#include "mastering/maximizer/streaming_preview.h"
#include "mastering/maximizer/true_peak_limiter.h"

using Catch::Matchers::WithinAbs;
using namespace sonare;
using namespace sonare::mastering::maximizer;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(float frequency_hz, int sample_rate, int samples, float amplitude) {
  std::vector<float> out(static_cast<size_t>(samples));
  for (int i = 0; i < samples; ++i) {
    out[static_cast<size_t>(i)] =
        amplitude * static_cast<float>(std::sin(2.0 * kPi * frequency_hz * i / sample_rate));
  }
  return out;
}

Audio sine_audio(float amplitude, int sample_rate = 48000, float duration_sec = 1.0f) {
  const int samples = static_cast<int>(duration_sec * static_cast<float>(sample_rate));
  return Audio::from_vector(sine(1000.0f, sample_rate, samples, amplitude), sample_rate);
}

float peak_abs(const std::vector<float>& samples) {
  float peak = 0.0f;
  for (float sample : samples) {
    peak = std::max(peak, std::abs(sample));
  }
  return peak;
}

void process(sonare::mastering::common::ProcessorBase& processor, std::vector<float>& mono) {
  float* channels[] = {mono.data()};
  processor.process(channels, 1, static_cast<int>(mono.size()));
}

}  // namespace

TEST_CASE("Maximizer applies input gain and respects ceiling", "[mastering][maximizer]") {
  Maximizer maximizer({12.0f, -6.0f, 0.0f, 0.0f});
  maximizer.prepare(48000.0, 512);

  auto signal = sine(1000.0f, 48000, 2048, 0.5f);
  process(maximizer, signal);

  REQUIRE(peak_abs(signal) <= 0.502f);
  REQUIRE(maximizer.last_gain_reduction_db() < -5.5f);
}

TEST_CASE("TruePeakLimiter enforces interpolated ceiling", "[mastering][maximizer]") {
  TruePeakLimiter limiter({-6.0f, 0.0f, 0.0f, 4});
  limiter.prepare(48000.0, 64);

  std::vector<float> signal = {0.0f, 1.0f, 0.0f, -1.0f};
  process(limiter, signal);

  REQUIRE(peak_abs(signal) <= 0.502f);
  REQUIRE(limiter.last_gain_reduction_db() < -5.5f);
}

TEST_CASE("TruePeakLimiter catches sinc-estimated inter-sample overs", "[mastering][maximizer]") {
  TruePeakLimiter limiter({-1.0f, 0.0f, 0.0f, 8});
  limiter.prepare(48000.0, 64);

  std::vector<float> signal = {0.0f, 0.99f, 0.99f, 0.0f, -0.99f, -0.99f, 0.0f};
  process(limiter, signal);

  const Audio limited = Audio::from_buffer(signal.data(), signal.size(), 48000);
  REQUIRE(analysis::meter::true_peak_db(limited, 8) <= -0.99f);
}

TEST_CASE("SoftKneeMax softens drive and respects ceiling", "[mastering][maximizer]") {
  SoftKneeMax maximizer({6.0f, -3.0f, 6.0f, 0.0f});
  maximizer.prepare(48000.0, 512);

  auto signal = sine(1000.0f, 48000, 2048, 0.9f);
  process(maximizer, signal);

  REQUIRE(peak_abs(signal) <= 0.708f);
  REQUIRE(maximizer.last_gain_reduction_db() <= 0.0f);
}

TEST_CASE("AdaptiveRelease limits peaks and adapts release", "[mastering][maximizer]") {
  AdaptiveRelease limiter({-6.0f, 0.0f, 10.0f, 120.0f});
  limiter.prepare(48000.0, 512);

  auto signal = sine(1000.0f, 48000, 2048, 1.0f);
  process(limiter, signal);
  REQUIRE(peak_abs(signal) <= 0.502f);

  auto second = sine(1000.0f, 48000, 2048, 1.0f);
  process(limiter, second);

  REQUIRE(peak_abs(second) <= 0.502f);
  REQUIRE(limiter.current_release_ms() > 10.0f);
  REQUIRE(limiter.current_release_ms() <= 120.0f);
}

TEST_CASE("LoudnessOptimize moves loudness toward target without exceeding ceiling",
          "[mastering][maximizer]") {
  const Audio input = sine_audio(0.05f);
  const auto result = loudness_optimize(input, {-20.0f, -1.0f, 4});

  REQUIRE(result.audio.size() == input.size());
  REQUIRE(result.applied_gain_db > 0.0f);
  REQUIRE(std::isfinite(result.input_lufs));
  REQUIRE(std::isfinite(result.output_lufs));
  REQUIRE(std::abs(result.output_lufs + 20.0f) < std::abs(result.input_lufs + 20.0f));
  REQUIRE(analysis::meter::true_peak_db(result.audio, 4) <= -0.99f);
}

TEST_CASE("LoudnessOptimize caps gain when target would exceed ceiling", "[mastering][maximizer]") {
  const Audio input = sine_audio(0.8f);
  const auto result = loudness_optimize(input, {0.0f, -6.0f, 4});

  REQUIRE(result.applied_gain_db < 0.0f);
  REQUIRE(analysis::meter::true_peak_db(result.audio, 4) <= -5.99f);
}

TEST_CASE("StreamingPreview reports platform normalization and ceiling risk",
          "[mastering][maximizer]") {
  const Audio input = sine_audio(0.2f);
  const std::vector<StreamingPlatform> platforms = {{"Test", 0.0f, -6.0f}};

  const auto result = streaming_preview(input, platforms);

  REQUIRE(result.size() == 1);
  REQUIRE(result[0].name == "Test");
  REQUIRE(std::isfinite(result[0].integrated_lufs));
  REQUIRE(std::isfinite(result[0].true_peak_db));
  REQUIRE_THAT(result[0].normalization_gain_db,
               WithinAbs(0.0f - result[0].integrated_lufs, 0.001f));
  REQUIRE(result[0].ceiling_risk);
}

TEST_CASE("Maximizer processors validate configuration and state", "[mastering][maximizer]") {
  REQUIRE_THROWS(Maximizer({0.0f, -1.0f, -1.0f, 10.0f}));
  REQUIRE_THROWS(TruePeakLimiter({-1.0f, 1.0f, 10.0f, 0}));
  REQUIRE_THROWS(AdaptiveRelease({-1.0f, 1.0f, 20.0f, 10.0f}));
  REQUIRE_THROWS(SoftKneeMax({0.0f, -1.0f, -1.0f, 10.0f}));

  Maximizer unprepared;
  std::vector<float> signal(4, 0.0f);
  float* channels[] = {signal.data()};
  REQUIRE_THROWS(unprepared.process(channels, 1, 4));

  const Audio empty;
  REQUIRE_THROWS(loudness_optimize(empty));
  REQUIRE_THROWS(streaming_preview(empty));
}
