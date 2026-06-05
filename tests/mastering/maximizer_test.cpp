#include "mastering/maximizer/maximizer.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "mastering/maximizer/adaptive_release.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "mastering/maximizer/soft_knee_max.h"
#include "mastering/maximizer/streaming_preview.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "metering/lufs.h"
#include "metering/true_peak.h"
#include "support/audio_fixtures.h"

using Catch::Matchers::WithinAbs;
using namespace sonare;
using namespace sonare::mastering::maximizer;

namespace {
using sonare::test::generate_sine_samples;
using sonare::test::peak_abs;
using sonare::test::process;

Audio sine_audio(float amplitude, int sample_rate = 48000, float duration_sec = 1.0f) {
  const int samples = static_cast<int>(duration_sec * static_cast<float>(sample_rate));
  return Audio::from_vector(generate_sine_samples(1000.0f, sample_rate, samples, amplitude),
                            sample_rate);
}

}  // namespace

TEST_CASE("Maximizer applies input gain and respects ceiling", "[mastering][maximizer]") {
  Maximizer maximizer({12.0f, -6.0f, 0.0f, 0.0f});
  maximizer.prepare(48000.0, 512);

  auto signal = generate_sine_samples(1000.0f, 48000, 2048, 0.5f);
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
  REQUIRE(metering::true_peak_db(limited, 8) <= -0.99f);
}

TEST_CASE("TruePeakLimiter supports 4x detection with input-rate gain fallback",
          "[mastering][maximizer]") {
  TruePeakLimiter limiter({-6.0f, 0.0f, 0.0f, 4, true});
  limiter.prepare(48000.0, 64);

  std::vector<float> signal = {0.0f, 1.2f, 0.3f, -1.1f, 0.0f};
  process(limiter, signal);

  REQUIRE(peak_abs(signal) <= 0.502f);
  REQUIRE(limiter.last_gain_reduction_db() < -5.5f);
}

TEST_CASE("TruePeakLimiter reports effective polyphase latency", "[mastering][maximizer]") {
  TruePeakLimiter limiter({-1.0f, 1.0f, 10.0f, 4});
  limiter.prepare(48000.0, 64);

  // Signal-path latency equals lookahead_samples_ only (1ms @ 48kHz = 48).
  // The centered upsampler/downsampler FIRs add zero group delay, so the
  // true-peak filter latency is NOT part of the reported signal-path delay.
  REQUIRE(limiter.latency_samples() == 48);

  limiter.set_config({-1.0f, 1.0f, 10.0f, 2});
  REQUIRE(limiter.latency_samples() == 48);
  REQUIRE_THROWS(TruePeakLimiter({-1.0f, 1.0f, 10.0f, 3}));
}

TEST_CASE("TruePeakLimiter set_config applies scalar changes without wiping running state",
          "[mastering][maximizer]") {
  TruePeakLimiter limiter({-6.0f, 1.0f, 25.0f, 4});
  limiter.prepare(48000.0, 256);

  auto hot = generate_sine_samples(1000.0f, 48000, 256, 0.95f);
  float* channel[] = {hot.data()};
  limiter.process(channel, 1, 256);
  const float reduction_db = limiter.last_gain_reduction_db();
  REQUIRE(reduction_db < -1.0f);

  // Scalar change (ceiling / release only): applied in place, the running
  // state — including the gain-reduction telemetry — must survive.
  limiter.set_config({-3.0f, 1.0f, 50.0f, 4});
  REQUIRE(limiter.last_gain_reduction_db() == reduction_db);

  // Structural change (lookahead) re-prepares and resets, and the new
  // lookahead is reflected in the reported latency.
  limiter.set_config({-3.0f, 2.0f, 50.0f, 4});
  REQUIRE(limiter.last_gain_reduction_db() == 0.0f);
  REQUIRE(limiter.latency_samples() == 96);

  // The updated -3 dB ceiling (0.708) is in effect on subsequent processing:
  // the 0.95 sine is limited near the NEW ceiling — clearly above the stale
  // -6 dB ceiling (0.5) and clearly below the unlimited input peak.
  auto follow_up = generate_sine_samples(1000.0f, 48000, 4800, 0.95f);
  for (size_t offset = 0; offset < follow_up.size(); offset += 256) {
    float* follow_channel[] = {follow_up.data() + offset};
    const int block = static_cast<int>(std::min<size_t>(256, follow_up.size() - offset));
    limiter.process(follow_channel, 1, block);
  }
  REQUIRE(peak_abs(follow_up) > 0.52f);
  REQUIRE(peak_abs(follow_up) <= 0.75f);
  REQUIRE(limiter.last_gain_reduction_db() < 0.0f);
}

