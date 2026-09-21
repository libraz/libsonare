/// @file rotary_structure_test.cpp
/// @brief The measured rotary rotor structure: a glide whose time constant is
///        held in seconds, a different constant in each direction, two rotors
///        on independent rates, and the arrival asymmetry -- a rotor sent up to
///        a rate settles a fixed distance short of it, one sent down arrives.
///
/// The expectations are structural quantities (a time constant, a settled
/// distance, a direction) transcribed from measurements of unit
/// roland-sc8850-01, never a reference recording: the rotary's audio is not
/// something this project holds a recording of. Every assertion names the
/// source of its tolerance, and the case reports how many comparisons it made,
/// so a check that stopped reaching the rotor shows up as a count that fell
/// rather than as a run that looks exactly like a pass.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "effects/modulation/rotary.h"
#include "support/audio_fixtures.h"

namespace {

using sonare::effects::modulation::Rotary;
using sonare::effects::modulation::RotaryConfig;
using sonare::test::generate_sine;
using sonare::test::kRate;
using sonare::test::max_abs_difference;
using sonare::test::process_stereo;

// The distance each rotor settles short of a rate it was sent up to. Measured
// at the unit's own power-on acceleration bytes; the archive reports both
// holding to a thousandth of a hertz over the whole of the rate table.
constexpr float kLowRotorShortfallHz = 0.245f;
constexpr float kHighRotorShortfallHz = 0.062f;

// The rotor's control loop shifts by one step of the unit's clock over 2^15,
// and the acceleration byte's top four bits pick how many such steps the gap
// is closed over -- so a divisor is a count of 1.024 s steps.
constexpr double kAccelStepS = 32768.0 / 32000.0;
constexpr double kLowRotorDivisor = 4.0;    // power-on Low Accl byte 24 -> entry 3.
constexpr double kHighRotorDivisor = 16.0;  // power-on Hi Accl byte 88 -> entry 11.

// The acceleration time constant in seconds is this insert's sample-rate-invariant
// quantity, and 3% is the band two rates are required to agree inside.
constexpr double kTauTolerance = 0.03;
// The archive's own resolution on both settled distances.
constexpr double kRateFloorHz = 0.001;

constexpr double kSecondRate = 44100.0;
// Short enough that the ramp is read from its shape rather than from an arrival.
constexpr double kRampSeconds = 0.5;
// A time constant fast enough that 0.3 s is fifteen of them, so an asymptote is
// reached to a ten-thousandth of what the assertions below resolve.
constexpr float kSettleTauS = 0.02f;
constexpr double kSettleSeconds = 0.3;

constexpr int kRenderSamples = 14400;  // 0.3 s at 48000 Hz.

/// Counts every comparison it makes, so the case can report its own reach.
class Tally {
 public:
  void near(double got, double expected, double tolerance, const std::string& what) {
    ++count_;
    INFO(what << ": got " << got << ", expected " << expected << ", tolerance " << tolerance);
    CHECK(std::fabs(got - expected) <= tolerance);
  }

  void same(bool held, const std::string& what) {
    ++count_;
    INFO(what);
    CHECK(held);
  }

  int count() const { return count_; }

