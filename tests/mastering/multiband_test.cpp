#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "mastering/multiband/crossover.h"
#include "mastering/multiband/multiband_compressor.h"
#include "mastering/multiband/multiband_dynamic_eq.h"
#include "mastering/multiband/multiband_expander.h"
#include "mastering/multiband/multiband_imager.h"
#include "mastering/multiband/multiband_limiter.h"
#include "mastering/multiband/multiband_saturation.h"

using namespace sonare::mastering::multiband;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(float frequency_hz, int sample_rate, int samples, float amplitude = 0.25f) {
  std::vector<float> out(static_cast<size_t>(samples));
  for (int i = 0; i < samples; ++i) {
    out[static_cast<size_t>(i)] =
        amplitude * static_cast<float>(std::sin(2.0 * kPi * frequency_hz * i / sample_rate));
  }
  return out;
}

float rms_tail(const std::vector<float>& samples, size_t skip) {
  double sum = 0.0;
  size_t count = 0;
  for (size_t i = std::min(skip, samples.size()); i < samples.size(); ++i) {
    sum += static_cast<double>(samples[i]) * samples[i];
    ++count;
  }
  return count == 0 ? 0.0f : static_cast<float>(std::sqrt(sum / static_cast<double>(count)));
}

void process(sonare::mastering::common::ProcessorBase& processor, std::vector<float>& mono) {
  float* channels[] = {mono.data()};
  processor.process(channels, 1, static_cast<int>(mono.size()));
}

void process_stereo(sonare::mastering::common::ProcessorBase& processor, std::vector<float>& left,
                    std::vector<float>& right) {
  float* channels[] = {left.data(), right.data()};
  processor.process(channels, 2, static_cast<int>(left.size()));
}

std::vector<float> side_signal(const std::vector<float>& left, const std::vector<float>& right) {
  std::vector<float> side(left.size());
  for (size_t i = 0; i < left.size(); ++i) {
    side[i] = 0.5f * (left[i] - right[i]);
  }
  return side;
}

std::vector<float> reconstruct_mono(const CrossoverOutput& output) {
  std::vector<float> reconstructed(static_cast<size_t>(output.num_samples()), 0.0f);
  for (int band = 0; band < output.num_bands(); ++band) {
    const auto& samples = output.bands[static_cast<size_t>(band)][0];
    for (size_t i = 0; i < reconstructed.size(); ++i) {
      reconstructed[i] += samples[i];
    }
  }
  return reconstructed;
}

}  // namespace

