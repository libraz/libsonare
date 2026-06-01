#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "mastering/master.h"
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
#include "util/lpc.h"

using Catch::Matchers::WithinAbs;
using sonare::ar_interpolate;
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

float rms(const std::vector<float>& samples, size_t skip = 0) {
  double sum = 0.0;
  size_t count = 0;
  for (size_t i = std::min(skip, samples.size()); i < samples.size(); ++i) {
    sum += static_cast<double>(samples[i]) * samples[i];
    ++count;
  }
  return count == 0 ? 0.0f : static_cast<float>(std::sqrt(sum / static_cast<double>(count)));
}

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

// The historical "mastering::common::* aliases of rt::*" test was removed when
// the rt shim layer under mastering/common/ was deleted; rt primitives are now
// consumed directly via #include "rt/...". The remaining tests in this file
// continue to exercise the umbrella header and the real common/ implementations
// (Biquad, JilesAtherton, loudness_measure, NoiseTracker).

TEST_CASE("ParamSmoother reaches target immediately with zero time", "[mastering]") {
  ParamSmoother smoother(0.0f, 0.0f, 48000.0);

  smoother.set_target(1.0f);

  REQUIRE_THAT(smoother.process(), WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(smoother.current(), WithinAbs(1.0f, 0.0001f));
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

TEST_CASE("Oversampler preserves source samples while FIR-interpolating intermediate samples",
          "[mastering]") {
  Oversampler oversampler(4);
  const std::vector<float> input = {0.0f, 1.0f, 0.0f};

  const auto upsampled = oversampler.upsample(input);

  REQUIRE(upsampled.size() == input.size() * 4);
  REQUIRE_THAT(upsampled[0], WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(upsampled[4], WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(upsampled[8], WithinAbs(0.0f, 0.0001f));
  REQUIRE(std::abs(upsampled[2]) > 0.01f);
  REQUIRE(oversampler.latency_samples() == 6);
}

TEST_CASE("Oversampler downsample uses FIR decimation", "[mastering]") {
  Oversampler oversampler(8);
  std::vector<float> low_rate(256, 0.0f);
  for (size_t i = 0; i < low_rate.size(); ++i) {
    low_rate[i] = 0.4f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 250.0 *
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
  REQUIRE_THAT(output[4], WithinAbs(0.8f, 0.0001f));
  REQUIRE(std::abs(output[5]) > 0.01f);

  TruePeakFilter fallback(1, 2);
  std::vector<float> output_2x(input.size() * 2, 0.0f);
  float* output_2x_channels[] = {output_2x.data()};
  fallback.upsample(channels, output_2x_channels, 1, static_cast<int>(input.size()));
  REQUIRE(fallback.factor() == 2);
  REQUIRE(fallback.latency_samples() == 6);
  REQUIRE_THAT(output_2x[2], WithinAbs(0.8f, 0.0001f));
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

TEST_CASE("AR interpolation fills missing samples from past context", "[mastering]") {
  const std::vector<float> signal = {1.0f, 0.5f, 0.0f, 0.0f};
  const bool mask[] = {true, true, false, false};
  LpcResult model;
  model.ar = {1.0f, -0.5f};
  model.variance = 0.0f;

  const auto interpolated = ar_interpolate(signal.data(), mask, signal.size(), model);

  REQUIRE_THAT(interpolated[2], WithinAbs(0.25f, 0.0001f));
  REQUIRE_THAT(interpolated[3], WithinAbs(0.125f, 0.0001f));
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