TEST_CASE("TruePeakLimiter keeps polyphase detector state across blocks",
          "[mastering][maximizer]") {
  TruePeakLimiter full({-6.0f, 1.0f, 25.0f, 4});
  TruePeakLimiter split({-6.0f, 1.0f, 25.0f, 4});
  full.prepare(48000.0, 128);
  split.prepare(48000.0, 32);

  auto split_signal = generate_sine_samples(6000.0f, 48000, 256, 0.95f);
  for (size_t offset = 0; offset < split_signal.size(); offset += 16) {
    float* channel[] = {split_signal.data() + offset};
    split.process(channel, 1, static_cast<int>(std::min<size_t>(16, split_signal.size() - offset)));
  }

  const Audio limited = Audio::from_buffer(split_signal.data(), split_signal.size(), 48000);
  REQUIRE(metering::true_peak_db(limited, 4) <= -5.9f);
  REQUIRE(split.last_gain_reduction_db() < -1.0f);
}

TEST_CASE("TruePeakLimiter keeps mono channel state across stereo alternation",
          "[mastering][maximizer]") {
  TruePeakLimiter limiter({-6.0f, 1.0f, 0.0f, 4, true});
  limiter.prepare(1000.0, 1);

  float first_left[] = {1.0f};
  float* first_mono[] = {first_left};
  limiter.process(first_mono, 1, 1);
  REQUIRE_THAT(first_left[0], WithinAbs(0.0f, 0.0001f));

  float second_left[] = {0.0f};
  float second_right[] = {0.0f};
  float* second_stereo[] = {second_left, second_right};
  limiter.process(second_stereo, 2, 1);

  REQUIRE(second_left[0] > 0.2f);
  REQUIRE(second_left[0] <= 0.502f);
}

TEST_CASE("SoftKneeMax softens drive and respects ceiling", "[mastering][maximizer]") {
  SoftKneeMax maximizer({6.0f, -3.0f, 6.0f, 0.0f});
  maximizer.prepare(48000.0, 512);

  auto signal = generate_sine_samples(1000.0f, 48000, 2048, 0.9f);
  process(maximizer, signal);

  REQUIRE(peak_abs(signal) <= 0.708f);
  REQUIRE(maximizer.last_gain_reduction_db() <= 0.0f);
}

TEST_CASE("SoftKneeMax keeps knee and limiting in the same time reference",
          "[mastering][maximizer]") {
  // Regression for the knee/lookahead alignment fix: the soft-knee shaping is a
  // full pre-stage applied to the whole block before the maximizer runs, so the
  // maximizer's lookahead delay and detector both observe the knee-shaped
  // signal. The output must stay within the ceiling with no spurious overs that
  // a knee-vs-envelope timing skew would produce. The lookahead is non-zero so
  // any skew would be observable.
  SoftKneeMax maximizer({12.0f, -1.0f, 6.0f, 50.0f});
  maximizer.prepare(48000.0, 512);

  // A signal with sharp periodic transients exercises the lookahead alignment.
  std::vector<float> signal(2048, 0.0f);
  for (size_t i = 0; i < signal.size(); i += 64) {
    signal[i] = 0.95f;
  }
  process(maximizer, signal);

  const float ceiling = std::pow(10.0f, -1.0f / 20.0f);
  for (float s : signal) {
    REQUIRE(std::isfinite(s));
    REQUIRE(std::abs(s) <= ceiling + 0.01f);
  }
}

