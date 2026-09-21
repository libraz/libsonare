/// @file delay_damping_test.cpp
/// @brief The measured stereo-delay damping: a one-pole moving-average filter
///        inside the feedback loop, distinguishable from an outside-the-loop
///        placement by its band-dependent decay.
///
/// Every expectation is a structural quantity -- how a held note's build
/// depends on frequency, whether a skirt levels off or keeps steepening, where
/// a half-power corner lands at two sample rates. None is a recorded waveform,
/// and none could be: the contract this module keeps is the control protocol
/// and not the audio (src/midi/synth/docs/gs.md).
///
/// The load-bearing case is the placement. An outside-the-loop filter applies
/// the same shape to both windows of one take and so cancels out of their
/// ratio; an inside-the-loop one puts a fresh helping into every pass, so the
/// ratio falls away above the corner and the frequency it hinges on travels
/// with the corner. A case that only checked "the output got darker" would
/// pass for either placement.
///
/// Reach is an output: the case reports how many comparisons it made, because
/// a run that compared nothing looks exactly like a run that passed.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "effects/delay/stereo_delay.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"

namespace {

using sonare::constants::kEpsilon;
using sonare::constants::kSqrt2D;
using sonare::constants::kTwoPiD;
using sonare::effects::delay::StereoDelay;
using sonare::effects::delay::StereoDelayConfig;
using sonare::rt::ParamDescriptor;
using sonare::test::power_spectrum;
using sonare::test::process_stereo;

/// Counts every comparison it makes, so the case can report its own reach.
class Tally {
 public:
  void near(double got, double reading, double tolerance, const std::string& what) {
    ++count_;
    INFO(what << ": got " << got << ", expected " << reading << ", tolerance " << tolerance);
    CHECK(std::fabs(got - reading) <= tolerance);
  }

  void at_least(double got, double bound, const std::string& what) {
    ++count_;
    INFO(what << ": got " << got << ", needs at least " << bound);
    CHECK(got >= bound);
  }

