#include "mastering/maximizer/maximizer.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "mastering/api/internal_processor_runner.h"
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

class OfflineChunkTrackingProcessor final : public rt::ProcessorBase {
 public:
  void prepare(double, int max_block_size) override {
    largest_prepared_block = std::max(largest_prepared_block, max_block_size);
  }
  void prepare(double sample_rate, int max_block_size, int max_channels) override {
    largest_requested_channels = std::max(largest_requested_channels, max_channels);
    prepare(sample_rate, max_block_size);
  }
  void process(float* const*, int num_channels, int num_samples) override {
    largest_processed_block = std::max(largest_processed_block, num_samples);
    largest_processed_channels = std::max(largest_processed_channels, num_channels);
  }
  void reset() override {}

  int largest_prepared_block = 0;
  int largest_processed_block = 0;
  int largest_requested_channels = 0;
  int largest_processed_channels = 0;
};

}  // namespace

TEST_CASE("offline processor runner bounds work blocks and passes channel capacity",
          "[mastering][maximizer]") {
  using sonare::mastering::api::internal::kOfflineProcessorBlockSize;
  constexpr int kSamples = 2 * kOfflineProcessorBlockSize + 17;

  std::vector<float> mono(static_cast<size_t>(kSamples), 0.25f);
  OfflineChunkTrackingProcessor mono_processor;
  sonare::mastering::api::internal::run_processor_mono(mono_processor, mono, 48000);
  REQUIRE(mono_processor.largest_prepared_block == kOfflineProcessorBlockSize);
  REQUIRE(mono_processor.largest_processed_block == kOfflineProcessorBlockSize);
  REQUIRE(mono_processor.largest_requested_channels == 1);
  REQUIRE(mono_processor.largest_processed_channels == 1);

  std::vector<float> left(static_cast<size_t>(kSamples), 0.25f);
  std::vector<float> right(static_cast<size_t>(kSamples), -0.25f);
  OfflineChunkTrackingProcessor stereo_processor;
  sonare::mastering::api::internal::run_processor_stereo(stereo_processor, left, right, 48000);
  REQUIRE(stereo_processor.largest_prepared_block == kOfflineProcessorBlockSize);
  REQUIRE(stereo_processor.largest_processed_block == kOfflineProcessorBlockSize);
  REQUIRE(stereo_processor.largest_requested_channels == 2);
  REQUIRE(stereo_processor.largest_processed_channels == 2);
}

TEST_CASE("TruePeakLimiter supports bounded offline mono and stereo scratch capacity",
          "[mastering][maximizer]") {
  using sonare::mastering::api::internal::kOfflineProcessorBlockSize;
  TruePeakLimiter limiter({-1.0f, 1.0f, 20.0f, 4});
  limiter.prepare(48000.0, kOfflineProcessorBlockSize, 2);

  std::vector<float> left(kOfflineProcessorBlockSize, 0.25f);
  std::vector<float> right(kOfflineProcessorBlockSize, -0.25f);
  float* stereo[] = {left.data(), right.data()};
  REQUIRE_NOTHROW(limiter.process(stereo, 2, kOfflineProcessorBlockSize));

  float* too_many_channels[] = {left.data(), right.data(), left.data()};
  REQUIRE_THROWS_AS(limiter.process(too_many_channels, 3, 1), SonareException);
}

TEST_CASE("TruePeakLimiter retains program gain reduction across a silent drain block",
          "[mastering][maximizer]") {
  TruePeakLimiter limiter({-12.0f, 1.0f, 20.0f, 4});
  limiter.prepare(48000.0, 64, 1);
  std::vector<float> hot(64, 2.0f);
  float* channels[] = {hot.data()};
  limiter.process(channels, 1, 64);
  const float program_reduction = limiter.minimum_gain_reduction_db();
  REQUIRE(program_reduction < -1.0f);

  std::vector<float> silence(64, 0.0f);
  channels[0] = silence.data();
  limiter.process(channels, 1, 64);
  REQUIRE(limiter.minimum_gain_reduction_db() <= program_reduction);

  limiter.prepare(48000.0, 64, 1);
  REQUIRE(limiter.minimum_gain_reduction_db() == 0.0f);
}

