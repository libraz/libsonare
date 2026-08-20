#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

#include "mastering/api/internal_processor_runner.h"
#include "mastering/eq/linear_phase.h"
#include "rt/processor_base.h"
#include "util/constants.h"

namespace internal = sonare::mastering::api::internal;

namespace {

using sonare::constants::kTwoPi;

/// A pure delay line that also records the shape of every block the offline
/// runner hands it. The delay makes the runner's latency compensation checkable
/// bit-exactly: a correctly compensated run must return the input unchanged.
class RecordingDelay : public sonare::rt::ProcessorBase {
 public:
  explicit RecordingDelay(int latency) : latency_(latency) {}

  void prepare(double sample_rate, int max_block_size) override {
    prepare(sample_rate, max_block_size, 2);
  }

  void prepare(double sample_rate, int max_block_size, int max_channels) override {
    (void)sample_rate;
    prepared_block_size = max_block_size;
    delay_lines_.assign(static_cast<std::size_t>(max_channels),
                        std::vector<float>(static_cast<std::size_t>(std::max(latency_, 0)), 0.0f));
    write_index_ = 0;
    block_sizes.clear();
  }

  void process(float* const* channels, int num_channels, int num_samples) override {
    block_sizes.push_back(num_samples);
    if (latency_ <= 0) {
      return;
    }
    for (int i = 0; i < num_samples; ++i) {
      for (int ch = 0; ch < num_channels; ++ch) {
        auto& line = delay_lines_[static_cast<std::size_t>(ch)];
        const float delayed = line[static_cast<std::size_t>(write_index_)];
        line[static_cast<std::size_t>(write_index_)] = channels[ch][i];
        channels[ch][i] = delayed;
      }
      write_index_ = (write_index_ + 1) % latency_;
    }
  }

  void reset() override {}

  int latency_samples() const noexcept override { return latency_; }

  int prepared_block_size = 0;
  std::vector<int> block_sizes;

 private:
  int latency_ = 0;
  std::vector<std::vector<float>> delay_lines_;
  int write_index_ = 0;
};

std::vector<float> ramp(int n, float scale) {
  std::vector<float> out(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    out[static_cast<std::size_t>(i)] = scale * static_cast<float>((i % 977) - 488);
  }
  return out;
}

std::vector<float> tone(int n, double sample_rate, double hz) {
  std::vector<float> out(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    out[static_cast<std::size_t>(i)] =
        static_cast<float>(0.4 * std::sin(kTwoPi * hz * static_cast<double>(i) / sample_rate));
  }
  return out;
}

/// Exact double-precision FIR with the runner's latency-compensated alignment:
/// y[i] = sum_k h[k] * x[i + half - k], with x zero outside [0, n).
std::vector<double> reference_fir(const std::vector<float>& x, const std::vector<float>& h) {
  const int n = static_cast<int>(x.size());
  const int taps = static_cast<int>(h.size());
  const int half = taps / 2;
  std::vector<double> y(static_cast<std::size_t>(n), 0.0);
  for (int i = 0; i < n; ++i) {
    double acc = 0.0;
    for (int k = 0; k < taps; ++k) {
      const int idx = i + half - k;
      if (idx >= 0 && idx < n) {
        acc += static_cast<double>(h[static_cast<std::size_t>(k)]) *
               static_cast<double>(x[static_cast<std::size_t>(idx)]);
      }
    }
    y[static_cast<std::size_t>(i)] = acc;
  }
  return y;
}

}  // namespace

// A processor whose fast path needs a block that is a whole multiple of an
// internal partition size (LinearPhaseEq's partitioned FFT convolver) only
// keeps that path if every block it is handed is the size it was prepared at.
// A ragged final chunk or a short latency flush would drop the tail of the
// render onto the slow path.
TEST_CASE("offline runner hands every processor block at the prepared size",
          "[mastering][offline_runner]") {
  const int latency = GENERATE_COPY(0, 1, 4096, 8192);
  // Lengths spanning: shorter than the block cap, an exact multiple of the cap,
  // and a ragged remainder above the cap.
  const int n = GENERATE_COPY(1, 1000, 49152, internal::kOfflineProcessorBlockSize,
                              internal::kOfflineProcessorBlockSize + 1,
                              2 * internal::kOfflineProcessorBlockSize + 24576);
  CAPTURE(latency, n);

  SECTION("mono") {
    auto samples = ramp(n, 1.0e-4f);
    RecordingDelay processor(latency);
    internal::run_processor_mono(processor, samples, 48000);
    REQUIRE_FALSE(processor.block_sizes.empty());
    for (int block : processor.block_sizes) {
      CHECK(block == processor.prepared_block_size);
    }
  }

  SECTION("stereo") {
    auto left = ramp(n, 1.0e-4f);
    auto right = ramp(n, -2.0e-4f);
    RecordingDelay processor(latency);
    internal::run_processor_stereo(processor, left, right, 48000);
    REQUIRE_FALSE(processor.block_sizes.empty());
    for (int block : processor.block_sizes) {
      CHECK(block == processor.prepared_block_size);
    }
  }
}

