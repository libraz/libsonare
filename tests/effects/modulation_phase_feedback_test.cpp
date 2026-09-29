/// @file modulation_phase_feedback_test.cpp
/// @brief Chorus feedback, the right-channel LFO phase of chorus and flanger,
///        and the flanger's sample-and-hold step rate.
///
/// The LFO is read back through the delay itself: a linear ramp through a
/// linearly interpolated line comes out as `c * (n - d[n])`, so the delay the
/// line used at each sample is `n - out / c`, exactly.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#include "core/fft.h"
#include "effects/modulation/chorus.h"
#include "effects/modulation/flanger.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"

namespace {

using sonare::constants::kTwoPiD;
using sonare::effects::modulation::Chorus;
using sonare::effects::modulation::ChorusConfig;
using sonare::effects::modulation::Flanger;
using sonare::effects::modulation::FlangerConfig;

constexpr double kRate = 48000.0;
constexpr int kFftLength = 16384;
constexpr double kDegreesPerTurn = 360.0;

// --- feedback -----------------------------------------------------------------

// A loop delay of 2 ms puts the comb teeth 500 Hz apart, so a tooth and the
// notch half a spacing away are 500 and 250 Hz.
constexpr float kLoopDelayMs = 2.0f;
constexpr double kToothHz = 500.0;
constexpr double kNotchHz = 250.0;

ChorusConfig still_chorus(float feedback) {
  ChorusConfig config;
  config.rate_hz = 0.0f;
  config.depth_ms = 0.0f;
  config.center_delay_ms = kLoopDelayMs;
  config.dry_wet = 1.0f;
  config.feedback = feedback;
  return config;
}

/// Magnitude in dB of the chorus's impulse response at each bin.
std::vector<double> impulse_db(const ChorusConfig& config) {
  Chorus chorus(config);
  chorus.prepare(kRate, kFftLength);
  std::vector<float> impulse = sonare::test::generate_impulse(kFftLength);
  sonare::test::process(chorus, impulse);
  sonare::FFT plan(kFftLength);
  std::vector<std::complex<float>> spectrum(static_cast<std::size_t>(plan.n_bins()));
  plan.forward(impulse.data(), spectrum.data());
  std::vector<double> db(spectrum.size());
  for (std::size_t i = 0; i < db.size(); ++i) {
    // Floors a raw |X| before a log, below any real null.
    db[i] = 20.0 * std::log10(static_cast<double>(std::abs(spectrum[i])) + 1e-30);
  }
  return db;
}

double db_at(const std::vector<double>& db, double hz) {
  return db[static_cast<std::size_t>(std::lround(hz * kFftLength / kRate))];
}

TEST_CASE("chorus feedback builds a comb at multiples of the loop delay", "[chorus-feedback]") {
  const std::vector<double> flat = impulse_db(still_chorus(0.0f));
  const std::vector<double> positive = impulse_db(still_chorus(0.7f));
  const std::vector<double> negative = impulse_db(still_chorus(-0.7f));

  // No feedback is one echo: a flat magnitude.
  CHECK(std::fabs(db_at(flat, kToothHz) - db_at(flat, kNotchHz)) < 0.1);
  // A positive loop peaks at the multiples of 1/delay and dips halfway between.
  CHECK(db_at(positive, kToothHz) - db_at(positive, kNotchHz) > 10.0);
  // Its sign moves the teeth by half a spacing: peaks and notches swap.
  CHECK(db_at(negative, kNotchHz) - db_at(negative, kToothHz) > 10.0);
  // The tooth stands well above the dry level and the notch below it.
  CHECK(db_at(positive, kToothHz) > 6.0);
  CHECK(db_at(positive, kNotchHz) < -3.0);
}

TEST_CASE("chorus feedback is clamped and automatable in place", "[chorus-feedback]") {
  Chorus chorus(still_chorus(0.0f));
  chorus.prepare(kRate, kFftLength);
  REQUIRE(chorus.parameter_is_realtime_safe(5));
  REQUIRE(chorus.set_parameter(5, 0.7f));
  // Far past the limit must still be a stable loop, not a runaway.
  REQUIRE(chorus.set_parameter(5, 40.0f));
  std::vector<float> impulse = sonare::test::generate_impulse(kFftLength);
  sonare::test::process(chorus, impulse);
  CHECK(sonare::test::peak_abs(impulse) < 100.0f);
  CHECK(std::isfinite(impulse.back()));
  CHECK(std::fabs(impulse.back()) < 1e-3f);
}

// --- LFO read-back ------------------------------------------------------------

constexpr double kLfoRateHz = 5.0;
constexpr float kCenterMs = 5.0f;
constexpr float kDepthMs = 4.0f;
constexpr float kRampSlope = 1e-4f;
// One LFO period at 5 Hz: the warm-up ends on a whole cycle so both channels
// are read from a known phase.
constexpr double kWarmSeconds = 0.2;
constexpr double kReadSeconds = 1.0;

struct Delays {
  std::vector<double> left;
  std::vector<double> right;
};

/// Runs a ramp through @p processor and returns the delay each channel used over
/// the read window. @p between runs after the warm-up and before the window.
template <typename Processor, typename Between>
Delays read_delays(Processor& processor, double sample_rate, Between between) {
  const std::size_t warm = static_cast<std::size_t>(kWarmSeconds * sample_rate);
  const std::size_t window = static_cast<std::size_t>(kReadSeconds * sample_rate);
  std::vector<float> left(warm + window);
  for (std::size_t i = 0; i < left.size(); ++i) left[i] = kRampSlope * static_cast<float>(i);
  std::vector<float> right = left;
  float* channels[] = {left.data(), right.data()};
  processor.process(channels, 2, static_cast<int>(warm));
  between();
  float* rest[] = {left.data() + warm, right.data() + warm};
  processor.process(rest, 2, static_cast<int>(window));
  Delays out;
  for (std::size_t i = warm; i < left.size(); ++i) {
    const double n = static_cast<double>(i);
    out.left.push_back(n - static_cast<double>(left[i]) / kRampSlope);
    out.right.push_back(n - static_cast<double>(right[i]) / kRampSlope);
  }
  return out;
}

/// Phase of a sinusoid at kLfoRateHz over the window, in radians.
double phase_of(const std::vector<double>& samples, double sample_rate) {
  double along_sin = 0.0;
  double along_cos = 0.0;
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const double angle = kTwoPiD * kLfoRateHz * static_cast<double>(i) / sample_rate;
    along_sin += samples[i] * std::sin(angle);
    along_cos += samples[i] * std::cos(angle);
  }
  return std::atan2(along_cos, along_sin);
}