TEST_CASE("TruePeakLimiter processes a three-minute stereo master within bounded work buffers",
          "[.][slow][memory][mastering][maximizer]") {
  constexpr int kSampleRate = 44100;
  constexpr int kDurationSeconds = 3 * 60;
  constexpr int kSamples = kSampleRate * kDurationSeconds;
  std::vector<float> left = generate_sine_samples(440.0f, kSampleRate, kSamples, 0.75f);
  std::vector<float> right = generate_sine_samples(880.0f, kSampleRate, kSamples, 0.65f);

  TruePeakLimiter limiter({-1.0f, 1.0f, 50.0f, 4});
  sonare::mastering::api::internal::run_processor_stereo(limiter, left, right, kSampleRate);

  REQUIRE(left.size() == static_cast<size_t>(kSamples));
  REQUIRE(right.size() == static_cast<size_t>(kSamples));
  REQUIRE(
      std::all_of(left.begin(), left.end(), [](float sample) { return std::isfinite(sample); }));
  REQUIRE(
      std::all_of(right.begin(), right.end(), [](float sample) { return std::isfinite(sample); }));
}

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

  std::vector<float> signal(64, 1.0f);
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

  // The delayed continuous polyphase path adds one FIR group delay while
  // upsampling and another while decimating (6 + 6 at factor 4).
  REQUIRE(limiter.latency_samples() == 60);

  limiter.set_config({-1.0f, 1.0f, 10.0f, 2});
  REQUIRE(limiter.latency_samples() == 60);
  REQUIRE_THROWS(TruePeakLimiter({-1.0f, 1.0f, 10.0f, 3}));
}

TEST_CASE("TruePeakLimiter accepts meter-supported oversample factors", "[mastering][maximizer]") {
  for (int factor : {1, 2, 4, 8, 16}) {
    TruePeakLimiter limiter({-1.0f, 0.0f, 10.0f, factor});
    limiter.prepare(48000.0, 64);
    auto signal = generate_sine_samples(1000.0f, 48000, 64, 0.25f);
    float* channels[] = {signal.data()};
    REQUIRE_NOTHROW(limiter.process(channels, 1, static_cast<int>(signal.size())));
    REQUIRE(std::all_of(signal.begin(), signal.end(),
                        [](float sample) { return std::isfinite(sample); }));
  }
}

