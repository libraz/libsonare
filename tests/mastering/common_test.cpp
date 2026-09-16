#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "mastering/api/internal_processor_runner.h"
#include "mastering/common/loudness_measure.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/master.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "metering/lufs.h"
#include "rt/adaa.h"
#include "rt/delay_line.h"
#include "rt/envelope_follower.h"
#include "rt/lookahead_buffer.h"
#include "rt/nonlinearities.h"
#include "rt/oversampler.h"
#include "rt/param_smoother.h"
#include "rt/partitioned_convolver.h"
#include "rt/processor_base.h"
#include "rt/processor_chain.h"
#include "rt/scoped_no_denormals.h"
#include "rt/sliding_max.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"
#include "util/lpc.h"

using Catch::Matchers::WithinAbs;
using sonare::ar_interpolate_region;
using sonare::ArInterpolateParams;
using sonare::interpolate_gap;
using sonare::lpc_autocorrelation;
using sonare::lpc_burg;
using sonare::lpc_residual;
using sonare::LpcResult;
using sonare::mastering::common::NoiseTracker;
// rt:: primitives were previously aliased into sonare::mastering::common::.
// This test exercises them directly; pull the full namespace into scope so
// the test bodies remain readable.
using namespace sonare::rt;  // NOLINT(google-build-using-namespace)

namespace {
using sonare::test::rms;

class NoLatencyProcessor : public ProcessorBase {
 public:
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
};

class FixedLatencyProcessor : public NoLatencyProcessor {
 public:
  explicit FixedLatencyProcessor(int latency) : latency_(latency) {}
  int latency_samples() const noexcept override { return latency_; }

 private:
  int latency_ = 0;
};

}  // namespace

TEST_CASE("Mastering umbrella header exposes representative modules", "[mastering]") {
  sonare::mastering::eq::ParametricEq eq;
  sonare::mastering::dynamics::Compressor compressor;
  sonare::mastering::maximizer::Maximizer maximizer;

  eq.prepare(48000.0, 16);
  compressor.prepare(48000.0, 16);
  maximizer.prepare(48000.0, 16);

  REQUIRE(eq.sample_rate() == 48000.0);
  REQUIRE(compressor.config().ratio == 2.0f);
  REQUIRE(maximizer.config().ceiling_db == -1.0f);
}

// There is no mastering::common::* alias layer over rt::*: rt primitives are
// included directly as "rt/...", and the cases below exercise them under those
// names. What this file covers from common/ is NoiseTracker and
// loudness_measure. Biquad and the Jiles-Atherton hysteresis model are covered
// by the eq_* and hysteresis_* files, not here.