/// Right-over-left LFO phase minus @p expected_deg, in degrees, wrapped to
/// [-180, 180] so a half-turn reads the same from either side.
double offset_error_deg(const Delays& delays, double sample_rate, double expected_deg) {
  double diff = (phase_of(delays.right, sample_rate) - phase_of(delays.left, sample_rate)) *
                    kDegreesPerTurn / kTwoPiD -
                expected_deg;
  while (diff > 180.0) diff -= kDegreesPerTurn;
  while (diff < -180.0) diff += kDegreesPerTurn;
  return diff;
}

ChorusConfig sweeping_chorus(float phase_deg) {
  ChorusConfig config;
  config.rate_hz = static_cast<float>(kLfoRateHz);
  config.depth_ms = kDepthMs;
  config.center_delay_ms = kCenterMs;
  config.dry_wet = 1.0f;
  config.phase_deg = phase_deg;
  return config;
}

FlangerConfig sweeping_flanger(float phase_deg) {
  FlangerConfig config;
  config.rate_hz = static_cast<float>(kLfoRateHz);
  config.depth_ms = kDepthMs;
  config.center_delay_ms = kCenterMs;
  config.feedback = 0.0f;
  config.dry_wet = 1.0f;
  config.phase_deg = phase_deg;
  return config;
}

constexpr float kPhasesDeg[] = {0.0f, 45.0f, 120.0f, 180.0f};
constexpr double kPhaseToleranceDeg = 1.0;

TEST_CASE("phaseDeg sets the measured right-over-left LFO phase", "[mod-phase]") {
  for (const float phase : kPhasesDeg) {
    INFO("phaseDeg " << phase);
    Chorus chorus(sweeping_chorus(phase));
    chorus.prepare(kRate, 1 << 16);
    CHECK(std::fabs(offset_error_deg(read_delays(chorus, kRate, [] {}), kRate, phase)) <
          kPhaseToleranceDeg);
    Flanger flanger(sweeping_flanger(phase));
    flanger.prepare(kRate, 1 << 16);
    CHECK(std::fabs(offset_error_deg(read_delays(flanger, kRate, [] {}), kRate, phase)) <
          kPhaseToleranceDeg);
  }
}

TEST_CASE("the default right-over-left phase is the one the inserts had before", "[mod-phase]") {
  // Quarter of a turn for the chorus, half a turn for the flanger.
  ChorusConfig chorus_config;
  chorus_config.rate_hz = static_cast<float>(kLfoRateHz);
  chorus_config.depth_ms = kDepthMs;
  chorus_config.center_delay_ms = kCenterMs;
  chorus_config.dry_wet = 1.0f;
  Chorus chorus(chorus_config);
  chorus.prepare(kRate, 1 << 16);
  CHECK(std::fabs(offset_error_deg(read_delays(chorus, kRate, [] {}), kRate, 90.0)) <
        kPhaseToleranceDeg);

  FlangerConfig flanger_config = sweeping_flanger(0.0f);
  flanger_config.phase_deg = FlangerConfig{}.phase_deg;
  Flanger flanger(flanger_config);
  flanger.prepare(kRate, 1 << 16);
  CHECK(std::fabs(offset_error_deg(read_delays(flanger, kRate, [] {}), kRate, 180.0)) <
        kPhaseToleranceDeg);
}

