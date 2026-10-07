/// @file ensemble_deviation_test.cpp
/// @brief Ensemble per-voice deviations (pre-delay, depth, pan) and the single
///        LFO rate: voice i of three is offset by an evenly spaced value from
///        -dev to +dev, and rateHz drives both LFOs at the configured ratio.

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "effects/modulation/ensemble.h"
#include "rt/processor_base.h"
#include "support/audio_fixtures.h"
#ifdef SONARE_WITH_MASTERING
#include "mastering/api/insert_factory.h"
#endif

namespace {

using sonare::effects::modulation::Ensemble;
using sonare::effects::modulation::EnsembleConfig;
using sonare::test::kRate;

constexpr std::size_t kImpulseLength = 2400;
// The voices are read in one window each, split at the midpoints between them.
constexpr float kCenterMs = 8.0f;
constexpr float kPanGapMs = 4.0f;

/// A still, fully wet ensemble: no modulation, a wide tone filter.
EnsembleConfig still_config() {
  EnsembleConfig config;
  config.depth_slow_ms = 0.0f;
  config.depth_fast_ms = 0.0f;
  config.center_delay_ms = kCenterMs;
  config.tone_hz = 20000.0f;
  config.dry_wet = 1.0f;
  return config;
}

struct VoiceReading {
  double delay_ms[3];
  double gain_left[3];
  double gain_right[3];
};

/// Energy centroid (ms) and summed level of each voice window of the impulse
/// response, left and right.
VoiceReading read_voices(const EnsembleConfig& config, double sample_rate) {
  const double gap_ms = config.pre_delay_dev_ms;
  Ensemble processor(config);
  processor.prepare(sample_rate, static_cast<int>(kImpulseLength));
  std::vector<float> left(kImpulseLength, 0.0f);
  std::vector<float> right(kImpulseLength, 0.0f);
  left[0] = right[0] = 1.0f;
  sonare::test::process_stereo(processor, left, right);
  const double ms_to_samples = 0.001 * sample_rate;
  const double centre = kCenterMs * ms_to_samples;
  const double half_gap = 0.5 * gap_ms * ms_to_samples;
  VoiceReading out{};
  for (int voice = 0; voice < 3; ++voice) {
    const double lo = centre + (voice - 1) * gap_ms * ms_to_samples - half_gap;
    const double hi = lo + 2.0 * half_gap;
    double sum = 0.0;
    double moment = 0.0;
    double sum_right = 0.0;
    for (std::size_t i = static_cast<std::size_t>(std::max(0.0, lo)); i < hi; ++i) {
      sum += left[i];
      moment += left[i] * static_cast<double>(i);
      sum_right += right[i];
    }
    out.delay_ms[voice] = moment / sum / ms_to_samples;
    // Each voice enters the mix at one third, so a unity voice reads 1/3.
    out.gain_left[voice] = sum * 3.0;
    out.gain_right[voice] = sum_right * 3.0;
  }
  return out;
}

/// Mean delay (samples) of the three voices read off a linear ramp: the ramp
/// comes back as `slope * (n - mean delay)` through the interpolated lines.
std::vector<double> mean_delay_samples(Ensemble& processor, std::size_t count) {
  constexpr float kSlope = 1e-4f;
  std::vector<float> left(count);
  for (std::size_t i = 0; i < count; ++i) left[i] = kSlope * static_cast<float>(i);
  std::vector<float> right = left;
  sonare::test::process_stereo(processor, left, right);
  std::vector<double> delay(count);
  for (std::size_t i = 0; i < count; ++i) {
    delay[i] = static_cast<double>(i) - static_cast<double>(left[i]) / kSlope;
  }
  return delay;
}

std::vector<double> mean_delay_samples(const EnsembleConfig& config, double sample_rate,
                                       double seconds) {
  const std::size_t count = static_cast<std::size_t>(sample_rate * seconds);
  Ensemble processor(config);
  processor.prepare(sample_rate, static_cast<int>(count));
  return mean_delay_samples(processor, count);
}

double peak_to_peak(const std::vector<double>& delay, std::size_t skip) {
  double lo = delay[skip];
  double hi = delay[skip];
  for (std::size_t i = skip; i < delay.size(); ++i) {
    lo = std::min(lo, delay[i]);
    hi = std::max(hi, delay[i]);
  }
  return hi - lo;
}

/// Frequency from the first to the last rising mean crossing.
double crossing_frequency_hz(const std::vector<double>& delay, std::size_t skip,
                             double sample_rate) {
  double mean = 0.0;
  for (std::size_t i = skip; i < delay.size(); ++i) mean += delay[i];
  mean /= static_cast<double>(delay.size() - skip);
  double first = -1.0;
  double last = -1.0;
  int crossings = 0;
  for (std::size_t i = skip + 1; i < delay.size(); ++i) {
    const double a = delay[i - 1] - mean;
    const double b = delay[i] - mean;
    if (a < 0.0 && b >= 0.0) {
      const double at = static_cast<double>(i - 1) + (-a) / (b - a);
      if (first < 0.0) first = at;
      last = at;
      ++crossings;
    }
  }
  if (crossings < 2) return 0.0;
  return (crossings - 1) * sample_rate / (last - first);
}

/// A config where only the mean delay of the voices moves, at the slow LFO.
EnsembleConfig slow_only_config() {
  EnsembleConfig config = still_config();
  config.depth_slow_ms = 4.0f;
  config.depth_dev = 1.0f;
  return config;
}

EnsembleConfig fast_only_config() {
  EnsembleConfig config = still_config();
  config.depth_fast_ms = 1.0f;
  config.depth_dev = 1.0f;
  return config;
}

constexpr std::size_t kSkipSamples = 4800;

}  // namespace