// The zero padding that squares the ragged tail up to a whole block must not
// reach the caller: a pure delay run through the runner has to return the input
// unchanged, sample for sample, including the very last sample.
TEST_CASE("offline runner trims its zero padding exactly", "[mastering][offline_runner]") {
  const int latency = GENERATE(0, 1, 512, 8192, 70000);
  const int n = GENERATE(1, 1000, 49152, 65537, 155648);
  CAPTURE(latency, n);

  const auto input = ramp(n, 1.0e-4f);

  auto mono = input;
  RecordingDelay mono_processor(latency);
  internal::run_processor_mono(mono_processor, mono, 48000);
  REQUIRE(mono.size() == input.size());
  CHECK(mono == input);

  auto left = input;
  auto right = input;
  for (auto& sample : right) sample = -sample;
  const auto right_input = right;
  RecordingDelay stereo_processor(latency);
  internal::run_processor_stereo(stereo_processor, left, right, 48000);
  CHECK(left == input);
  CHECK(right == right_input);
}

// End-to-end guard on the numeric consequence of the aligned blocking: the
// offline LinearPhaseEq result must stay close to an exact double-precision
// convolution with the same taps. The partitioned FFT convolver is less exact
// than a direct time-domain sum, so this pins the error floor rather than
// asserting bit-equality.
TEST_CASE("offline LinearPhaseEq stays close to an exact convolution",
          "[mastering][offline_runner][eq]") {
  using sonare::mastering::eq::EqBand;
  using sonare::mastering::eq::EqBandType;
  using sonare::mastering::eq::LinearPhaseEq;
  using sonare::mastering::eq::LinearPhaseEqConfig;

  constexpr int kSampleRate = 48000;
  // Ragged lengths: below the block cap, and above it with a remainder, so the
  // padded tail is exactly what the aligned blocking has to get right.
  const int n = GENERATE(12000, 70000);
  CAPTURE(n);

  // A short custom kernel keeps the O(n * taps) double reference cheap enough
  // for the default (Debug) test run.
  LinearPhaseEqConfig config;
  config.resolution = LinearPhaseEqConfig::Resolution::Custom;
  config.fft_size = 4096;
  config.kernel_size = 257;
  config.use_partitioned_convolution = true;

  const auto configure = [](LinearPhaseEq& eq) {
    EqBand band{};
    band.enabled = true;
    band.type = EqBandType::Peak;
    band.frequency_hz = 2400.0f;
    band.gain_db = -4.0f;
    band.q = 1.2f;
    eq.set_band(0, band);
  };

  const auto input = tone(n, kSampleRate, 220.0);

  std::vector<float> taps;
  {
    LinearPhaseEq eq(config);
    eq.prepare(kSampleRate, 1024, 1);
    configure(eq);
    taps = eq.kernel();
  }
  const auto expected = reference_fir(input, taps);
  double peak = 0.0;
  for (double value : expected) peak = std::max(peak, std::abs(value));
  REQUIRE(peak > 0.1);

  auto actual = input;
  {
    LinearPhaseEq eq(config);
    eq.prepare(kSampleRate, 1024, 1);
    configure(eq);
    internal::run_processor_mono(eq, actual, kSampleRate);
  }
  REQUIRE(actual.size() == expected.size());

  double max_error = 0.0;
  for (std::size_t i = 0; i < actual.size(); ++i) {
    max_error = std::max(max_error, std::abs(static_cast<double>(actual[i]) - expected[i]));
  }
  CAPTURE(max_error, peak);
  // -110 dB relative to peak: comfortably above the measured float-FFT floor
  // (about -118 dB at the widest kernel) and far below any audible threshold,
  // so this fails on a real misalignment rather than on rounding.
  CHECK(max_error < peak * 3.2e-6);
}