TEST_CASE("phaseDeg moves in place while running", "[mod-phase]") {
  for (const double rate : {48000.0, 44100.0}) {
    INFO("rate " << rate);
    Chorus chorus(sweeping_chorus(0.0f));
    chorus.prepare(rate, 1 << 16);
    REQUIRE(chorus.parameter_is_realtime_safe(6));
    const Delays chorus_delays =
        read_delays(chorus, rate, [&] { REQUIRE(chorus.set_parameter(6, 60.0f)); });
    CHECK(std::fabs(offset_error_deg(chorus_delays, rate, 60.0)) < kPhaseToleranceDeg);
    Flanger flanger(sweeping_flanger(0.0f));
    flanger.prepare(rate, 1 << 16);
    REQUIRE(flanger.parameter_is_realtime_safe(6));
    const Delays flanger_delays =
        read_delays(flanger, rate, [&] { REQUIRE(flanger.set_parameter(6, 60.0f)); });
    CHECK(std::fabs(offset_error_deg(flanger_delays, rate, 60.0)) < kPhaseToleranceDeg);
  }
}

// --- step rate ----------------------------------------------------------------

constexpr float kStepLfoHz = 20.0f;
constexpr float kStepRateHz = 500.0f;
// A change in the delay smaller than this is float noise from the read-back,
// and one larger is the LFO moving.
constexpr double kMovedSamples = 0.05;

FlangerConfig stepping_flanger(float step_rate_hz) {
  FlangerConfig config = sweeping_flanger(0.0f);
  config.rate_hz = kStepLfoHz;
  config.step_rate_hz = step_rate_hz;
  return config;
}

/// Sample indices at which the left delay moved.
std::vector<std::size_t> moves(const std::vector<double>& delays) {
  std::vector<std::size_t> at;
  for (std::size_t i = 1; i < delays.size(); ++i) {
    if (std::fabs(delays[i] - delays[i - 1]) > kMovedSamples) at.push_back(i);
  }
  return at;
}

TEST_CASE("stepRateHz holds the LFO between steps at that rate", "[flanger-step]") {
  for (const double rate : {48000.0, 44100.0}) {
    INFO("rate " << rate);
    Flanger flanger(stepping_flanger(kStepRateHz));
    flanger.prepare(rate, 1 << 16);
    const std::vector<std::size_t> at = moves(read_delays(flanger, rate, [] {}).left);
    const double expected_samples = rate / kStepRateHz;
    REQUIRE(at.size() > 100);
    // Held between steps: far fewer movements than samples, one per period.
    CHECK(static_cast<double>(at.size()) < 1.05 * kReadSeconds * kStepRateHz);
    double total = 0.0;
    int count = 0;
    for (std::size_t i = 1; i < at.size(); ++i) {
      const double gap = static_cast<double>(at[i] - at[i - 1]);
      // A step that lands on an LFO crest moves less than the noise floor and
      // shows up as a double gap; it is not a period.
      if (gap > 1.5 * expected_samples) continue;
      total += gap;
      ++count;
    }
    REQUIRE(count > 100);
    CHECK(std::fabs(total / count - expected_samples) < 0.01 * expected_samples);
  }
}

TEST_CASE("stepRateHz of zero leaves the LFO continuous", "[flanger-step]") {
  Flanger flanger(stepping_flanger(0.0f));
  flanger.prepare(kRate, 1 << 16);
  const std::vector<std::size_t> at = moves(read_delays(flanger, kRate, [] {}).left);
  // The delay is moving on nearly every sample; only the LFO's crests stand still.
  CHECK(static_cast<double>(at.size()) > 0.9 * kReadSeconds * kRate);
}

TEST_CASE("stepRateHz is automatable and 0 turns the hold off in place", "[flanger-step]") {
  Flanger flanger(stepping_flanger(kStepRateHz));
  flanger.prepare(kRate, 1 << 16);
  REQUIRE(flanger.parameter_is_realtime_safe(7));
  const std::vector<std::size_t> at =
      moves(read_delays(flanger, kRate, [&] { REQUIRE(flanger.set_parameter(7, 0.0f)); }).left);
  CHECK(static_cast<double>(at.size()) > 0.9 * kReadSeconds * kRate);
  REQUIRE(flanger.set_parameter(7, kStepRateHz));
  REQUIRE_FALSE(flanger.set_parameter(7, std::nanf("")));
}

}  // namespace
