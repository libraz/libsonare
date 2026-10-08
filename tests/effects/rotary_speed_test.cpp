/// @file rotary_speed_test.cpp
/// @brief The rotary's two-speed switch and per-rotor output levels.
///
/// The horn's rate is read off the tremolo it imprints on a tone above the
/// crossover, so the reading covers the path the audio takes and not only the
/// rotor accessor.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

#include "effects/modulation/rotary.h"
#include "support/audio_fixtures.h"

namespace {

using sonare::effects::modulation::Rotary;
using sonare::effects::modulation::RotaryConfig;

constexpr float kHornToneHz = 3000.0f;
constexpr float kDrumToneHz = 60.0f;
constexpr double kSkipS = 0.3;
constexpr double kReadS = 4.0;
constexpr double kRateTolerance = 0.02;

/// Horn tremolo rate in Hz: mean-crossings of the 1 ms peak envelope, first to last.
double horn_rate_from_audio(RotaryConfig config, double sample_rate) {
  config.depth_ms = 0.0f;
  config.tremolo = 1.0f;
  Rotary rotary(config);
  rotary.prepare(sample_rate, 4096);
  const int samples = static_cast<int>((kSkipS + kReadS) * sample_rate);
  std::vector<float> left =
      sonare::test::generate_sine(samples, kHornToneHz, static_cast<int>(sample_rate), 0.5f);
  std::vector<float> right = left;
  sonare::test::process_stereo(rotary, left, right);
  const int window = static_cast<int>(0.001 * sample_rate);
  std::vector<double> env;
  for (int start = static_cast<int>(kSkipS * sample_rate); start + window <= samples;
       start += window) {
    double peak = 0.0;
    for (int i = start; i < start + window; ++i) {
      peak = std::max(peak, static_cast<double>(std::fabs(left[static_cast<std::size_t>(i)])));
    }
    env.push_back(peak);
  }
  double lo = env.front();
  double hi = env.front();
  for (double v : env) {
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  const double mid = 0.5 * (lo + hi);
  double first = -1.0;
  double last = -1.0;
  int crossings = 0;
  for (std::size_t i = 1; i < env.size(); ++i) {
    if (env[i - 1] < mid && env[i] >= mid) {
      const double t =
          (static_cast<double>(i - 1) + (mid - env[i - 1]) / (env[i] - env[i - 1])) * 0.001;
      if (crossings == 0) first = t;
      last = t;
      ++crossings;
    }
  }
  REQUIRE(crossings >= 3);
  return static_cast<double>(crossings - 1) / (last - first);
}

RotaryConfig switched(float speed) {
  RotaryConfig config;
  config.horn_slow_hz = 1.0f;
  config.horn_fast_hz = 7.0f;
  config.drum_slow_hz = 0.5f;
  config.drum_fast_hz = 5.0f;
  config.speed = speed;
  return config;
}

void run_silence(Rotary& rotary, double sample_rate, double seconds) {
  std::vector<float> left(static_cast<std::size_t>(sample_rate * seconds), 0.0f);
  std::vector<float> right = left;
  sonare::test::process_stereo(rotary, left, right);
}

/// RMS of the left plane for a tone through a rotary with the given levels and no modulation.
double leveled_rms(float tone_hz, float horn_db, float drum_db) {
  RotaryConfig config;
  config.depth_ms = 0.0f;
  config.tremolo = 0.0f;
  config.horn_level_db = horn_db;
  config.drum_level_db = drum_db;
  Rotary rotary(config);
  rotary.prepare(sonare::test::kRate, 4096);
  const int samples = static_cast<int>(sonare::test::kRate);
  std::vector<float> left =
      sonare::test::generate_sine(samples, tone_hz, static_cast<int>(sonare::test::kRate), 0.5f);
  std::vector<float> right = left;
  sonare::test::process_stereo(rotary, left, right);
  double sum = 0.0;
  for (std::size_t i = samples / 2; i < left.size(); ++i) sum += double(left[i]) * left[i];
  return std::sqrt(sum / static_cast<double>(left.size() - samples / 2));
}

}  // namespace

TEST_CASE("speed 0 and 1 select each rotor's slow and fast rate", "[rotary-speed]") {
  const double slow = horn_rate_from_audio(switched(0.0f), sonare::test::kRate);
  const double fast = horn_rate_from_audio(switched(1.0f), sonare::test::kRate);
  CHECK(slow == Catch::Approx(1.0).epsilon(kRateTolerance));
  CHECK(fast == Catch::Approx(7.0).epsilon(kRateTolerance));

  Rotary slow_rotary(switched(0.0f));
  slow_rotary.prepare(sonare::test::kRate, 64);
  CHECK(slow_rotary.horn_rate_hz() == 1.0f);
  CHECK(slow_rotary.drum_rate_hz() == 0.5f);
  Rotary fast_rotary(switched(1.0f));
  fast_rotary.prepare(sonare::test::kRate, 64);
  CHECK(fast_rotary.horn_rate_hz() == 7.0f);
  CHECK(fast_rotary.drum_rate_hz() == 5.0f);
}

TEST_CASE("the speed switch replaces the single-rate keys while it is on", "[rotary-speed]") {
  RotaryConfig config = switched(1.0f);
  config.rate_hz = 2.0f;
  config.drum_rate_hz = 2.0f;
  Rotary rotary(config);
  rotary.prepare(sonare::test::kRate, 64);
  CHECK(rotary.horn_rate_hz() == 7.0f);
  CHECK(rotary.drum_rate_hz() == 5.0f);
}

TEST_CASE("speed -1 keeps the single-rate behaviour", "[rotary-speed]") {
  RotaryConfig config = switched(-1.0f);
  Rotary rotary(config);
  rotary.prepare(sonare::test::kRate, 64);
  CHECK(rotary.horn_rate_hz() == config.rate_hz);
  CHECK(rotary.drum_rate_hz() == config.drum_rate_hz);
  CHECK(horn_rate_from_audio(config, sonare::test::kRate) ==
        Catch::Approx(static_cast<double>(config.rate_hz)).epsilon(kRateTolerance));

  REQUIRE(rotary.set_parameter(9, 1.0f));
  run_silence(rotary, sonare::test::kRate, 0.05);
  CHECK(rotary.horn_rate_hz() > config.rate_hz);
  REQUIRE(rotary.set_parameter(9, -1.0f));
  REQUIRE(rotary.set_parameter(0, 3.0f));
  run_silence(rotary, sonare::test::kRate, 0.05);
  CHECK(rotary.horn_rate_hz() == Catch::Approx(3.0f));
}

TEST_CASE("a speed change glides on the accel and decel time constants", "[rotary-speed]") {
  constexpr float kAccelS = 0.4f;
  constexpr float kDecelS = 0.9f;
  RotaryConfig config = switched(0.0f);
  config.accel_tau_s = kAccelS;
  config.decel_tau_s = kDecelS;
  Rotary rotary(config);
  rotary.prepare(sonare::test::kRate, 4096);
  const double one_minus_inv_e = 1.0 - std::exp(-1.0);

  REQUIRE(rotary.set_parameter(9, 1.0f));
  run_silence(rotary, sonare::test::kRate, kAccelS);
  CHECK(rotary.horn_rate_hz() ==
        Catch::Approx(1.0 + 6.0 * one_minus_inv_e).epsilon(kRateTolerance));
  CHECK(rotary.drum_rate_hz() ==
        Catch::Approx(0.5 + 4.5 * one_minus_inv_e).epsilon(kRateTolerance));

  run_silence(rotary, sonare::test::kRate, 10.0 * kAccelS);
  CHECK(rotary.horn_rate_hz() == Catch::Approx(7.0).epsilon(kRateTolerance));

  REQUIRE(rotary.set_parameter(9, 0.0f));
  run_silence(rotary, sonare::test::kRate, kDecelS);
  CHECK(rotary.horn_rate_hz() ==
        Catch::Approx(7.0 - 6.0 * one_minus_inv_e).epsilon(kRateTolerance));
}

TEST_CASE("a speed-up settles short of the fast rate by the undershoot", "[rotary-speed]") {
  RotaryConfig config = switched(0.0f);
  config.accel_tau_s = 0.05f;
  config.undershoot_hz = 0.25f;
  Rotary rotary(config);
  rotary.prepare(sonare::test::kRate, 4096);
  REQUIRE(rotary.set_parameter(9, 1.0f));
  run_silence(rotary, sonare::test::kRate, 2.0);
  CHECK(rotary.horn_rate_hz() == Catch::Approx(6.75f).margin(0.01));
}

TEST_CASE("a speed glide settles on its target at every sample rate", "[rotary-speed]") {
  constexpr float kTauS = 0.5f;
  constexpr float kUndershootHz = 0.25f;
  for (const double rate : {44100.0, 48000.0, 96000.0, 192000.0}) {
    CAPTURE(rate);
    RotaryConfig config = switched(0.0f);
    config.accel_tau_s = kTauS;
    config.decel_tau_s = kTauS;
    Rotary rotary(config);
    rotary.prepare(rate, 4096);

    // Mid-glide the rotor follows the exponential the time constant names.
    REQUIRE(rotary.set_parameter(9, 1.0f));
    run_silence(rotary, rate, kTauS);
    const double samples = std::floor(rate * kTauS);
    const double expected = 7.0 - 6.0 * std::exp(-samples / (static_cast<double>(kTauS) * rate));
    CHECK(rotary.horn_rate_hz() == Catch::Approx(expected).margin(1e-4));

    // Long after it, both directions sit on the target itself, not a rate-dependent offset short.
    run_silence(rotary, rate, 20.0 * kTauS);
    CHECK(rotary.horn_rate_hz() == 7.0f);
    CHECK(rotary.drum_rate_hz() == 5.0f);
    REQUIRE(rotary.set_parameter(9, 0.0f));
    run_silence(rotary, rate, 21.0 * kTauS);
    CHECK(rotary.horn_rate_hz() == 1.0f);
    CHECK(rotary.drum_rate_hz() == 0.5f);

    // The undershoot stays an intentional, exact offset.
    REQUIRE(rotary.set_parameter(17, kUndershootHz));
    REQUIRE(rotary.set_parameter(9, 1.0f));
    run_silence(rotary, rate, 21.0 * kTauS);
    CHECK(rotary.horn_rate_hz() == 7.0f - kUndershootHz);
  }
}

TEST_CASE("the level keys scale each rotor's contribution", "[rotary-speed]") {
  const double horn_ref = leveled_rms(kHornToneHz, -120.0f, -120.0f);
  CHECK(horn_ref < 1e-3);
  const double unity = leveled_rms(kHornToneHz, 0.0f, -120.0f);
  const double quieter = leveled_rms(kHornToneHz, -6.0f, -120.0f);
  CHECK(quieter / unity == Catch::Approx(std::pow(10.0, -6.0 / 20.0)).epsilon(0.01));
  const double louder = leveled_rms(kHornToneHz, 6.0f, -120.0f);
  CHECK(louder / unity == Catch::Approx(std::pow(10.0, 6.0 / 20.0)).epsilon(0.01));

  const double drum_unity = leveled_rms(kDrumToneHz, -120.0f, 0.0f);
  const double drum_quiet = leveled_rms(kDrumToneHz, -120.0f, -6.0f);
  CHECK(drum_quiet / drum_unity == Catch::Approx(std::pow(10.0, -6.0 / 20.0)).epsilon(0.01));

  Rotary rotary;
  CHECK(rotary.set_parameter(10, -3.0f));
  CHECK(rotary.set_parameter(11, -3.0f));
  rotary.prepare(sonare::test::kRate, 256);
  CHECK(rotary.set_parameter(14, 0.0f));
  CHECK(rotary.parameter_is_realtime_safe(14));
}