TEST_CASE("ParamSmoother reaches target immediately with zero time", "[mastering]") {
  ParamSmoother smoother(0.0f, 0.0f, 48000.0);

  smoother.set_target(1.0f);

  REQUIRE_THAT(smoother.process(), WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(smoother.current(), WithinAbs(1.0f, 0.0001f));
}

TEST_CASE("ParamSmoother ignores non-finite updates and accepts later finite targets",
          "[mastering][validation]") {
  ParamSmoother smoother(0.25f, 0.0f, 48000.0);
  smoother.set_target(std::numeric_limits<float>::quiet_NaN());
  REQUIRE_THAT(smoother.process(), WithinAbs(0.25f, 0.0001f));
  smoother.reset(std::numeric_limits<float>::infinity());
  REQUIRE_THAT(smoother.current(), WithinAbs(0.25f, 0.0001f));

  smoother.set_target(0.75f);
  REQUIRE_THAT(smoother.process(), WithinAbs(0.75f, 0.0001f));
}

TEST_CASE("ParamSmoother approaches target monotonically", "[mastering]") {
  ParamSmoother smoother(0.0f, 10.0f, 48000.0);

  smoother.set_target(1.0f);
  const float first = smoother.process();
  const float second = smoother.process();

  REQUIRE(first > 0.0f);
  REQUIRE(second > first);
  REQUIRE(second < 1.0f);
}

TEST_CASE("EnvelopeFollower follows attack and release", "[mastering]") {
  EnvelopeFollower follower;
  follower.prepare(1000.0, 0.0f, 100.0f);

  REQUIRE_THAT(follower.process(1.0f), WithinAbs(1.0f, 0.0001f));

  const float released = follower.process(0.0f);
  REQUIRE(released > 0.0f);
  REQUIRE(released < 1.0f);
}

TEST_CASE("EnvelopeFollower smooths bidirectional targets", "[mastering]") {
  EnvelopeFollower follower;
  follower.prepare(1000.0, 0.0f, 100.0f);
  follower.reset(0.0f);

  REQUIRE_THAT(follower.smooth_bidirectional(-6.0f), WithinAbs(-6.0f, 0.0001f));

  const float released = follower.smooth_bidirectional(0.0f);
  REQUIRE(released > -6.0f);
  REQUIRE(released < 0.0f);
}

TEST_CASE("DelayLine delays by configured samples", "[mastering]") {
  DelayLine delay;
  delay.prepare(2);

  REQUIRE_THAT(delay.process(1.0f), WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(delay.process(2.0f), WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(delay.process(3.0f), WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(delay.process(4.0f), WithinAbs(2.0f, 0.0001f));
}

TEST_CASE("DelayLine zero delay returns input immediately", "[mastering]") {
  DelayLine delay;
  delay.prepare(0);

  REQUIRE_THAT(delay.process(1.0f), WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(delay.process(-0.5f), WithinAbs(-0.5f, 0.0001f));
}

TEST_CASE("ProcessorBase reports zero latency by default", "[mastering]") {
  NoLatencyProcessor processor;

  REQUIRE(processor.latency_samples() == 0);
  REQUIRE(processor.latency_samples_q8() == 0);
}

TEST_CASE("ScopedNoDenormals is constructible as a block-scope guard", "[mastering]") {
  ScopedNoDenormals guard;

  SUCCEED();
}

TEST_CASE("Processor chain sums reported latency", "[mastering]") {
  FixedLatencyProcessor first(32);
  FixedLatencyProcessor second(7);
  NoLatencyProcessor zero;

  REQUIRE(total_latency_samples({&first, &second, &zero}) == 39);
  REQUIRE(total_latency_samples_q8({&first, &second, &zero}) == (39 << 8));
  REQUIRE_THROWS(total_latency_samples({&first, nullptr}));
  REQUIRE_THROWS(total_latency_samples_q8({&first, nullptr}));
}

TEST_CASE("Adaa1 falls back to direct nonlinearity for repeated samples", "[mastering]") {
  Adaa1<TanhNonlinearity> adaa;

  (void)adaa.process(0.5f);
  REQUIRE_THAT(adaa.process(0.5f), WithinAbs(std::tanh(0.5f), 0.0001f));
  REQUIRE(adaa.latency_samples() == 0);
  REQUIRE(adaa.latency_samples_q8() == 128);
}

TEST_CASE("Adaa1 hard clip averages across discontinuity", "[mastering]") {
  Adaa1<HardClipNonlinearity> adaa({1.0f});
  adaa.reset(0.0f);

  REQUIRE_THAT(adaa.process(2.0f), WithinAbs(0.75f, 0.0001f));
}

TEST_CASE("Nonlinearity antiderivatives match local slopes", "[mastering]") {
  const ArctanNonlinearity arctan;
  const CubicSoftClipNonlinearity cubic;
  constexpr float x = 0.25f;
  constexpr float dx = 0.0001f;

  const float arctan_slope =
      (arctan.antiderivative(x + dx) - arctan.antiderivative(x - dx)) / (2.0f * dx);
  const float cubic_slope =
      (cubic.antiderivative(x + dx) - cubic.antiderivative(x - dx)) / (2.0f * dx);

  REQUIRE_THAT(arctan_slope, WithinAbs(arctan.apply(x), 0.001f));
  REQUIRE_THAT(cubic_slope, WithinAbs(cubic.apply(x), 0.001f));
}

TEST_CASE("SlidingMax tracks maximum over a moving window", "[mastering]") {
  SlidingMax<float> sliding_max(3);

  sliding_max.push(1.0f);
  REQUIRE_THAT(sliding_max.max(), WithinAbs(1.0f, 0.0001f));

  sliding_max.push(3.0f);
  REQUIRE_THAT(sliding_max.max(), WithinAbs(3.0f, 0.0001f));

  sliding_max.push(2.0f);
  REQUIRE_THAT(sliding_max.max(), WithinAbs(3.0f, 0.0001f));

  sliding_max.push(0.0f);
  REQUIRE_THAT(sliding_max.max(), WithinAbs(3.0f, 0.0001f));

  sliding_max.push(-1.0f);
  REQUIRE_THAT(sliding_max.max(), WithinAbs(2.0f, 0.0001f));
}

TEST_CASE("LookaheadBuffer exposes peak while delaying output", "[mastering]") {
  LookaheadBuffer buffer;
  buffer.prepare(2);

  REQUIRE_THAT(buffer.process(0.25f), WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(buffer.peak(), WithinAbs(0.25f, 0.0001f));

  REQUIRE_THAT(buffer.process(-1.0f), WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(buffer.peak(), WithinAbs(1.0f, 0.0001f));

  REQUIRE_THAT(buffer.process(0.5f), WithinAbs(0.25f, 0.0001f));
  REQUIRE_THAT(buffer.peak(), WithinAbs(1.0f, 0.0001f));
}

TEST_CASE("LookaheadBuffer drops expired peak values", "[mastering]") {
  LookaheadBuffer buffer;
  buffer.prepare(2);

  REQUIRE_THAT(buffer.process(1.0f), WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(buffer.peak(), WithinAbs(1.0f, 0.0001f));

  REQUIRE_THAT(buffer.process(0.5f), WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(buffer.peak(), WithinAbs(1.0f, 0.0001f));

  REQUIRE_THAT(buffer.process(0.25f), WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(buffer.peak(), WithinAbs(1.0f, 0.0001f));

  REQUIRE_THAT(buffer.process(0.125f), WithinAbs(0.5f, 0.0001f));
  REQUIRE_THAT(buffer.peak(), WithinAbs(0.5f, 0.0001f));
}

TEST_CASE("Oversampler filters every phase while interpolating intermediate samples",
          "[mastering]") {
  Oversampler oversampler(4);
  std::vector<float> input(64, 0.0f);
  input[32] = 1.0f;

  const auto upsampled = oversampler.upsample(input);

  REQUIRE(upsampled.size() == input.size() * 4);
  REQUIRE(std::abs(upsampled[32 * 4]) > 0.9f);
  REQUIRE(std::abs(upsampled[32 * 4]) < 1.0f);
  REQUIRE(std::abs(upsampled[32 * 4 + 1]) > 0.01f);
  REQUIRE(std::abs(upsampled[32 * 4 + 2]) > 0.01f);
  REQUIRE(std::abs(upsampled[32 * 4 + 3]) > 0.01f);
  REQUIRE(oversampler.latency_samples() == 6);
}

TEST_CASE("Oversampler round trip preserves a mid-band sine", "[mastering]") {
  constexpr double kSampleRate = 48000.0;
  constexpr double kFrequency = 6000.0;
  constexpr size_t kLength = 4096;
  constexpr size_t kEdge = 32;
  Oversampler oversampler(4);
  std::vector<float> input(kLength, 0.0f);
  for (size_t i = 0; i < input.size(); ++i) {
    input[i] = 0.5f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * kFrequency *
                                                  static_cast<double>(i) / kSampleRate));
  }

  const auto round_trip = oversampler.downsample(oversampler.upsample(input));
  float max_error = 0.0f;
  for (size_t i = kEdge; i < input.size() - kEdge; ++i) {
    max_error = std::max(max_error, std::abs(round_trip[i] - input[i]));
  }

  CAPTURE(max_error);
  REQUIRE(max_error < 1.0e-3f);
}

TEST_CASE("Oversampler downsample uses FIR decimation", "[mastering]") {
  Oversampler oversampler(8);
  std::vector<float> low_rate(256, 0.0f);
  for (size_t i = 0; i < low_rate.size(); ++i) {
    low_rate[i] = 0.4f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * 250.0 *
                                                     static_cast<double>(i) / 48000.0));
  }
  const auto upsampled = oversampler.upsample(low_rate);
  const auto round_trip = oversampler.downsample(upsampled);

  REQUIRE(round_trip.size() == low_rate.size());
  REQUIRE(rms(round_trip, 32) > rms(low_rate, 32) * 0.75f);

  std::vector<float> high_rate_noise(1024, 0.0f);
  for (size_t i = 0; i < high_rate_noise.size(); ++i) {
    high_rate_noise[i] = (i % 2 == 0) ? 1.0f : -1.0f;
  }
  const auto rejected = oversampler.downsample(high_rate_noise);

  REQUIRE(rms(rejected, 16) < 0.05f);
}

TEST_CASE("Oversampler streaming round trip is invariant to block partitioning", "[mastering]") {
  constexpr size_t kFrames = 16384;
  Oversampler oversampler(4);
  std::vector<float> input(
      kFrames + static_cast<size_t>(oversampler.streaming_round_trip_latency_samples()), 0.0f);
  for (size_t i = 0; i < kFrames; ++i) {
    input[i] = 0.7f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * 753.0 *
                                                  static_cast<double>(i) / 48000.0));
  }

  const auto process_in_blocks = [&](size_t block_size) {
    Oversampler::StreamingState state;
    oversampler.prepare_streaming(&state, block_size);
    std::vector<float> output;
    output.reserve(input.size());
    std::vector<float> up(block_size * static_cast<size_t>(oversampler.factor()));
    std::vector<float> down(block_size);
    for (size_t offset = 0; offset < input.size(); offset += block_size) {
      const size_t count = std::min(block_size, input.size() - offset);
      oversampler.upsample_to_streaming(input.data() + offset, count, up.data(), up.size(), &state);
      for (size_t i = 0; i < count * static_cast<size_t>(oversampler.factor()); ++i) {
        up[i] = std::tanh(up[i] * 1.3f);
      }
      oversampler.downsample_to_streaming(up.data(),
                                          count * static_cast<size_t>(oversampler.factor()),
                                          down.data(), down.size(), &state);
      output.insert(output.end(), down.begin(), down.begin() + static_cast<std::ptrdiff_t>(count));
    }
    return output;
  };

  const auto one_block = process_in_blocks(input.size());
  CAPTURE(rms(one_block, 128), rms(input, 128));
  REQUIRE(rms(one_block, 128) > rms(input, 128) * 0.5f);
  for (const size_t block_size : {size_t{128}, size_t{1024}, size_t{4096}}) {
    const auto partitioned = process_in_blocks(block_size);
    REQUIRE(partitioned == one_block);
  }
}

TEST_CASE("Oversampler supports only power of two mastering factors", "[mastering]") {
  Oversampler oversampler(2);

  oversampler.set_factor(4);
  REQUIRE(oversampler.factor() == 4);
  oversampler.set_factor(8);
  REQUIRE(oversampler.factor() == 8);
  REQUIRE_THROWS(oversampler.set_factor(3));
}

TEST_CASE("TruePeakFilter returns sample peak and interpolated output", "[mastering]") {
  TruePeakFilter filter(1);
  std::vector<float> input = {0.0f, 0.8f, 0.0f};
  const float* channels[] = {input.data()};
  std::vector<float> output(input.size() * 4, 0.0f);
  float* output_channels[] = {output.data()};

  filter.upsample(channels, output_channels, 1, static_cast<int>(input.size()));

  REQUIRE(filter.factor() == 4);
  REQUIRE(filter.latency_samples() == 6);
  REQUIRE_THAT(filter.process(channels, 1, static_cast<int>(input.size())), WithinAbs(0.8f, 0.3f));
  REQUIRE(output[4] > 0.7f);
  REQUIRE(output[4] < 0.8f);
  REQUIRE(std::abs(output[5]) > 0.01f);

  TruePeakFilter fallback(1, 2);
  std::vector<float> output_2x(input.size() * 2, 0.0f);
  float* output_2x_channels[] = {output_2x.data()};
  fallback.upsample(channels, output_2x_channels, 1, static_cast<int>(input.size()));
  REQUIRE(fallback.factor() == 2);
  REQUIRE(fallback.latency_samples() == 6);
  REQUIRE(output_2x[2] > 0.7f);
  REQUIRE(output_2x[2] < 0.8f);
  TruePeakFilter eightx(1, 8);
  REQUIRE(eightx.factor() == 8);
  REQUIRE_THROWS(TruePeakFilter(1, 3));
}

TEST_CASE("PartitionedConvolver matches direct convolution across streaming blocks",
          "[mastering]") {
  const std::vector<float> ir = {0.5f, -0.25f, 0.125f, 0.0625f, -0.03125f, 0.015625f};
  const std::vector<float> input = {1.0f, 0.25f,  -0.5f,  0.75f, 0.0f, -0.25f,
                                    0.5f, 0.125f, -0.75f, 0.25f, 0.0f, 1.0f};

  std::vector<float> expected(input.size(), 0.0f);
  for (size_t n = 0; n < input.size(); ++n) {
    for (size_t k = 0; k < ir.size(); ++k) {
      if (n >= k) expected[n] += input[n - k] * ir[k];
    }
  }

  PartitionedConvolver convolver({4});
  convolver.set_impulse_response(ir);
  std::vector<float> actual(input.size(), 0.0f);
  for (size_t offset = 0; offset < input.size(); offset += 4) {
    convolver.process_block(input.data() + offset, actual.data() + offset);
  }

  REQUIRE(convolver.partition_size() == 4);
  REQUIRE(convolver.fft_size() == 8);
  REQUIRE(convolver.num_partitions() == 2);
  for (size_t i = 0; i < input.size(); ++i) {
    REQUIRE_THAT(actual[i], WithinAbs(expected[i], 0.0001f));
  }
}

TEST_CASE("PartitionedConvolver reset restores initial response", "[mastering]") {
  PartitionedConvolver convolver({4});
  convolver.set_impulse_response(std::vector<float>{1.0f, 0.5f});

  const std::vector<float> block = {1.0f, 2.0f, 3.0f, 4.0f};
  std::vector<float> first(4, 0.0f);
  std::vector<float> second(4, 0.0f);
  convolver.process_block(block.data(), first.data());
  convolver.reset();
  convolver.process_block(block.data(), second.data());

  for (size_t i = 0; i < block.size(); ++i) {
    REQUIRE_THAT(second[i], WithinAbs(first[i], 0.0001f));
  }
  REQUIRE_THROWS(PartitionedConvolver({0}));
}

TEST_CASE("LPC autocorrelation estimates first-order AR process", "[mastering]") {
  std::vector<float> signal(256, 0.0f);
  signal[0] = 1.0f;
  for (size_t i = 1; i < signal.size(); ++i) {
    signal[i] = 0.8f * signal[i - 1];
  }

  const auto model = lpc_autocorrelation(signal.data(), signal.size(), 1);

  REQUIRE(model.ar.size() == 2);
  REQUIRE_THAT(model.ar[0], WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(model.ar[1], WithinAbs(-0.8f, 0.02f));
  REQUIRE(model.variance >= 0.0f);
}

TEST_CASE("LPC Burg returns stable residual for predictable signal", "[mastering]") {
  std::vector<float> signal(256, 0.0f);
  signal[0] = 1.0f;
  for (size_t i = 1; i < signal.size(); ++i) {
    signal[i] = 0.6f * signal[i - 1];
  }

  const auto model = lpc_burg(signal.data(), signal.size(), 1);
  const auto baseline = lpc_burg(signal.data(), signal.size(), 0);
  const auto residual = lpc_residual(signal.data(), signal.size(), model);

  REQUIRE(model.ar.size() == 2);
  REQUIRE(model.variance < baseline.variance);
  REQUIRE(std::abs(residual.back()) < 0.0001f);
}

namespace {

/// A two-tone bed an AR model of a handful of poles predicts well, which is what
/// makes a reconstruction error attributable to the interpolation rather than to
/// the model's fit.
std::vector<float> ar_gap_bed(size_t frames) {
  std::vector<float> bed(frames);
  for (size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i);
    bed[i] = static_cast<float>(0.6 * std::sin(sonare::constants::kTwoPiD * 11.0 * t / 256.0) +
                                0.25 * std::sin(sonare::constants::kTwoPiD * 37.0 * t / 256.0));
  }
  return bed;
}

float max_abs_error(const std::vector<float>& got, const std::vector<float>& want, size_t start,
                    size_t end) {
  float worst = 0.0f;
  for (size_t i = start; i < end; ++i) worst = std::max(worst, std::abs(got[i] - want[i]));
  return worst;
}

}  // namespace

TEST_CASE("Two-sided AR interpolation beats the interpolation baseline over a gap", "[mastering]") {
  constexpr size_t kFrames = 1024;
  constexpr size_t kGapStart = 512;
  constexpr size_t kGapEnd = kGapStart + 48;
  const std::vector<float> clean = ar_gap_bed(kFrames);

  std::vector<float> baseline = clean;
  interpolate_gap(baseline.data(), baseline.size(), kGapStart, kGapEnd);

  ArInterpolateParams params;
  params.order = 48;
  std::vector<float> solved = clean;
  REQUIRE(ar_interpolate_region(solved.data(), solved.size(), kGapStart, kGapEnd, params));

  // Both fills leave everything outside the gap alone.
  for (size_t i = 0; i < kFrames; ++i) {
    if (i >= kGapStart && i < kGapEnd) continue;
    REQUIRE(baseline[i] == clean[i]);
    REQUIRE(solved[i] == clean[i]);
  }

  // The gap spans two periods of the lower tone, so the cubic / linear baseline
  // cannot follow it and the AR solve is what recovers the waveform. Measured:
  // peak error 0.990 for the baseline against 0.0017 for the solve, so each bound
  // below clears its measurement by at least a factor of five.
  const float baseline_error = max_abs_error(baseline, clean, kGapStart, kGapEnd);
  const float solved_error = max_abs_error(solved, clean, kGapStart, kGapEnd);
  CAPTURE(baseline_error, solved_error);
  REQUIRE(baseline_error > 0.5f);
  REQUIRE(solved_error < 0.01f);
  REQUIRE(solved_error < baseline_error * 0.1f);
}

TEST_CASE("AR interpolation declines a gap past its cap", "[mastering]") {
  constexpr size_t kFrames = 1024;
  const std::vector<float> clean = ar_gap_bed(kFrames);

  ArInterpolateParams params;
  params.order = 48;
  params.max_gap = 32;
  std::vector<float> solved = clean;
  REQUIRE_FALSE(ar_interpolate_region(solved.data(), solved.size(), 256, 256 + 33, params));
  for (size_t i = 0; i < kFrames; ++i) REQUIRE(solved[i] == clean[i]);
  REQUIRE(ar_interpolate_region(solved.data(), solved.size(), 256, 256 + 32, params));
}

TEST_CASE("AR interpolation with blend 0 leaves the interpolation baseline", "[mastering]") {
  constexpr size_t kFrames = 1024;
  constexpr size_t kGapStart = 512;
  constexpr size_t kGapEnd = kGapStart + 48;
  const std::vector<float> clean = ar_gap_bed(kFrames);

  std::vector<float> baseline = clean;
  interpolate_gap(baseline.data(), baseline.size(), kGapStart, kGapEnd);

  ArInterpolateParams params;
  params.order = 48;
  params.blend = 0.0f;
  std::vector<float> solved = clean;
  REQUIRE(ar_interpolate_region(solved.data(), solved.size(), kGapStart, kGapEnd, params));

  for (size_t i = 0; i < kFrames; ++i) REQUIRE(solved[i] == baseline[i]);
}

TEST_CASE("NoiseTracker initializes and follows stationary noise", "[mastering]") {
  NoiseTracker tracker(3, 48000, NoiseTracker::Mode::Mcra);
  const float power[] = {0.1f, 0.2f, 0.4f};

  for (int i = 0; i < 16; ++i) {
    tracker.update(power);
  }

  REQUIRE(tracker.n_bins() == 3);
  REQUIRE(tracker.mode() == NoiseTracker::Mode::Mcra);
  REQUIRE_THAT(tracker.noise_psd()[0], WithinAbs(0.1f, 0.001f));
  REQUIRE_THAT(tracker.noise_psd()[1], WithinAbs(0.2f, 0.001f));
  REQUIRE_THAT(tracker.speech_presence_probability()[0], WithinAbs(0.0f, 0.0001f));
}

TEST_CASE("NoiseTracker limits speech bursts with IMCRA mode", "[mastering]") {
  NoiseTracker mcra(1, 48000, NoiseTracker::Mode::Mcra);
  NoiseTracker imcra(1, 48000, NoiseTracker::Mode::Imcra);
  const float noise[] = {1.0f};
  const float burst[] = {100.0f};

  for (int i = 0; i < 90; ++i) {
    mcra.update(noise);
    imcra.update(noise);
  }
  for (int i = 0; i < 30; ++i) {
    mcra.update(burst);
    imcra.update(burst);
  }

  REQUIRE(imcra.speech_presence_probability()[0] > 0.5f);
  REQUIRE(imcra.noise_psd()[0] <= mcra.noise_psd()[0]);
  REQUIRE(imcra.noise_psd()[0] < 5.0f);
}

TEST_CASE("NoiseTracker reset clears learned state", "[mastering]") {
  NoiseTracker tracker(1, 48000, NoiseTracker::Mode::Static);
  const float power[] = {0.5f};
  tracker.update(power);
  REQUIRE(tracker.noise_psd()[0] > 0.1f);

  tracker.reset();

  REQUIRE(tracker.noise_psd()[0] < 0.000001f);
  REQUIRE_THROWS(tracker.update(nullptr));
}

// ---------------------------------------------------------------------------
// Loudness series
// ---------------------------------------------------------------------------

namespace {

namespace common = sonare::mastering::common;

std::vector<float> series_tone(std::size_t frames, int sample_rate, float hz, float amplitude,
                               std::size_t silent_head = 0) {
  std::vector<float> out(frames, 0.0f);
  for (std::size_t i = silent_head; i < frames; ++i) {
    const double phase = sonare::constants::kTwoPiD * static_cast<double>(hz) *
                         static_cast<double>(i) / static_cast<double>(sample_rate);
    out[i] = amplitude * static_cast<float>(std::sin(phase));
  }
  return out;
}

// Index of the first element carrying a measurement; a block covering only
// silence is -inf, which is what makes the two series' origins observable.
std::ptrdiff_t first_finite(const std::vector<float>& series) {
  for (std::size_t i = 0; i < series.size(); ++i) {
    if (std::isfinite(series[i])) return static_cast<std::ptrdiff_t>(i);
  }
  return -1;
}

/// Records how many blocks the offline runner hands a processor. The runner's
/// block loop is the only source a per-stage gain-reduction series could read,
/// so its call count bounds that series' resolution.
class CountingProcessor : public sonare::rt::ProcessorBase {
 public:
  void prepare(double sample_rate, int max_block_size) override {
    prepare(sample_rate, max_block_size, 1);
  }
  void prepare(double, int, int) override { calls = 0; }
  void process(float* const*, int, int) override { ++calls; }
  void reset() override {}

  int calls = 0;
};

}  // namespace

// The block layout is fixed by the spec, so the element count is a closed-form
// function of the input length. The expected values below are derived from
// ITU-R BS.1770-4 (400 ms window / 100 ms hop) and EBU R128 (3 s window /
// 100 ms hop) by hand, not from the constants the implementation mirrors:
// 4 s yields (4.0 - 0.4) / 0.1 + 1 = 37 momentary and (4.0 - 3.0) / 0.1 + 1 = 11
// short-term blocks at any rate. A hop off by a single sample moves both counts.
TEST_CASE("Loudness series block counts follow the BS.1770 window and hop", "[mastering]") {
  for (const int sample_rate : {44100, 48000}) {
    INFO("sample_rate = " << sample_rate);
    const std::size_t frames = static_cast<std::size_t>(4 * sample_rate);
    const auto samples = series_tone(frames, sample_rate, 1000.0f, 0.25f);

    common::LoudnessSeries series;
    common::measure_loudness_series_interleaved(samples.data(), frames, 1, sample_rate, &series);

    CHECK(series.momentary_lufs.size() == 37);
    CHECK(series.short_term_lufs.size() == 11);
  }
}

// Both series advance by 100 ms but start at their own window length, so the
// element concurrent with short_term[j] is momentary[j + 26]. A signal that is
// silent for exactly 3 s exposes the offset: the first block of each series to
// carry any tone differs by that lead.
TEST_CASE("Loudness series share a hop but not an origin", "[mastering]") {
  constexpr int kSampleRate = 44100;
  const std::size_t frames = static_cast<std::size_t>(5 * kSampleRate);
  const std::size_t silent_head = static_cast<std::size_t>(3 * kSampleRate);
  const auto samples = series_tone(frames, kSampleRate, 1000.0f, 0.25f, silent_head);

  common::LoudnessSeries series;
  common::measure_loudness_series_interleaved(samples.data(), frames, 1, kSampleRate, &series);

  const std::ptrdiff_t first_momentary = first_finite(series.momentary_lufs);
  const std::ptrdiff_t first_short_term = first_finite(series.short_term_lufs);
  REQUIRE(first_momentary >= 0);
  REQUIRE(first_short_term >= 0);
  CHECK(first_momentary == 27);
  CHECK(first_short_term == 1);
  CHECK(first_momentary - first_short_term ==
        static_cast<std::ptrdiff_t>(common::kShortTermSeriesLead));
}

// Only complete windows are emitted, so a signal shorter than a window yields no
// measurement at all rather than one taken over a sub-spec window.
TEST_CASE("Loudness series stay empty below their window length", "[mastering]") {
  constexpr int kSampleRate = 48000;

  SECTION("under 400 ms leaves both series empty") {
    const std::size_t frames = static_cast<std::size_t>(0.3 * kSampleRate);
    const auto samples = series_tone(frames, kSampleRate, 1000.0f, 0.25f);
    common::LoudnessSeries series;
    common::measure_loudness_series_interleaved(samples.data(), frames, 1, kSampleRate, &series);
    CHECK(series.momentary_lufs.empty());
    CHECK(series.short_term_lufs.empty());
  }

  SECTION("under 3 s keeps momentary but empties short-term") {
    const std::size_t frames = static_cast<std::size_t>(2.9 * kSampleRate);
    const auto samples = series_tone(frames, kSampleRate, 1000.0f, 0.25f);
    common::LoudnessSeries series;
    common::measure_loudness_series_interleaved(samples.data(), frames, 1, kSampleRate, &series);
    CHECK_FALSE(series.momentary_lufs.empty());
    CHECK(series.short_term_lufs.empty());
  }
}

// The series are the measurement's own intermediate, not a second pass over the
// same audio. Reducing them independently must reproduce the scalars bit for
// bit; any tolerance here would hide a second measurement.
TEST_CASE("Loudness series reduce to the summary scalars exactly", "[mastering]") {
  constexpr int kSampleRate = 44100;
  const std::size_t frames = static_cast<std::size_t>(4 * kSampleRate);
  auto samples = series_tone(frames, kSampleRate, 440.0f, 0.3f);
  // Break the stationarity so max-M, max-S and the final block differ from each
  // other; on a constant tone every block is equal and the check is vacuous.
  for (std::size_t i = frames / 2; i < frames; ++i) samples[i] *= 0.25f;

  common::LoudnessSeries series;
  const common::LoudnessSummary summary = common::measure_loudness_summary_interleaved(
      samples.data(), frames, 1, kSampleRate, common::kDefaultTruePeakOversample, &series);

  REQUIRE_FALSE(series.momentary_lufs.empty());
  REQUIRE_FALSE(series.short_term_lufs.empty());
  const float max_momentary =
      *std::max_element(series.momentary_lufs.begin(), series.momentary_lufs.end());
  const float max_short_term =
      *std::max_element(series.short_term_lufs.begin(), series.short_term_lufs.end());
  CHECK(max_momentary == summary.max_momentary_lufs);
  CHECK(max_short_term == summary.max_short_term_lufs);
  // The returned array is the one the loudness range was computed from.
  CHECK(sonare::metering::lra_from_short_term_blocks(series.short_term_lufs) ==
        summary.loudness_range);
  // A constant series would satisfy the two maxima trivially.
  CHECK(max_momentary >
        *std::min_element(series.momentary_lufs.begin(), series.momentary_lufs.end()));
}

// The mono meter is the one-channel case of the interleaved meter, so the
// series overload must not introduce a second code path.
TEST_CASE("Single-channel series overload matches the mono summary bit for bit", "[mastering]") {
  constexpr int kSampleRate = 48000;
  const std::size_t frames = static_cast<std::size_t>(4 * kSampleRate);
  auto samples = series_tone(frames, kSampleRate, 440.0f, 0.3f);
  // A stationary tone makes the loudness range zero on both sides and max-M
  // equal to max-S, so two of the five comparisons would hold even if the two
  // paths diverged. Breaking the stationarity gives all five distinct values.
  for (std::size_t i = frames / 2; i < frames; ++i) samples[i] *= 0.25f;
  const sonare::Audio audio = sonare::Audio::from_buffer(samples.data(), frames, kSampleRate);

  const common::LoudnessSummary mono = common::measure_loudness_summary(audio);
  common::LoudnessSeries series;
  const common::LoudnessSummary interleaved = common::measure_loudness_summary_interleaved(
      samples.data(), frames, 1, kSampleRate, common::kDefaultTruePeakOversample, &series);

  // The fixture's job is to make these five comparisons distinguishable; if the
  // signal ever goes stationary again, two of them go trivial in silence.
  CHECK(mono.loudness_range > 0.0f);
  CHECK(mono.max_momentary_lufs != mono.max_short_term_lufs);

  CHECK(interleaved.integrated_lufs == mono.integrated_lufs);
  CHECK(interleaved.max_momentary_lufs == mono.max_momentary_lufs);
  CHECK(interleaved.max_short_term_lufs == mono.max_short_term_lufs);
  CHECK(interleaved.true_peak_dbtp == mono.true_peak_dbtp);
  CHECK(interleaved.loudness_range == mono.loudness_range);
}

// A static gain is the one stage whose level delta has a closed form, so it is
// the lower bound the delta must satisfy exactly.
TEST_CASE("Stage level delta reports a static gain as a flat offset", "[mastering]") {
  constexpr int kSampleRate = 44100;
  const std::size_t frames = static_cast<std::size_t>(4 * kSampleRate);
  const auto before_samples = series_tone(frames, kSampleRate, 440.0f, 0.4f);
  std::vector<float> after_samples(before_samples);
  for (float& sample : after_samples) sample *= 0.5f;

  common::LoudnessSeries before;
  common::LoudnessSeries after;
  common::measure_loudness_series_interleaved(before_samples.data(), frames, 1, kSampleRate,
                                              &before);
  common::measure_loudness_series_interleaved(after_samples.data(), frames, 1, kSampleRate, &after);

  std::vector<float> momentary_delta;
  std::vector<float> short_term_delta;
  common::stage_level_delta_lu(before, after, &momentary_delta, &short_term_delta);

  REQUIRE(momentary_delta.size() == before.momentary_lufs.size());
  REQUIRE(short_term_delta.size() == before.short_term_lufs.size());
  REQUIRE_FALSE(momentary_delta.empty());
  for (const float delta : momentary_delta) {
    CHECK_THAT(delta, WithinAbs(-6.0206f, 0.01f));
  }
  for (const float delta : short_term_delta) {
    CHECK_THAT(delta, WithinAbs(-6.0206f, 0.01f));
  }
}

// Silent blocks are -inf on both sides; subtracting them directly would emit a
// NaN into a report vector.
TEST_CASE("Stage level delta reports silence as no change", "[mastering]") {
  constexpr int kSampleRate = 48000;
  const std::size_t frames = static_cast<std::size_t>(4 * kSampleRate);
  const std::vector<float> silence(frames, 0.0f);

  common::LoudnessSeries series;
  common::measure_loudness_series_interleaved(silence.data(), frames, 1, kSampleRate, &series);
  REQUIRE_FALSE(series.momentary_lufs.empty());
  REQUIRE_FALSE(std::isfinite(series.momentary_lufs.front()));

  std::vector<float> momentary_delta;
  common::stage_level_delta_lu(series, series, &momentary_delta, nullptr);
  REQUIRE(momentary_delta.size() == series.momentary_lufs.size());
  for (const float delta : momentary_delta) {
    CHECK(delta == 0.0f);
  }
}

TEST_CASE("Stage level delta refuses series of differing length", "[mastering]") {
  common::LoudnessSeries before;
  common::LoudnessSeries after;
  before.momentary_lufs = {-20.0f, -21.0f};
  after.momentary_lufs = {-20.0f};
  std::vector<float> delta;
  CHECK_THROWS(common::stage_level_delta_lu(before, after, &delta, nullptr));
}

// The residual is what a stage removed. Against a silent output it is the input
// itself, which pins the subtraction's orientation.
TEST_CASE("Residual loudness of a fully removed signal matches the input", "[mastering]") {
  constexpr int kSampleRate = 44100;
  const std::size_t frames = static_cast<std::size_t>(4 * kSampleRate);
  const auto before_samples = series_tone(frames, kSampleRate, 440.0f, 0.4f);
  const std::vector<float> after_samples(frames, 0.0f);

  const common::LoudnessSummary residual = common::measure_residual_loudness_summary(
      before_samples.data(), after_samples.data(), frames, kSampleRate);
  const common::LoudnessSummary input =
      common::measure_loudness_summary_interleaved(before_samples.data(), frames, 1, kSampleRate);

  CHECK(residual.integrated_lufs == input.integrated_lufs);
  CHECK(residual.true_peak_dbtp == input.true_peak_dbtp);
  // An unchanged stage leaves nothing behind.
  const common::LoudnessSummary nothing = common::measure_residual_loudness_summary(
      before_samples.data(), before_samples.data(), frames, kSampleRate);
  CHECK_FALSE(std::isfinite(nothing.integrated_lufs));
}

// The offline runner hands a processor one block of min(n + latency, 64 Ki)
// samples at a time, so `last_gain_reduction_db()` read once per block cannot
// produce more elements than that. At 44.1 kHz a one-second input is a single
// block: a gain-reduction series read there would hold exactly one element,
// which is the scalar the chain already reports. Raising that resolution means
// changing the runner's block size, which is what keeps a real per-stage
// gain-reduction series out of this change.
TEST_CASE("Offline runner blocks bound a per-stage gain-reduction series", "[mastering]") {
  namespace internal = sonare::mastering::api::internal;

  SECTION("one second at 44.1 kHz is a single block") {
    std::vector<float> samples(44100, 0.1f);
    CountingProcessor processor;
    internal::run_processor_mono(processor, samples, 44100);
    CHECK(processor.calls == 1);
  }

  SECTION("the counter does move above the block cap") {
    std::vector<float> samples(3 * static_cast<std::size_t>(internal::kOfflineProcessorBlockSize),
                               0.1f);
    CountingProcessor processor;
    internal::run_processor_mono(processor, samples, 44100);
    CHECK(processor.calls == 3);
  }

  SECTION("the compressor adds no latency that would split the block") {
    sonare::mastering::dynamics::Compressor compressor;
    // Declaring prepare(double, int) hides the base three-argument overload, so
    // the channel-aware form the runner uses is only reachable through the base.
    sonare::rt::ProcessorBase& as_processor = compressor;
    as_processor.prepare(44100.0, 44100, 1);
    CHECK(as_processor.latency_samples() == 0);
  }
}

// The residual tap subtracts the chain input from the post-repair buffer in
// place, which only holds while every repair stage preserves the frame count.
TEST_CASE("Chain repair stages preserve the frame count", "[mastering]") {
  namespace repair = sonare::mastering::repair;
  constexpr int kSampleRate = 44100;
  const std::size_t frames = static_cast<std::size_t>(0.5 * kSampleRate);
  const auto samples = series_tone(frames, kSampleRate, 440.0f, 0.4f);
  const sonare::Audio input = sonare::Audio::from_buffer(samples.data(), frames, kSampleRate);

  CHECK(repair::declick(input).size() == frames);
  CHECK(repair::declip(input).size() == frames);
  CHECK(repair::decrackle(input).size() == frames);
  CHECK(repair::dehum(input).size() == frames);
  CHECK(repair::dereverb_classical(input).size() == frames);
  CHECK(repair::denoise_classical(input).size() == frames);
}