TEST_CASE("Crossover splits into residual bands that reconstruct the input",
          "[mastering][multiband]") {
  Crossover crossover({{200.0f, 2000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley});
  crossover.prepare(48000.0, 1024);

  auto signal = sine(100.0f, 48000, 48000, 0.2f);
  auto high = sine(6000.0f, 48000, 48000, 0.1f);
  for (size_t i = 0; i < signal.size(); ++i) {
    signal[i] += high[i];
  }
  float* channels[] = {signal.data()};

  const auto output = crossover.split(channels, 1, static_cast<int>(signal.size()));
  const auto reconstructed = reconstruct_mono(output);

  REQUIRE(output.num_bands() == 3);
  REQUIRE(output.num_channels() == 1);
  REQUIRE(rms_tail(output.bands[0][0], 4096) > rms_tail(output.bands[2][0], 4096));
  float max_error = 0.0f;
  for (size_t i = 4096; i < signal.size(); ++i) {
    max_error = std::max(max_error, std::abs(reconstructed[i] - signal[i]));
  }
  REQUIRE(max_error < 0.000001f);
}

TEST_CASE("Crossover routes low and high tones to expected bands", "[mastering][multiband]") {
  Crossover crossover({{1000.0f}, CrossoverSlope::LR2, CrossoverMode::LinkwitzRiley});
  crossover.prepare(48000.0, 1024);

  auto low = sine(100.0f, 48000, 48000, 0.4f);
  auto high = sine(8000.0f, 48000, 48000, 0.4f);

  float* low_channels[] = {low.data()};
  const auto low_split = crossover.split(low_channels, 1, static_cast<int>(low.size()));
  crossover.reset();
  float* high_channels[] = {high.data()};
  const auto high_split = crossover.split(high_channels, 1, static_cast<int>(high.size()));

  REQUIRE(rms_tail(low_split.bands[0][0], 4096) > rms_tail(low_split.bands[1][0], 4096) * 4.0f);
  REQUIRE(rms_tail(high_split.bands[1][0], 4096) > rms_tail(high_split.bands[0][0], 4096) * 4.0f);
}

TEST_CASE("Crossover validates cutoff configuration", "[mastering][multiband]") {
  REQUIRE_THROWS(Crossover({{1000.0f, 500.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley}));

  Crossover crossover({{30000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley});
  REQUIRE_THROWS(crossover.prepare(48000.0, 1024));
}

TEST_CASE("MultibandCompressor compresses only the configured low band", "[mastering][multiband]") {
  MultibandCompressorConfig config;
  config.crossover = {{1000.0f}, CrossoverSlope::LR2, CrossoverMode::LinkwitzRiley};
  config.bands = {
      {-30.0f, 8.0f, 0.0f, 20.0f, 0.0f, 0.0f, false,
       sonare::mastering::dynamics::DetectorMode::Rms},
      {0.0f, 1.0f, 0.0f, 20.0f, 0.0f, 0.0f, false, sonare::mastering::dynamics::DetectorMode::Rms},
  };
  MultibandCompressor compressor(config);
  compressor.prepare(48000.0, 1024);

  auto signal = sine(100.0f, 48000, 48000, 0.6f);
  auto high = sine(8000.0f, 48000, 48000, 0.1f);
  for (size_t i = 0; i < signal.size(); ++i) {
    signal[i] += high[i];
  }

  auto before = signal;
  process(compressor, signal);

  Crossover analyzer(config.crossover);
  analyzer.prepare(48000.0, 1024);
  float* before_channels[] = {before.data()};
  auto before_split = analyzer.split(before_channels, 1, static_cast<int>(before.size()));
  analyzer.reset();
  float* after_channels[] = {signal.data()};
  auto after_split = analyzer.split(after_channels, 1, static_cast<int>(signal.size()));

  REQUIRE(rms_tail(after_split.bands[0][0], 4096) <
          rms_tail(before_split.bands[0][0], 4096) * 0.6f);
  REQUIRE(rms_tail(after_split.bands[1][0], 4096) >
          rms_tail(before_split.bands[1][0], 4096) * 0.65f);
  REQUIRE(compressor.last_gain_reductions_db().size() == 2);
  REQUIRE(compressor.last_gain_reductions_db()[0] < -6.0f);
}

TEST_CASE("MultibandCompressor validates band count", "[mastering][multiband]") {
  MultibandCompressorConfig config;
  config.crossover = {{1000.0f, 4000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands.resize(2);
  REQUIRE_THROWS(MultibandCompressor(config));
}

TEST_CASE("MultibandLimiter limits only the configured high band", "[mastering][multiband]") {
  MultibandLimiterConfig config;
  config.crossover = {{1000.0f}, CrossoverSlope::LR2, CrossoverMode::LinkwitzRiley};
  config.bands = {
      {0.0f, 0.0f, 20.0f},
      {-18.0f, 0.0f, 20.0f},
  };
  MultibandLimiter limiter(config);
  limiter.prepare(48000.0, 1024);

  auto low = sine(100.0f, 48000, 48000, 0.1f);
  auto high = sine(8000.0f, 48000, 48000, 0.8f);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);

  process(limiter, low);
  limiter.reset();
  process(limiter, high);

  REQUIRE(rms_tail(high, 4096) < high_before * 0.45f);
  REQUIRE(rms_tail(low, 4096) > low_before * 0.7f);
  REQUIRE(limiter.last_gain_reductions_db().size() == 2);
  REQUIRE(limiter.last_gain_reductions_db()[1] < -6.0f);
}

TEST_CASE("MultibandLimiter validates band count", "[mastering][multiband]") {
  MultibandLimiterConfig config;
  config.crossover = {{1000.0f, 4000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands.resize(2);
  REQUIRE_THROWS(MultibandLimiter(config));
}

TEST_CASE("MultibandExpander reduces only the configured low noise band",
          "[mastering][multiband]") {
  MultibandExpanderConfig config;
  config.crossover = {{1000.0f}, CrossoverSlope::LR2, CrossoverMode::LinkwitzRiley};
  config.bands = {
      {-20.0f, 3.0f, 0.0f, 20.0f, -50.0f},
      {-80.0f, 1.0f, 0.0f, 20.0f, -50.0f},
  };
  MultibandExpander expander(config);
  expander.prepare(48000.0, 1024);

  auto low = sine(100.0f, 48000, 48000, 0.02f);
  auto high = sine(8000.0f, 48000, 48000, 0.3f);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);

  process(expander, low);
  expander.reset();
  process(expander, high);

  REQUIRE(rms_tail(low, 4096) < low_before * 0.35f);
  REQUIRE(rms_tail(high, 4096) > high_before * 0.75f);
  REQUIRE(expander.last_gain_reductions_db().size() == 2);
  REQUIRE(expander.last_gain_reductions_db()[1] == 0.0f);
}

TEST_CASE("MultibandExpander validates band count", "[mastering][multiband]") {
  MultibandExpanderConfig config;
  config.crossover = {{1000.0f, 4000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands.resize(2);
  REQUIRE_THROWS(MultibandExpander(config));
}

TEST_CASE("MultibandSaturation saturates only the configured high band", "[mastering][multiband]") {
  MultibandSaturationConfig config;
  config.crossover = {{1000.0f}, CrossoverSlope::LR2, CrossoverMode::LinkwitzRiley};
  config.bands = {
      {0.0f, 0.0f, 0.0f, true},
      {12.0f, 1.0f, -12.0f, true},
  };
  MultibandSaturation saturation(config);
  saturation.prepare(48000.0, 1024);

  auto low = sine(100.0f, 48000, 48000, 0.4f);
  auto high = sine(8000.0f, 48000, 48000, 0.8f);
  const auto low_before = low;
  const auto high_before = high;

  process(saturation, low);
  saturation.reset();
  process(saturation, high);

  REQUIRE(rms_tail(low, 4096) > rms_tail(low_before, 4096) * 0.9f);
  REQUIRE(rms_tail(high, 4096) < rms_tail(high_before, 4096) * 0.75f);
  REQUIRE(high[48000 / 32] != high_before[48000 / 32]);
}

TEST_CASE("MultibandSaturation validates configuration", "[mastering][multiband]") {
  MultibandSaturationConfig config;
  config.crossover = {{1000.0f, 4000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands.resize(2);
  REQUIRE_THROWS(MultibandSaturation(config));

  config.crossover = {{1000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands = {{}, {0.0f, 1.5f, 0.0f, true}};
  REQUIRE_THROWS(MultibandSaturation(config));
}

TEST_CASE("MultibandDynamicEq applies dynamic EQ inside selected band", "[mastering][multiband]") {
  MultibandDynamicEqConfig config;
  config.crossover = {{1000.0f}, CrossoverSlope::LR2, CrossoverMode::LinkwitzRiley};
  config.bands = {
      {{sonare::mastering::eq::EqBandType::Peak, 100.0f, 0.0f, 1.0f, -36.0f, 4.0f, -12.0f, true}},
      {},
  };
  MultibandDynamicEq dynamic_eq(config);
  dynamic_eq.prepare(48000.0, 1024);

  auto low = sine(100.0f, 48000, 48000, 0.5f);
  auto high = sine(8000.0f, 48000, 48000, 0.5f);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);

  process(dynamic_eq, low);
  const float low_applied_gain_db = dynamic_eq.last_applied_gain_db()[0][0];
  dynamic_eq.reset();
  process(dynamic_eq, high);

  REQUIRE(rms_tail(low, 4096) < low_before * 0.75f);
  REQUIRE(rms_tail(high, 4096) > high_before * 0.75f);
  REQUIRE(dynamic_eq.last_applied_gain_db().size() == 2);
  REQUIRE(low_applied_gain_db < -6.0f);
}

TEST_CASE("MultibandDynamicEq validates band count and dynamic band count",
          "[mastering][multiband]") {
  MultibandDynamicEqConfig config;
  config.crossover = {{1000.0f, 4000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands.resize(2);
  REQUIRE_THROWS(MultibandDynamicEq(config));

  config.crossover = {{1000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands = {{},
                  std::vector<sonare::mastering::eq::DynamicEqBand>(
                      sonare::mastering::eq::DynamicEq::kMaxBands + 1)};
  REQUIRE_THROWS(MultibandDynamicEq(config));
}

TEST_CASE("MultibandImager widens only the configured high band", "[mastering][multiband]") {
  MultibandImagerConfig config;
  config.crossover = {{1000.0f}, CrossoverSlope::LR2, CrossoverMode::LinkwitzRiley};
  config.bands = {
      {1.0f, true},
      {2.0f, true},
  };
  MultibandImager imager(config);
  imager.prepare(48000.0, 1024);

  auto low_left = sine(100.0f, 48000, 48000, 0.3f);
  auto low_right = low_left;
  auto high_left = sine(8000.0f, 48000, 48000, 0.2f);
  auto high_right = high_left;
  for (auto& sample : high_right) {
    sample = -sample;
  }

  const float low_side_before = rms_tail(side_signal(low_left, low_right), 4096);
  const float high_side_before = rms_tail(side_signal(high_left, high_right), 4096);

  process_stereo(imager, low_left, low_right);
  imager.reset();
  process_stereo(imager, high_left, high_right);

  const float low_side_after = rms_tail(side_signal(low_left, low_right), 4096);
  const float high_side_after = rms_tail(side_signal(high_left, high_right), 4096);

  REQUIRE(low_side_after <= low_side_before + 0.000001f);
  REQUIRE(high_side_after > high_side_before * 1.5f);
}

TEST_CASE("MultibandImager mono input is transparent", "[mastering][multiband]") {
  MultibandImagerConfig config;
  config.crossover = {{1000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands = {{0.0f, true}, {2.0f, true}};
  MultibandImager imager(config);
  imager.prepare(48000.0, 1024);

  auto mono = sine(500.0f, 48000, 48000, 0.3f);
  auto before = mono;
  process(imager, mono);

  float max_error = 0.0f;
  for (size_t i = 4096; i < mono.size(); ++i) {
    max_error = std::max(max_error, std::abs(mono[i] - before[i]));
  }
  REQUIRE(max_error < 0.000001f);
}

TEST_CASE("MultibandImager validates configuration", "[mastering][multiband]") {
  MultibandImagerConfig config;
  config.crossover = {{1000.0f, 4000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands.resize(2);
  REQUIRE_THROWS(MultibandImager(config));

  config.crossover = {{1000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands = {{1.0f, true}, {-1.0f, true}};
  REQUIRE_THROWS(MultibandImager(config));
}