TEST_CASE("TruePeakLimiter rejects blocks larger than prepared capacity",
          "[mastering][maximizer]") {
  for (bool detect_only : {false, true}) {
    TruePeakLimiter limiter({-1.0f, 0.0f, 10.0f, 4, detect_only});
    limiter.prepare(48000.0, 64);
    std::vector<float> signal(65, 0.0f);
    float* channels[] = {signal.data()};
    REQUIRE_THROWS_AS(limiter.process(channels, 1, static_cast<int>(signal.size())),
                      SonareException);
  }
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
  REQUIRE(limiter.latency_samples() == 108);

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

TEST_CASE("TruePeakLimiter release time constant is independent of oversample factor",
          "[mastering][maximizer]") {
  // Regression: the gain-smoother loop advances once per OVERSAMPLED sample, so
  // its release time constant must be converted to a one-pole coefficient at the
  // oversampled rate (base rate * oversample factor). Computing it at the base
  // rate made the release run `oversample_factor` times too fast, so the
  // recovery slope in BASE samples depended on the oversample factor (e.g. the
  // factor-8 limiter released ~4x faster than the factor-2 limiter). With the
  // fix the base-sample recovery is essentially factor-independent.
  const int sample_rate = 48000;
  const float ceiling_db = -6.0f;
  const float release_ms = 50.0f;

  auto recovery_samples = [&](int factor) {
    TruePeakLimiter limiter({ceiling_db, 0.0f, release_ms, factor});
    limiter.prepare(static_cast<double>(sample_rate), 1);
    // Drive the gain hard down with a short loud burst, one base sample per
    // block so last_gain_reduction_db() tracks the base-rate gain envelope.
    for (int i = 0; i < 8; ++i) {
      float sample = 4.0f;
      float* channel[] = {&sample};
      limiter.process(channel, 1, 1);
    }
    const float start_gr = limiter.last_gain_reduction_db();
    REQUIRE(start_gr < -3.0f);
    // Feed a long sub-ceiling tail one base sample at a time and count the base
    // samples until the gain reduction recovers halfway (in dB) back toward 0.
    const float half = start_gr * 0.5f;
    int crossing = -1;
    for (int i = 0; i < sample_rate; ++i) {
      float sample = 0.1f;
      float* channel[] = {&sample};
      limiter.process(channel, 1, 1);
      if (limiter.last_gain_reduction_db() >= half) {
        crossing = i;
        break;
      }
    }
    REQUIRE(crossing > 0);
    return crossing;
  };

  const int r2 = recovery_samples(2);
  const int r4 = recovery_samples(4);
  const int r8 = recovery_samples(8);
  const int lo = std::min({r2, r4, r8});
  const int hi = std::max({r2, r4, r8});
  // A one-pole release half-life is amplitude-independent, so with the fix the
  // three factors recover in an essentially identical number of base samples.
  REQUIRE(static_cast<float>(hi) <= 1.2f * static_cast<float>(lo));
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
  // Prepare for the largest block processed below; process() must never be
  // handed more samples than the prepared maximum block size.
  constexpr int kBurstSamples = 8192;
  limiter.prepare(48000.0, kBurstSamples);

  // Sustained sine -> low crest factor -> release should approach max_release_ms.
  auto signal = generate_sine_samples(1000.0f, 48000, 2048, 1.0f);
  process(limiter, signal);
  REQUIRE(peak_abs(signal) <= 0.502f);
  REQUIRE(limiter.current_crest_factor() < 2.0f);
  REQUIRE(limiter.current_release_ms() >= 100.0f);

  // Transient burst -> high crest -> release should drop toward min. The crest
  // detector is a sliding window (crest_window_ms), not a per-block statistic,
  // so the burst has to outlast both that window and the release smoothing
  // before the running RMS reflects the new material rather than the sine
  // that came before it.
  std::vector<float> burst(kBurstSamples, 0.0f);
  for (int i = 0; i < kBurstSamples; i += 512) {
    burst[static_cast<std::size_t>(i)] = 1.0f;
  }
  process(limiter, burst);
  REQUIRE(limiter.current_crest_factor() > 5.0f);
  REQUIRE(limiter.current_release_ms() < 60.0f);
}

namespace {

// Sustained low tone under periodic transients, driven hard enough that the
// limiter under the adaptive release is working continuously.
std::vector<float> adaptive_release_program(int samples, int sample_rate) {
  std::vector<float> out(static_cast<std::size_t>(samples), 0.0f);
  const int period = sample_rate / 8;
  for (int i = 0; i < samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    float value = 0.35f * std::sin(2.0f * sonare::constants::kPi * 110.0f * t);
    const int local = i % period;
    if (local < 96) {
      value += 1.2f * (1.0f - static_cast<float>(local) / 96.0f);
    }
    out[static_cast<std::size_t>(i)] = value;
  }
  return out;
}

// Renders @p input through one AdaptiveRelease instance, handing it @p blocks
// samples at a time (cycling through the list; the last chunk is what is left).
std::vector<float> run_adaptive_release(const std::vector<float>& input,
                                        const AdaptiveReleaseConfig& config,
                                        const std::vector<int>& blocks, int prepared_block,
                                        int sample_rate) {
  std::vector<float> output = input;
  AdaptiveRelease processor(config);
  processor.prepare(sample_rate, prepared_block);
  const int total = static_cast<int>(output.size());
  int offset = 0;
  std::size_t index = 0;
  while (offset < total) {
    const int count = std::min(blocks[index % blocks.size()], total - offset);
    float* channels[] = {output.data() + offset};
    processor.process(channels, 1, count);
    offset += count;
    ++index;
  }
  return output;
}

float max_abs_difference(const std::vector<float>& a, const std::vector<float>& b) {
  REQUIRE(a.size() == b.size());
  float worst = 0.0f;
  for (std::size_t i = 0; i < a.size(); ++i) {
    worst = std::max(worst, std::abs(a[i] - b[i]));
  }
  return worst;
}

}  // namespace

// A host's buffer size is an arbitrary choice: a 0.5 s preview rendered in
// 512-sample callbacks and the same material rendered in one offline block must
// come out identical. Every envelope AdaptiveRelease keeps (crest peak, running
// RMS, release smoothing) advances per sample at a rate fixed by the sample
// rate and the configured milliseconds, and the release it hands the inner
// limiter is published on a grid anchored to the stream position, so nothing
// here may be derived from num_samples.
TEST_CASE("AdaptiveRelease output does not depend on the caller's block size",
          "[mastering][maximizer][block_invariance]") {
  using sonare::mastering::api::internal::kOfflineProcessorBlockSize;
  constexpr int kSampleRate = 48000;
  // One whole offline block plus a ragged tail, so the reference render also
  // exercises a short final chunk.
  const int total = kOfflineProcessorBlockSize + 1024;
  const auto input = adaptive_release_program(total, kSampleRate);
  const AdaptiveReleaseConfig config;  // defaults: 30 ms crest, 20 ms smoothing

  const auto offline = run_adaptive_release(input, config, {kOfflineProcessorBlockSize},
                                            kOfflineProcessorBlockSize, kSampleRate);

  // A typical host callback, prepared at its own block size the way a live
  // host would.
  CHECK(max_abs_difference(run_adaptive_release(input, config, {512}, 512, kSampleRate), offline) ==
        0.0f);
  // A ragged split that lands mid-transient and inside the control grid.
  CHECK(max_abs_difference(
            run_adaptive_release(input, config, {17, 31, 64, 512, 3000}, 4096, kSampleRate),
            offline) == 0.0f);
}

// crestWindowMs and releaseSmoothingMs are published, automatable parameters.
// Both set the speed of a per-sample envelope, so both must still shape the
// render when the whole signal arrives as one offline block. Deriving their
// coefficients from num_samples instead made the exponent underflow (a 65536
// sample block against a 30 ms window is exp(-45)), pinning both rates at
// "instantaneous" and rendering the two knobs bit-for-bit inert.
TEST_CASE("AdaptiveRelease crest window and release smoothing shape a single-block render",
          "[mastering][maximizer][block_invariance]") {
  constexpr int kSampleRate = 48000;
  constexpr int kTotal = 32768;  // one block, far longer than either time constant
  const auto input = adaptive_release_program(kTotal, kSampleRate);

  auto render = [&](float crest_window_ms, float release_smoothing_ms) {
    AdaptiveReleaseConfig config;
    config.crest_window_ms = crest_window_ms;
    config.release_smoothing_ms = release_smoothing_ms;
    return run_adaptive_release(input, config, {kTotal}, kTotal, kSampleRate);
  };

  const AdaptiveReleaseConfig defaults;
  const auto reference = render(defaults.crest_window_ms, defaults.release_smoothing_ms);
  const float crest_delta =
      max_abs_difference(render(1.0f, defaults.release_smoothing_ms), reference);
  const float smoothing_delta =
      max_abs_difference(render(defaults.crest_window_ms, 0.0f), reference);
  CAPTURE(crest_delta, smoothing_delta);
  CHECK(crest_delta > 1.0e-3f);
  CHECK(smoothing_delta > 1.0e-3f);
}

TEST_CASE("AdaptiveRelease preserves lookahead state across release updates",
          "[mastering][maximizer]") {
  AdaptiveRelease limiter({-6.0f, 1.0f, 10.0f, 120.0f});
  limiter.prepare(1000.0, 1);

  std::vector<float> first = {1.0f};
  process(limiter, first);
  REQUIRE_THAT(first[0], WithinAbs(0.0f, 0.0001f));

  float released_sample = 0.0f;
  for (int i = 0; i < limiter.latency_samples(); ++i) {
    std::vector<float> block = {0.0f};
    process(limiter, block);
    released_sample = std::max(released_sample, block[0]);
  }

  REQUIRE(released_sample > 0.2f);
  REQUIRE(released_sample <= 0.502f);
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

TEST_CASE("LoudnessOptimize drives its limiter toward a target the headroom cannot reach",
          "[mastering][maximizer]") {
  // A 0 LUFS target at a -6 dB ceiling asks for more gain than the peak headroom
  // has: the limiter is what closes the distance, so the static gain goes up and
  // the ceiling is enforced afterwards.
  const Audio input = sine_audio(0.8f);
  LoudnessOptimizeConfig config;
  config.target_lufs = 0.0f;
  config.ceiling_db = -6.0f;
  const auto result = loudness_optimize(input, config);

  REQUIRE(result.applied_gain_db > 0.0f);
  // Never more than the requested target - input gain, whatever the allowance.
  REQUIRE(result.applied_gain_db <= config.target_lufs - result.input_lufs + 1e-3f);
  REQUIRE(metering::true_peak_db(result.audio, 4) <= -5.99f);
  // A steady sine cannot be limited into loudness, so the target is missed and
  // the result has to say so.
  REQUIRE(result.loudness_target_limited);
}

TEST_CASE("LoudnessOptimize clamps to the bare headroom at zero limiter allowance",
          "[mastering][maximizer]") {
  const Audio input = sine_audio(0.8f);
  LoudnessOptimizeConfig config;
  config.target_lufs = 0.0f;
  config.ceiling_db = -6.0f;
  config.max_limiter_gain_reduction_db = 0.0f;
  const auto result = loudness_optimize(input, config);

  REQUIRE(result.applied_gain_db < 0.0f);
  REQUIRE(metering::true_peak_db(result.audio, 4) <= -5.99f);
  REQUIRE(result.loudness_target_limited);
}

TEST_CASE("LoudnessOptimize rejects a negative limiter allowance", "[mastering][maximizer]") {
  LoudnessOptimizeConfig config;
  config.max_limiter_gain_reduction_db = -1.0f;
  REQUIRE_THROWS(loudness_optimize(sine_audio(0.5f), config));
}

TEST_CASE("LoudnessOptimize returns time-aligned output with zero reported latency",
          "[mastering][maximizer]") {
  // The helper pads by the internal true-peak limiter's look-ahead, processes,
  // and drops the leading delayed samples, so the returned audio is already
  // time-aligned. It must therefore report zero latency (not the internal
  // limiter latency, which would make a caller double-compensate an already
  // aligned buffer).
  const Audio input = sine_audio(0.05f);
  const auto result = loudness_optimize(input, {-20.0f, -1.0f, 4});

  REQUIRE(result.audio.size() == input.size());
  REQUIRE(result.latency_samples == 0);
}

TEST_CASE("LoudnessOptimize accepts meter-supported true-peak oversample factors",
          "[mastering][maximizer]") {
  const Audio input = sine_audio(0.05f, 48000, 0.1f);
  for (int factor : {1, 2, 4, 8, 16}) {
    const auto result = loudness_optimize(input, {-20.0f, -1.0f, factor});
    REQUIRE(result.audio.size() == input.size());
    REQUIRE(std::isfinite(result.output_lufs));
  }
}

TEST_CASE("LoudnessOptimize rejects unsupported true-peak oversample factors",
          "[mastering][maximizer]") {
  const Audio input = sine_audio(0.05f, 48000, 0.1f);
  for (int factor : {0, 3, 5, 6, 7, 32}) {
    REQUIRE_THROWS(loudness_optimize(input, {-20.0f, -1.0f, factor}));
  }
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

TEST_CASE("StreamingPreview measures a stereo pair with channel summing",
          "[mastering][maximizer]") {
  constexpr int sample_rate = 48000;
  constexpr int frames = sample_rate * 4;
  const std::vector<float> left = generate_sine_samples(1000.0f, sample_rate, frames, 0.2f);
  const std::vector<float> right = generate_sine_samples(1731.0f, sample_rate, frames, 0.2f);

  std::vector<float> interleaved(static_cast<size_t>(frames) * 2);
  std::vector<float> downmix(static_cast<size_t>(frames));
  for (size_t index = 0; index < static_cast<size_t>(frames); ++index) {
    interleaved[2 * index] = left[index];
    interleaved[2 * index + 1] = right[index];
    downmix[index] = 0.5f * (left[index] + right[index]);
  }

  const std::vector<StreamingPlatform> platforms = {{"Test", -14.0f, -1.0f}};
  const auto stereo =
      streaming_preview_interleaved(interleaved.data(), frames, 2, sample_rate, platforms);
  const auto mono = streaming_preview(Audio::from_vector(downmix, sample_rate), platforms);

  const float expected =
      metering::lufs_interleaved(interleaved.data(), frames, 2, sample_rate).integrated_lufs;
  REQUIRE_THAT(stereo[0].integrated_lufs, WithinAbs(expected, 0.01f));

  // BS.1770 sums the channel powers while the 0.5*(L+R) downmix a mono caller
  // has to build quarters them, so a decorrelated pair reads 6.02 dB low. The
  // normalization gain is target minus that measurement, so the error passes
  // straight through to the gain and to the ceiling-risk verdict built on it.
  REQUIRE_THAT(stereo[0].integrated_lufs - mono[0].integrated_lufs, WithinAbs(6.02f, 0.3f));
  REQUIRE_THAT(stereo[0].normalization_gain_db,
               WithinAbs(-14.0f - stereo[0].integrated_lufs, 0.001f));
  REQUIRE_THAT(mono[0].normalization_gain_db - stereo[0].normalization_gain_db,
               WithinAbs(6.02f, 0.3f));
}

TEST_CASE("StreamingPreview keeps a measurement where a downmix cancels",
          "[mastering][maximizer]") {
  constexpr int sample_rate = 48000;
  constexpr int frames = sample_rate * 4;
  const std::vector<float> left = generate_sine_samples(1000.0f, sample_rate, frames, 0.5f);

  // Anti-phase pair: the downmix is silence, so the mono path falls below the
  // absolute gate and reports a non-finite loudness with a zeroed gain, while
  // the program itself is loud.
  std::vector<float> interleaved(static_cast<size_t>(frames) * 2);
  std::vector<float> downmix(static_cast<size_t>(frames));
  for (size_t index = 0; index < static_cast<size_t>(frames); ++index) {
    interleaved[2 * index] = left[index];
    interleaved[2 * index + 1] = -left[index];
    downmix[index] = 0.5f * (left[index] - left[index]);
  }

  const std::vector<StreamingPlatform> platforms = {{"Test", -14.0f, -1.0f}};
  const auto stereo =
      streaming_preview_interleaved(interleaved.data(), frames, 2, sample_rate, platforms);
  const auto mono = streaming_preview(Audio::from_vector(downmix, sample_rate), platforms);

  REQUIRE(!std::isfinite(mono[0].integrated_lufs));
  REQUIRE(mono[0].normalization_gain_db == 0.0f);
  REQUIRE(std::isfinite(stereo[0].integrated_lufs));
  REQUIRE_THAT(
      stereo[0].integrated_lufs,
      WithinAbs(
          metering::lufs_interleaved(interleaved.data(), frames, 2, sample_rate).integrated_lufs,
          0.01f));
  REQUIRE(stereo[0].normalization_gain_db != 0.0f);
}

TEST_CASE("StreamingPreview rejects invalid interleaved input", "[mastering][maximizer]") {
  const std::vector<float> samples(128, 0.1f);
  REQUIRE_THROWS(streaming_preview_interleaved(nullptr, 64, 2, 48000));
  REQUIRE_THROWS(streaming_preview_interleaved(samples.data(), 0, 2, 48000));
  REQUIRE_THROWS(streaming_preview_interleaved(samples.data(), 64, 0, 48000));
  REQUIRE_THROWS(streaming_preview_interleaved(samples.data(), 64, 2, 0));
  REQUIRE_THROWS(streaming_preview_interleaved(samples.data(), 64, 2, 48000, {}));
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