TEST_CASE("TruePeakLimiter detect-only forces base samples down for inter-sample overs",
          "[mastering][maximizer]") {
  // Regression for the detect-only (apply_gain_at_input_rate) min-over-window
  // mapping: the gain applied to each base sample is the minimum gain across the
  // oversampled subsamples of that base sample, so any inter-sample over forces
  // the corresponding base sample down. The base-rate peak must therefore be
  // pulled below the ceiling even though gain is applied at the input rate.
  TruePeakLimiter limiter({-6.0f, 0.0f, 0.0f, 4, /*apply_gain_at_input_rate=*/true});
  limiter.prepare(48000.0, 64);

  // Alternating extreme samples create strong inter-sample peaks between bases.
  std::vector<float> signal = {0.0f, 1.2f, -1.2f, 1.1f, -1.1f, 0.0f};
  process(limiter, signal);

  REQUIRE(peak_abs(signal) <= 0.502f);
  REQUIRE(limiter.last_gain_reduction_db() < -5.5f);
}

TEST_CASE("AdaptiveRelease limits peaks and adapts release", "[mastering][maximizer]") {
  AdaptiveRelease limiter({-6.0f, 0.0f, 10.0f, 120.0f});
  // Prepare for the full 2048-sample blocks processed below; process() must
  // never be handed more samples than the prepared maximum block size.
  limiter.prepare(48000.0, 2048);

  // Sustained sine -> low crest factor -> release should approach max_release_ms.
  auto signal = generate_sine_samples(1000.0f, 48000, 2048, 1.0f);
  process(limiter, signal);
  REQUIRE(peak_abs(signal) <= 0.502f);
  REQUIRE(limiter.current_crest_factor() < 2.0f);
  REQUIRE(limiter.current_release_ms() >= 100.0f);

  // Transient burst -> high crest -> release should drop toward min.
  std::vector<float> burst(2048, 0.0f);
  burst[0] = 1.0f;
  burst[512] = 1.0f;
  burst[1024] = 1.0f;
  burst[1536] = 1.0f;
  process(limiter, burst);
  REQUIRE(limiter.current_crest_factor() > 5.0f);
  REQUIRE(limiter.current_release_ms() < 60.0f);
}

TEST_CASE("AdaptiveRelease preserves lookahead state across release updates",
          "[mastering][maximizer]") {
  AdaptiveRelease limiter({-6.0f, 1.0f, 10.0f, 120.0f});
  limiter.prepare(1000.0, 1);

  std::vector<float> first = {1.0f};
  process(limiter, first);
  REQUIRE_THAT(first[0], WithinAbs(0.0f, 0.0001f));

  std::vector<float> second = {0.0f};
  process(limiter, second);

  REQUIRE(second[0] > 0.2f);
  REQUIRE(second[0] <= 0.502f);
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
  REQUIRE(metering::true_peak_db(result.audio, 4) <= -0.99f);
}

TEST_CASE("LoudnessOptimize caps gain when target would exceed ceiling", "[mastering][maximizer]") {
  const Audio input = sine_audio(0.8f);
  const auto result = loudness_optimize(input, {0.0f, -6.0f, 4});

  REQUIRE(result.applied_gain_db < 0.0f);
  REQUIRE(metering::true_peak_db(result.audio, 4) <= -5.99f);
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
  REQUIRE_THROWS(AdaptiveRelease({-1.0f, 1.0f, 20.0f, 100.0f, 30.0f, 2.0f, 10.0f, -1.0f}));
  REQUIRE_THROWS(SoftKneeMax({0.0f, -1.0f, -1.0f, 10.0f}));

  Maximizer unprepared;
  std::vector<float> signal(4, 0.0f);
  float* channels[] = {signal.data()};
  REQUIRE_THROWS(unprepared.process(channels, 1, 4));

  const Audio empty;
  REQUIRE_THROWS(loudness_optimize(empty));
  REQUIRE_THROWS(streaming_preview(empty));
}