TEST_CASE("Ensemble pre-delay deviation spreads the voices evenly", "[ensemble-deviation]") {
  for (const double rate : {48000.0, 44100.0}) {
    for (const float dev : {1.5f, 4.0f}) {
      EnsembleConfig config = still_config();
      config.pre_delay_dev_ms = dev;
      const VoiceReading r = read_voices(config, rate);
      INFO("rate " << rate << " dev " << dev);
      CHECK(r.delay_ms[1] - r.delay_ms[0] == Catch::Approx(dev).margin(0.02));
      CHECK(r.delay_ms[2] - r.delay_ms[1] == Catch::Approx(dev).margin(0.02));
      // The middle voice keeps the centre delay.
      CHECK(r.delay_ms[1] == Catch::Approx(kCenterMs).margin(0.1));
    }
  }
}

TEST_CASE("Ensemble pan deviation balances the outer voices", "[ensemble-deviation]") {
  EnsembleConfig config = still_config();
  config.pre_delay_dev_ms = kPanGapMs;
  const VoiceReading none = read_voices(config, kRate);
  for (int voice = 0; voice < 3; ++voice) {
    CHECK(none.gain_left[voice] == Catch::Approx(none.gain_right[voice]).margin(1e-4));
  }
  const double unity = none.gain_left[1];
  for (const float dev : {0.5f, 1.0f}) {
    config.pan_dev = dev;
    const VoiceReading r = read_voices(config, kRate);
    INFO("dev " << dev);
    CHECK(r.gain_left[0] == Catch::Approx(unity).margin(1e-3));
    CHECK(r.gain_right[0] == Catch::Approx(unity * (1.0 - dev)).margin(1e-3));
    CHECK(r.gain_left[1] == Catch::Approx(unity).margin(1e-3));
    CHECK(r.gain_right[1] == Catch::Approx(unity).margin(1e-3));
    CHECK(r.gain_left[2] == Catch::Approx(unity * (1.0 - dev)).margin(1e-3));
    CHECK(r.gain_right[2] == Catch::Approx(unity).margin(1e-3));
  }
}

TEST_CASE("Ensemble depth deviation scales each voice's sweep", "[ensemble-deviation]") {
  // Three phases 120 degrees apart with depth scales (1-d, 1, 1+d) sum to
  // d * sqrt(3) times one sweep, so the mean delay swings depth * d / sqrt(3)
  // either way and not at all when d is 0.
  constexpr double kSqrt3 = 1.7320508075688772;
  EnsembleConfig config = slow_only_config();
  config.rate_slow_hz = 2.0f;
  const double samples_per_ms = 0.001 * kRate;
  for (const float dev : {0.0f, 0.5f, 1.0f, -1.0f}) {
    config.depth_dev = dev;
    const double swing =
        peak_to_peak(mean_delay_samples(config, kRate, 1.0), kSkipSamples) / samples_per_ms;
    const double expected = 2.0 * 4.0 * std::fabs(dev) / kSqrt3;
    INFO("dev " << dev << " swing " << swing << " ms, expected " << expected);
    CHECK(swing == Catch::Approx(expected).margin(0.05));
  }
}