  void at_most(double got, double bound, const std::string& what) {
    ++count_;
    INFO(what << ": got " << got << ", needs at most " << bound);
    CHECK(got <= bound);
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

// Corners the byte was read at, in Hz. Each is an entry of the third-octave
// series its page prints against this parameter, and each was also fitted
// freely from that setting's own skirt, landing on the series to a median
// 0.0022 octaves and a worst of 0.0049.
constexpr float kLowCornerHz = 315.0f;
constexpr float kMidCornerHz = 1250.0f;
constexpr float kHighCornerHz = 5000.0f;
constexpr float kTopCornerHz = 8000.0f;

// Worst distance, in octaves, between a corner fitted from a skirt and the
// printed series. Used as the tolerance wherever two readings of one pole are
// held against each other.
constexpr double kCornerFitOctaves = 0.0049;

// The floor the band-dependence reading stands on, in dB. The departure it
// resolved stood 9.18 dB over this -- but in a loop whose gain the reading does
// not publish, and the size of a build is a figure about that gain, so the
// floor is what carries over and the 9.18 is not.
constexpr double kReadingFloorDb = 0.41;

// Margin a departure has to clear the floor by before two placements count as
// separated. The reading's own stood twenty-two times its floor.
constexpr double kSeparationMargin = 10.0;

// How flat "the same number in every band" was actually read: the filter-out
// control builds by a median 18.3 dB with per-band readings spanning 17.33 to
// 20.75, so 3.42 dB is the spread that reading carries.
constexpr double kControlSpreadDb = 3.42;

// The two rates the insert has to hold a corner across, and the tolerance the
// corner is owed between them.
constexpr double kHostRate = 48000.0;
constexpr double kAltHostRate = 44100.0;
constexpr double kCornerRateTolerance = 0.03;

// Loop the band dependence is read through: short taps, so a half-second take
// carries thirty passes, and the feedback near the top of its range, which is
// the state the reading was taken in so that there are passes to compound over.
constexpr float kLoopFeedback = 0.95f;
constexpr int kLoopSamples = 24000;  // 0.5 s at 48 kHz.
constexpr int kBandFft = 2048;
constexpr std::size_t kEarlyWindow = 1100;  // Past the longest first tap.
constexpr std::size_t kLateWindow = 21952;  // The last window of the take.

// Ten loop lengths, read together. One feedback loop puts a comb over the
// spectrum, and a band narrow enough to be a band holds few enough teeth that
// the comb, not the damping, would decide it; ten combs of different spacing
// average out where one does not. Each is a whole number of samples at the rate
// below, so the line's interpolator is not a second loss inside the same loop.
constexpr std::array<std::array<float, 2>, 5> kDelayPairsMs{
    {{10.0f, 13.0f}, {11.5f, 14.75f}, {13.25f, 16.875f}, {15.25f, 19.25f}, {17.5f, 22.0f}}};

// Octave band centres the build is read in.
constexpr std::array<double, 6> kBandCentresHz{250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0};

/// Deterministic broadband source. A tone set cannot serve here: the build has
/// to be read in bands, and a band needs comb teeth inside it to average.
std::vector<float> noise(int n) {
  std::vector<float> out(static_cast<std::size_t>(n));
  std::uint32_t state = 0x2545f491u;
  for (std::size_t i = 0; i < out.size(); ++i) {
    state = state * 1664525u + 1013904223u;
    out[i] = static_cast<float>(static_cast<double>(state >> 8) / 8388608.0 - 1.0) * 0.2f;
  }
  return out;
}

/// The damping written the way the reading names it: one multiply, one state,
/// no zero at Nyquist. The processor carries its own copy inside the loop; this
/// one exists to build the after-the-loop comparator, so that the placement is
/// the only thing that differs between the two renders.
class RunningAveragePole {
 public:
  RunningAveragePole(double corner_hz, double sample_rate)
      : gain_(static_cast<float>(1.0 - std::exp(-kTwoPiD * corner_hz / sample_rate))) {}

  float process(float x) noexcept {
    state_ += gain_ * (x - state_);
    return state_;
  }

 private:
  float gain_;
  float state_ = 0.0f;
};

StereoDelayConfig loop_config(float left_ms, float right_ms, float damping_hz) {
  StereoDelayConfig config;
  config.delay_time_l_ms = left_ms;
  config.delay_time_r_ms = right_ms;
  config.feedback = kLoopFeedback;
  config.dry_wet = 1.0f;
  config.damping_hz = damping_hz;
  return config;
}

/// Zero delay, no feedback and a fully wet mix, so the insert's output is the
/// damping stage alone: the line reads back the sample it just wrote, and the
/// mix and feedback smoothers rest at their targets from the first sample.
StereoDelayConfig bare_config(float damping_hz) {
  StereoDelayConfig config;
  config.delay_time_l_ms = 0.0f;
  config.delay_time_r_ms = 0.0f;
  config.feedback = 0.0f;
  config.dry_wet = 1.0f;
  config.damping_hz = damping_hz;
  return config;
}

struct Stereo {
  std::vector<float> left;
  std::vector<float> right;
};

Stereo render_stereo(const StereoDelayConfig& config, double sample_rate,
                     const std::vector<float>& input) {
  StereoDelay processor(config);
  processor.prepare(sample_rate, static_cast<int>(input.size()));
  Stereo out{input, input};
  process_stereo(processor, out.left, out.right);
  return out;
}

std::vector<float> render(const StereoDelayConfig& config, double sample_rate,
                          const std::vector<float>& input) {
  return render_stereo(config, sample_rate, input).left;
}

/// Band power of @p buf over one octave around @p centre_hz, linear.
double band_power(const std::vector<float>& buf, std::size_t from, double centre_hz,
                  double sample_rate) {
  const std::vector<double> power = power_spectrum(buf, from, kBandFft);
  const double bin_hz = sample_rate / kBandFft;
  const auto lo = static_cast<std::size_t>(std::ceil(centre_hz / kSqrt2D / bin_hz));
  const auto hi = static_cast<std::size_t>(std::floor(centre_hz * kSqrt2D / bin_hz));
  double sum = 0.0;
  for (std::size_t b = lo; b <= hi && b < power.size(); ++b) sum += power[b];
  return sum;
}

using BandProfile = std::array<double, kBandCentresHz.size()>;

/// How much a held note's level grows between an early and a late window of one
/// take, per band, in dB, gathered over every loop length. This is the quantity
/// the placement is read off: @p in_loop_hz drives the insert's own damping,
/// @p after_loop_hz the same filter applied once to what came out of it.
BandProfile build_db(double sample_rate, float in_loop_hz, float after_loop_hz,
                     const std::vector<float>& source) {
  BandProfile early{};
  BandProfile late{};
  for (const auto& pair : kDelayPairsMs) {
    Stereo out = render_stereo(loop_config(pair[0], pair[1], in_loop_hz), sample_rate, source);
    if (after_loop_hz > 0.0f) {
      RunningAveragePole post_left(after_loop_hz, sample_rate);
      RunningAveragePole post_right(after_loop_hz, sample_rate);
      for (float& s : out.left) s = post_left.process(s);
      for (float& s : out.right) s = post_right.process(s);
    }
    for (const std::vector<float>* channel : {&out.left, &out.right}) {
      for (std::size_t i = 0; i < kBandCentresHz.size(); ++i) {
        early[i] += band_power(*channel, kEarlyWindow, kBandCentresHz[i], sample_rate);
        late[i] += band_power(*channel, kLateWindow, kBandCentresHz[i], sample_rate);
      }
    }
  }
  BandProfile out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = 10.0 * std::log10((late[i] + kEpsilon) / (early[i] + kEpsilon));
  }
  return out;
}

/// Steady-state gain of the damping at @p freq_hz, in dB, read off the insert
/// itself through bare_config. The probe is an exact whole number of cycles in
/// the window it is projected over, so nothing leaks between bins.
double damping_gain_db(double sample_rate, float damping_hz, double freq_hz) {
  constexpr int kSettle = 4000;
  constexpr int kCycles = 24;
  const int n = std::max(16, static_cast<int>(std::lround(kCycles * sample_rate / freq_hz)));
  const double f = kCycles * sample_rate / n;
  std::vector<float> input(static_cast<std::size_t>(kSettle + n));
  for (std::size_t i = 0; i < input.size(); ++i) {
    input[i] = static_cast<float>(std::sin(kTwoPiD * f * static_cast<double>(i) / sample_rate));
  }
  const std::vector<float> out = render(bare_config(damping_hz), sample_rate, input);
  double re = 0.0;
  double im = 0.0;
  for (int i = 0; i < n; ++i) {
    const double phase = kTwoPiD * kCycles * i / n;
    const double y = out[static_cast<std::size_t>(kSettle + i)];
    re += y * std::cos(phase);
    im -= y * std::sin(phase);
  }
  const double amplitude = 2.0 * std::hypot(re, im) / n;
  return 20.0 * std::log10(std::max(amplitude, static_cast<double>(kEpsilon)));
}

/// The damping's own half-power corner in Hz, bracketed on a sixteenth-octave
/// grid and interpolated in log frequency. Returns 0 when the grid never
/// crossed, which the caller reports rather than absorbing.
double measured_corner_hz(double sample_rate, float damping_hz) {
  constexpr double kHalfPowerDb = -3.0102999566398120;
  constexpr int kPoints = 49;
  double prev_f = 0.0;
  double prev_db = 0.0;
  for (int i = 0; i < kPoints; ++i) {
    const double f = damping_hz * std::pow(2.0, -1.5 + 3.0 * i / (kPoints - 1));
    if (f > 0.45 * sample_rate) break;
    const double db = damping_gain_db(sample_rate, damping_hz, f);
    if (i > 0 && prev_db > kHalfPowerDb && db <= kHalfPowerDb) {
      const double t = (kHalfPowerDb - prev_db) / (db - prev_db);
      return std::exp(std::log(prev_f) + t * (std::log(f) - std::log(prev_f)));
    }
    prev_f = f;
    prev_db = db;
  }
  return 0.0;
}

/// Steady-state gain at exactly half the sample rate, driven by the alternating
/// signal that lives there. A pole put through the bilinear transform carries a
/// zero here and answers exactly nothing; a running average answers
/// (1 - pole) / (1 + pole), which is what names the pole with nothing fitted.
double nyquist_gain(double sample_rate, float damping_hz) {
  constexpr int kSettle = 8000;
  constexpr int kRead = 16;
  std::vector<float> input(static_cast<std::size_t>(kSettle + kRead));
  for (std::size_t i = 0; i < input.size(); ++i) input[i] = (i % 2 == 0) ? 1.0f : -1.0f;
  const std::vector<float> out = render(bare_config(damping_hz), sample_rate, input);
  double sum = 0.0;
  for (int i = 0; i < kRead; ++i) sum += std::fabs(out[static_cast<std::size_t>(kSettle + i)]);
  return sum / kRead;
}

/// Half-power corner of a running average whose pole is @p pole, in Hz, solved
/// exactly rather than through the small-angle approximation. Returns 0 when
/// the pole is so open that the response never falls half a power.
double corner_of_pole(double pole, double sample_rate) {
  const double a = 1.0 - pole;
  const double cos_w = 1.0 - a * a / (2.0 * pole);
  if (!(cos_w > -1.0) || !(cos_w < 1.0)) return 0.0;
  return std::acos(cos_w) * sample_rate / kTwoPiD;
}

struct Recovery {
  bool poison_reached_the_loop;
  bool last_block_finite;
};

/// Drives the insert over several blocks with one infinity planted in the
/// first. The damping cell sits inside the feedback path, so a poisoned one
/// recirculates for good unless the block's own discard covers it -- and the
/// sibling recovery cases leave this field at its bypass, so nothing else in
/// the tree drives the filter while the loop is poisoned.
Recovery recovers_from_non_finite(const StereoDelayConfig& config, double sample_rate) {
  constexpr int kBlock = 512;
  constexpr int kBlocks = 8;
  const auto all_finite = [](const std::vector<float>& buf) {
    return std::all_of(buf.begin(), buf.end(), [](float s) { return std::isfinite(s); });
  };
  StereoDelay processor(config);
  processor.prepare(sample_rate, kBlock);
  const std::vector<float> source = noise(kBlock);
  Recovery out{false, false};
  for (int block = 0; block < kBlocks; ++block) {
    std::vector<float> left = source;
    std::vector<float> right = source;
    if (block == 0) {
      left[kBlock / 2] = std::numeric_limits<float>::infinity();
      right[kBlock / 2] = std::numeric_limits<float>::infinity();
    }
    process_stereo(processor, left, right);
    if (block == 0) out.poison_reached_the_loop = !all_finite(left) && !all_finite(right);
    if (block + 1 == kBlocks) out.last_block_finite = all_finite(left) && all_finite(right);
  }
  return out;
}

/// The most negative entry of a departure profile, in dB.
double deepest(const BandProfile& departure) {
  double out = 0.0;
  for (const double d : departure) out = std::min(out, d);
  return out;
}

/// Index of the lowest band where a departure profile has fallen through half
/// its own deepest value. The reading takes this as a bracket between two bands
/// rather than interpolating, and what it is for is that it moves.
std::size_t half_depth_band(const BandProfile& departure) {
  const double half = 0.5 * deepest(departure);
  for (std::size_t i = 0; i < departure.size(); ++i) {
    if (departure[i] <= half) return i;
  }
  return departure.size();
}

}  // namespace

TEST_CASE("the stereo delay's in-loop damping matches the measured band-dependent structure",
          "[delay-damping]") {
  Tally tally;
  const std::vector<float> source = noise(kLoopSamples);

  // --- the placement is read off how the build depends on the band ----------
  // A filter on the way out is the same shape in both windows of one take and
  // cancels between them; one inside the loop puts a helping into every pass.
  const BandProfile control_build = build_db(kHostRate, 0.0f, 0.0f, source);
  const BandProfile in_loop_build = build_db(kHostRate, kHighCornerHz, 0.0f, source);
  const BandProfile after_loop_build = build_db(kHostRate, 0.0f, kHighCornerHz, source);
  const BandProfile low_corner_build = build_db(kHostRate, kLowCornerHz, 0.0f, source);

  // The control this reading needs: with no filter anywhere the loop builds by
  // one number of decibels and the same number in every band.
  double lowest = control_build[0];
  double highest = control_build[0];
  for (const double b : control_build) {
    lowest = std::min(lowest, b);
    highest = std::max(highest, b);
  }
  tally.at_most(highest - lowest, kControlSpreadDb,
                "filter-out control builds by the same number in every band");

  BandProfile in_loop_departure{};
  BandProfile low_corner_departure{};
  double worst_null_db = 0.0;
  for (std::size_t i = 0; i < kBandCentresHz.size(); ++i) {
    const std::string band = std::to_string(static_cast<int>(kBandCentresHz[i])) + " Hz";
    in_loop_departure[i] = in_loop_build[i] - control_build[i];
    low_corner_departure[i] = low_corner_build[i] - control_build[i];
    const double null_db = std::fabs(after_loop_build[i] - control_build[i]);
    worst_null_db = std::max(worst_null_db, null_db);
    tally.at_most(null_db, kReadingFloorDb,
                  "after-the-loop damping cancels out of the build at " + band);
    tally.at_most(in_loop_departure[i], kReadingFloorDb,
                  "in-loop damping does not lift the build at " + band);
  }

  // Inside the loop the build falls away as the band climbs, which is the
  // direction the reading names; outside it there is no direction to have.
  const std::size_t top = kBandCentresHz.size() - 1;
  tally.at_least(in_loop_departure[0] - in_loop_departure[top], kSeparationMargin * kReadingFloorDb,
                 "the in-loop departure deepens from the bottom band to the top");
  tally.at_most(std::fabs(after_loop_build[0] - after_loop_build[top] -
                          (control_build[0] - control_build[top])),
                kReadingFloorDb, "the after-the-loop departure has no such slope");

  // The separation itself, held against the floor the reading stands on and
  // against the null this run measured for itself. A case that only watched the
  // output darken would pass for either placement: darkening is what both do.
  tally.at_least(std::fabs(in_loop_build[top] - after_loop_build[top]),
                 kSeparationMargin * std::max(kReadingFloorDb, worst_null_db),
                 "in-loop and after-the-loop builds separate at the top band");

  // And where the departure hinges travels with the corner -- the other thing
  // an outside filter cannot do, its departure being zero at every corner. A
  // hinge is only there to move once there is a departure to have one, so each
  // profile has to stand clear of the floor before the two are compared.
  tally.at_most(deepest(in_loop_departure), -kSeparationMargin * kReadingFloorDb,
                "the departure at the high corner is deep enough to carry a hinge");
  tally.at_most(deepest(low_corner_departure), -kSeparationMargin * kReadingFloorDb,
                "the departure at the low corner is deep enough to carry a hinge");
  tally.at_least(static_cast<double>(half_depth_band(in_loop_departure)),
                 static_cast<double>(half_depth_band(low_corner_departure)) + 1.0,
                 "the band the departure hinges in climbs with the corner");

  // --- the skirt levels off instead of carrying a zero at Nyquist -----------
  for (const float corner : {kLowCornerHz, kMidCornerHz, kHighCornerHz}) {
    const std::string at = "corner " + std::to_string(static_cast<int>(corner)) + " Hz";
    const double gain = nyquist_gain(kHostRate, corner);
    // A bilinear pole's zero sits exactly here and answers zero.
    tally.at_least(gain, 0.01, "the response at half the rate is not a zero, " + at);

    // The depth names the pole with nothing fitted: depth = (1 - pole) /
    // (1 + pole). Inverting it and predicting the half-power corner has to land
    // on the corner read off the skirt -- one pole explaining both readings,
    // which is what separates this form from the bilinear one it beat.
    const double pole = (1.0 - gain) / (1.0 + gain);
    const double from_depth = corner_of_pole(pole, kHostRate);
    const double from_skirt = measured_corner_hz(kHostRate, corner);
    tally.at_least(from_skirt, 1.0, "the skirt crosses half power inside the grid, " + at);
    tally.at_most(std::fabs(std::log2(from_depth / from_skirt)), kCornerFitOctaves,
                  "the depth and the skirt name one pole, " + at);
  }

  // --- the corner is held in Hz, not as a coefficient -----------------------
  for (const float corner : {kLowCornerHz, kMidCornerHz, kHighCornerHz, kTopCornerHz}) {
    const std::string at = "corner " + std::to_string(static_cast<int>(corner)) + " Hz";
    const double host = measured_corner_hz(kHostRate, corner);
    const double alt = measured_corner_hz(kAltHostRate, corner);
    tally.at_least(host, 1.0, "the corner is readable at 48000 Hz, " + at);
    tally.at_least(alt, 1.0, "the corner is readable at 44100 Hz, " + at);
    tally.at_most(std::fabs(host - alt) / (0.5 * (host + alt)), kCornerRateTolerance,
                  "the corner lands at the same frequency at both rates, " + at);
  }

  // --- the damping cell is returned to rest with the rest of the loop -------
  const Recovery recovery = recovers_from_non_finite(
      loop_config(kDelayPairsMs[0][0], kDelayPairsMs[0][1], kMidCornerHz), kHostRate);
  tally.same(recovery.poison_reached_the_loop, "the planted infinity reached the loop at all");
  tally.same(recovery.last_block_finite, "and the loop is finite again a few blocks later");

  // --- the default leaves the signal untouched, sample for sample -----------
  const StereoDelayConfig defaults;
  tally.same(defaults.damping_hz == 0.0f, "the config's damping defaults to the bypass");

  // With the line at zero samples and the feedback out, every stage but the
  // damping is an identity, so a bypassed damping has to give the input back
  // bit for bit. A gain of one would not: s + 1 * (x - s) is not x.
  tally.same(render(bare_config(0.0f), kHostRate, source) == source,
             "the bypassed damping returns the input unchanged");
  tally.same(render(bare_config(kMidCornerHz), kHostRate, source) != source,
             "a corner in range does change the input");

  StereoDelayConfig explicit_bypass;
  explicit_bypass.damping_hz = 0.0f;
  tally.same(
      render(StereoDelayConfig{}, kHostRate, source) == render(explicit_bypass, kHostRate, source),
      "the default config and an explicit bypass render identically");

  // --- the corner is automatable, and automating it to zero really bypasses --
  {
    StereoDelay processor{bare_config(0.0f)};
    processor.prepare(kHostRate, kLoopSamples);

    const std::vector<ParamDescriptor> descriptors = processor.parameter_descriptors();
    const auto damping =
        std::find_if(descriptors.begin(), descriptors.end(),
                     [](const ParamDescriptor& d) { return d.key == "dampingHz"; });
    tally.same(damping != descriptors.end(), "the corner is published as a parameter");
    REQUIRE(damping != descriptors.end());
    const unsigned int id = damping->id;
    tally.same(processor.parameter_is_realtime_safe(id),
               "and is applicable from the audio callback, being one coefficient in place");
    tally.same(!processor.set_parameter(id, std::numeric_limits<float>::quiet_NaN()),
               "a corner no coefficient exists for is refused rather than stored");

    // Automating the corner in has to take the filter, and automating it back
    // to zero has to re-take the branch that skips it -- not leave a filter
    // running at unit gain, which is not the bypass it looks like.
    tally.same(processor.set_parameter(id, kMidCornerHz), "the corner is accepted live");
    std::vector<float> live = source;
    std::vector<float> live_right = source;
    process_stereo(processor, live, live_right);
    tally.same(live != source, "and the filter is running on the very next block");

    tally.same(processor.set_parameter(id, 0.0f), "zero is accepted live");
    std::vector<float> bypassed = source;
    std::vector<float> bypassed_right = source;
    process_stereo(processor, bypassed, bypassed_right);
    tally.same(bypassed == source, "and the path is a true bypass again, sample for sample");
  }

  WARN("comparisons: " << tally.count());
  REQUIRE(tally.count() >= 40);
}