 private:
  int count_ = 0;
};

struct Rates {
  float horn;
  float drum;
};

/// Runs `rotary` over `seconds` of silence and reads back where its rotors got
/// to. Silence is enough: the glide is driven by the sample clock, not the signal.
Rates run_for(Rotary& rotary, double sample_rate, double seconds) {
  const int samples = static_cast<int>(std::lround(seconds * sample_rate));
  std::vector<float> left(static_cast<std::size_t>(samples), 0.0f);
  std::vector<float> right(static_cast<std::size_t>(samples), 0.0f);
  float* planes[2] = {left.data(), right.data()};
  rotary.process(planes, 2, samples);
  return {rotary.horn_rate_hz(), rotary.drum_rate_hz()};
}

/// The time constant a first-order glide from `from` toward `to` must have had
/// to be at `now` after `seconds`. Reads the ramp's shape, so it needs no arrival.
double tau_from_ramp(double from, double to, double now, double seconds) {
  return -seconds / std::log((to - now) / (to - from));
}

/// What the horn's time constant measures as at `sample_rate`, from one ramp.
double horn_tau_at(double sample_rate, float tau_s, float from_hz, float to_hz) {
  RotaryConfig config;
  config.rate_hz = from_hz;
  config.accel_tau_s = tau_s;
  config.decel_tau_s = tau_s;
  Rotary rotary(config);
  rotary.prepare(sample_rate, 4096);
  rotary.set_parameter(0, to_hz);
  const Rates after = run_for(rotary, sample_rate, kRampSeconds);
  return tau_from_ramp(from_hz, to_hz, after.horn, kRampSeconds);
}

/// A fixed two-tone signal, deterministic and identical on both channels.
std::vector<float> test_signal() {
  const std::vector<float> a =
      generate_sine(kRenderSamples, 220.0f, static_cast<int>(kRate), 0.25f);
  const std::vector<float> b =
      generate_sine(kRenderSamples, 330.0f, static_cast<int>(kRate), 0.15f);
  std::vector<float> sum(a.size());
  for (std::size_t i = 0; i < sum.size(); ++i) sum[i] = a[i] + b[i];
  return sum;
}

/// Renders that signal through a rotary built from `config` and returns the left plane.
std::vector<float> render(RotaryConfig config) {
  Rotary rotary(config);
  rotary.prepare(kRate, kRenderSamples);
  std::vector<float> left = test_signal();
  std::vector<float> right = test_signal();
  process_stereo(rotary, left, right);
  return left;
}

}  // namespace