TEST_CASE("Ensemble rateHz drives both LFOs at the configured ratio", "[ensemble-deviation]") {
  constexpr double kSeconds = 2.0;
  EnsembleConfig slow = slow_only_config();
  EnsembleConfig fast = fast_only_config();
  for (EnsembleConfig* config : {&slow, &fast}) {
    config->rate_slow_hz = 2.0f;
    config->rate_fast_hz = 10.0f;
  }
  const auto slow_hz = [&](const EnsembleConfig& c) {
    return crossing_frequency_hz(mean_delay_samples(c, kRate, kSeconds), kSkipSamples, kRate);
  };

  SECTION("disabled leaves the two independent rates") {
    CHECK(slow_hz(slow) == Catch::Approx(2.0).epsilon(0.02));
    CHECK(slow_hz(fast) == Catch::Approx(10.0).epsilon(0.02));
  }

  SECTION("enabled moves both, keeping the slow:fast ratio") {
    slow.rate_hz = 4.0f;
    fast.rate_hz = 4.0f;
    const double slow_measured = slow_hz(slow);
    const double fast_measured = slow_hz(fast);
    CHECK(slow_measured == Catch::Approx(4.0).epsilon(0.02));
    CHECK(fast_measured == Catch::Approx(20.0).epsilon(0.02));
    CHECK(fast_measured / slow_measured == Catch::Approx(5.0).epsilon(0.03));
  }

  SECTION("realtime id 7 switches it on and back off") {
    Ensemble processor(slow);
    REQUIRE(processor.parameter_is_realtime_safe(7));
    const std::size_t count = static_cast<std::size_t>(kRate * kSeconds);
    processor.prepare(kRate, static_cast<int>(count));
    REQUIRE(processor.set_parameter(7, 4.0f));
    CHECK(crossing_frequency_hz(mean_delay_samples(processor, count), kSkipSamples, kRate) ==
          Catch::Approx(4.0).epsilon(0.02));
    REQUIRE(processor.set_parameter(7, 0.0f));
    processor.reset();
    CHECK(crossing_frequency_hz(mean_delay_samples(processor, count), kSkipSamples, kRate) ==
          Catch::Approx(2.0).epsilon(0.02));
  }
}

TEST_CASE("Ensemble deviation keys are realtime and reject non-finite values",
          "[ensemble-deviation]") {
  Ensemble processor;
  processor.prepare(kRate, 512);
  const auto descriptors = processor.parameter_descriptors();
  const char* const names[] = {"rateHz", "preDelayDevMs", "depthDev", "panDev"};
  for (unsigned id = 7; id <= 10; ++id) {
    INFO("id " << id);
    REQUIRE(descriptors.size() > id);
    CHECK(std::string(descriptors[id].key) == names[id - 7]);
    CHECK(processor.parameter_is_realtime_safe(id));
    CHECK(processor.set_parameter(id, 0.1f));
    CHECK_FALSE(processor.set_parameter(id, std::nanf("")));
  }
  CHECK_FALSE(processor.parameter_is_realtime_safe(13));
  CHECK_FALSE(processor.set_parameter(13, 0.0f));

  // A deviation far past its range is clamped to what prepare() sized and
  // still renders finite audio.
  processor.set_parameter(8, 1000.0f);
  processor.set_parameter(9, 100.0f);
  std::vector<float> left(4800, 0.5f);
  std::vector<float> right = left;
  sonare::test::process_stereo(processor, left, right);
  for (const float v : left) REQUIRE(std::isfinite(v));
}

#ifdef SONARE_WITH_MASTERING
TEST_CASE("Ensemble deviation keys reach the processor through the insert factory",
          "[ensemble-deviation]") {
  using sonare::mastering::api::make_insert;
  const auto render = [](const char* json) {
    auto processor = make_insert("effects.modulation.ensemble", json);
    REQUIRE(processor != nullptr);
    processor->prepare(kRate, 4800);
    std::vector<float> left = sonare::test::generate_sine(4800, 220.0f, 48000, 0.5f);
    std::vector<float> right = left;
    sonare::test::process_stereo(*processor, left, right);
    return left;
  };
  const std::vector<float> base = render("{}");
  CHECK(render(R"({"rateHz":0.0,"preDelayDevMs":0.0,"depthDev":0.0,"panDev":0.0})") == base);
  CHECK(render(R"({"rateHz":3.0})") != base);
  CHECK(render(R"({"preDelayDevMs":6.0})") != base);
  CHECK(render(R"({"depthDev":0.5})") != base);
  CHECK(render(R"({"panDev":0.8})") != base);
}
#endif