// One case rather than four sections: Catch2 runs a section per pass, so a tally
// declared across sections would report a quarter of its reach four times.
TEST_CASE("the rotary's rotors glide on a time constant held in seconds", "[rotary-structure]") {
  Tally tally;

  // The time constant is a duration, so two sample rates read it the same.
  {
    const auto tau_s = static_cast<float>(kLowRotorDivisor * kAccelStepS);
    const double at_48k = horn_tau_at(kRate, tau_s, 1.0f, 8.0f);
    const double at_44k1 = horn_tau_at(kSecondRate, tau_s, 1.0f, 8.0f);

    tally.near(at_48k, tau_s, kTauTolerance * tau_s, "accel tau at 48000 Hz");
    tally.near(at_44k1, tau_s, kTauTolerance * tau_s, "accel tau at 44100 Hz");
    tally.near(at_44k1, at_48k, kTauTolerance * at_48k, "accel tau across the two rates");

    // The other rotor's power-on entry, four times the first: the same ramp read
    // four times slower is what says the constant is carried and not hardcoded.
    const auto slow_tau_s = static_cast<float>(kHighRotorDivisor * kAccelStepS);
    const double slow_at_48k = horn_tau_at(kRate, slow_tau_s, 1.0f, 8.0f);
    tally.near(slow_at_48k, slow_tau_s, kTauTolerance * slow_tau_s,
               "the sixteen-step entry's tau at 48000 Hz");
    tally.near(slow_at_48k / at_48k, kHighRotorDivisor / kLowRotorDivisor,
               kTauTolerance * kHighRotorDivisor / kLowRotorDivisor,
               "the two entries' taus stand in their divisors' ratio");
  }

  // Speeding up settles short by the measured distance; slowing down arrives.
  {
    constexpr float kEntryHz = 6.0f;

    RotaryConfig climbing;
    climbing.rate_hz = 1.0f;
    climbing.drum_rate_hz = 1.0f;
    climbing.accel_tau_s = kSettleTauS;
    climbing.decel_tau_s = kSettleTauS;
    climbing.undershoot_hz = kHighRotorShortfallHz;
    climbing.drum_undershoot_hz = kLowRotorShortfallHz;
    Rotary up(climbing);
    up.prepare(kRate, 4096);
    up.set_parameter(0, kEntryHz);
    up.set_parameter(4, kEntryHz);
    const Rates from_below = run_for(up, kRate, kSettleSeconds);

    RotaryConfig falling = climbing;
    falling.rate_hz = 9.0f;
    falling.drum_rate_hz = 9.0f;
    Rotary down(falling);
    down.prepare(kRate, 4096);
    down.set_parameter(0, kEntryHz);
    down.set_parameter(4, kEntryHz);
    const Rates from_above = run_for(down, kRate, kSettleSeconds);

    tally.near(from_below.horn, kEntryHz - kHighRotorShortfallHz, kRateFloorHz,
               "high rotor sent up to the entry");
    tally.near(from_below.drum, kEntryHz - kLowRotorShortfallHz, kRateFloorHz,
               "low rotor sent up to the entry");
    tally.near(from_above.horn, kEntryHz, kRateFloorHz, "high rotor sent down onto the entry");
    tally.near(from_above.drum, kEntryHz, kRateFloorHz, "low rotor sent down onto the entry");
    tally.near(from_above.horn - from_below.horn, kHighRotorShortfallHz, kRateFloorHz,
               "the high rotor's two arrivals differ by its own distance");
    tally.near(from_above.drum - from_below.drum, kLowRotorShortfallHz, kRateFloorHz,
               "the low rotor's two arrivals differ by its own distance");
    // A tolerance this loose would let the descent pass by sitting on the climb's
    // asymptote, so the two arrivals have to be further apart than it is wide.
    tally.same(from_above.drum - from_below.drum > 200.0 * kRateFloorHz,
               "the arrival asymmetry is wider than the tolerance that reads it");

    // Where the entry is nearer than the distance, the rotor does not set off --
    // which is what collapses the bottom of the low rotor's table onto one rate.
    RotaryConfig nearby = climbing;
    nearby.drum_rate_hz = kEntryHz;
    Rotary barely(nearby);
    barely.prepare(kRate, 4096);
    barely.set_parameter(4, kEntryHz + 0.5f * kLowRotorShortfallHz);
    const Rates unmoved = run_for(barely, kRate, kSettleSeconds);
    tally.near(unmoved.drum, kEntryHz, kRateFloorHz,
               "a gap narrower than the distance moves the low rotor nowhere");
  }

  // The two rotors run at independent rates.
  {
    const RotaryConfig defaults;
    tally.same(defaults.drum_rate_hz == defaults.rate_hz * 0.74f,
               "the defaults stand in the ratio the pair has always run at");

    RotaryConfig split;
    split.rate_hz = 7.0f;
    split.drum_rate_hz = 2.0f;
    split.accel_tau_s = kSettleTauS;
    split.decel_tau_s = kSettleTauS;
    Rotary rotary(split);
    rotary.prepare(kRate, 4096);
    tally.same(rotary.horn_rate_hz() == 7.0f && rotary.drum_rate_hz() == 2.0f,
               "each rotor comes up on its own configured rate");

    rotary.set_parameter(4, 5.0f);
    const Rates after = run_for(rotary, kRate, kSettleSeconds);
    tally.near(after.drum, 5.0, kRateFloorHz, "the aimed rotor arrives");
    tally.same(after.horn == 7.0f, "the other rotor does not move with it");

    // Both rates have to reach the audio, not only the accessors above.
    RotaryConfig faster_drum = split;
    faster_drum.drum_rate_hz = 5.0f;
    RotaryConfig faster_horn = split;
    faster_horn.rate_hz = 9.0f;
    tally.same(max_abs_difference(render(split), render(faster_drum)) > 0.0f,
               "moving the drum rotor alone changes the rendered audio");
    tally.same(max_abs_difference(render(split), render(faster_horn)) > 0.0f,
               "moving the horn rotor alone changes the rendered audio");
  }

  // Speeding up and slowing down run on different time constants.
  {
    constexpr float kFastTauS = 0.5f;
    constexpr float kSlowTauS = 4.0f;
    constexpr float kRestHz = 1.0f;
    constexpr float kAimHz = 6.0f;
    constexpr double kLegSeconds = 0.25;

    RotaryConfig config;
    config.rate_hz = kRestHz;
    config.accel_tau_s = kFastTauS;
    config.decel_tau_s = kSlowTauS;
    Rotary rotary(config);
    rotary.prepare(kRate, 4096);

    rotary.set_parameter(0, kAimHz);
    const Rates climbed = run_for(rotary, kRate, kLegSeconds);
    const double up = tau_from_ramp(kRestHz, kAimHz, climbed.horn, kLegSeconds);

    rotary.set_parameter(0, kRestHz);
    const Rates fell = run_for(rotary, kRate, kLegSeconds);
    const double down = tau_from_ramp(climbed.horn, kRestHz, fell.horn, kLegSeconds);

    tally.near(up, kFastTauS, kTauTolerance * kFastTauS, "the climbing time constant");
    tally.near(down, kSlowTauS, kTauTolerance * kSlowTauS, "the falling time constant");
    tally.near(down / up, static_cast<double>(kSlowTauS / kFastTauS),
               kTauTolerance * kSlowTauS / kFastTauS,
               "the two directions stand in the ratio they were given");
  }

  WARN("comparisons: " << tally.count());
}
