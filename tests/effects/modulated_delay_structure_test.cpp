/// @file modulated_delay_structure_test.cpp
/// @brief The measured chorus/flanger front end -- one one-pole section offered
///        in two shapes, run per instance -- and the flanger loop whose sign
///        moves the teeth by half a spacing.
///
/// Expectations are structural: a band profile, a slope in decibels per octave,
/// where a comb's teeth sit relative to the other sign's, and a corner held in
/// hertz across two rates. None is a recorded waveform, and none could be: the
/// contract this module keeps is the control protocol and not the audio
/// (src/midi/synth/docs/gs.md).
///
/// The archive publishes the band profiles and states outright that it fits no
/// filter, no corner and no order to them. So the corner beside each profile
/// below is a free parameter of this comparison rather than a reading, and what
/// carries the shape is the control: the same profile refitted as two sections
/// in cascade, which lands three to six times further out.
///
/// Reach is an output: the case reports how many comparisons it made, because a
/// run that compared nothing looks exactly like a run that passed.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <string>
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
using sonare::effects::modulation::PreFilterMode;

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

// 0.51 s at 32 kHz. The bottom band the profiles reach is 24.8 Hz, which needs
// to sit well clear of DC, and the deepest loop used here rings out long before
// the window ends.
constexpr int kFftLength = 16384;

// The rate the profiles below were measured at, and the two rates the section's
// corner has to hold across.
constexpr double kMachineRate = 32000.0;
constexpr double kHostRate = 48000.0;
constexpr double kSecondHostRate = 44100.0;

// A published band reading: the centre of a twelfth-octave band and the level
// the archive reports for it, against the same chain with the byte flat.
struct BandReading {
  double hz;
  double db;
};

// Stereo chorus, pre-filter set to its low pass, cutoff byte at the bottom of
// its range. Decimated to a half octave over 100 Hz to 8 kHz -- above that the
// record's own limits put the profile past where the band set could follow it
// (its largest deviation lands in the top three bands and its settling figure
// never reaches zero), so those bands are not readings of the section.
constexpr BandReading kLowPassBottomByte[] = {
    {105.1, -0.75},   {148.7, -1.30},   {210.2, -2.24},   {297.3, -3.86},   {420.4, -5.88},
    {594.6, -8.34},   {840.9, -10.98},  {1189.2, -13.87}, {1681.8, -16.86}, {2378.4, -19.93},
    {3363.6, -23.07}, {4756.8, -26.38}, {6727.2, -30.11},
};

// The same profile with the cutoff byte at the middle of its range.
constexpr BandReading kLowPassMiddleByte[] = {
    {105.1, -0.65},   {148.7, -0.79},   {210.2, -0.99},   {297.3, -1.39},  {420.4, -2.06},
    {594.6, -3.27},   {840.9, -4.89},   {1189.2, -7.13},  {1681.8, -9.68}, {2378.4, -12.49},
    {3363.6, -15.53}, {4756.8, -18.77}, {6727.2, -22.48},
};

// The second shape the same byte offers, at the bottom of the cutoff range.
// Read from 35 Hz up: below that the published readings stop falling and run
// back and forth, which is the stimulus rather than the section.
constexpr BandReading kHighPassBottomByte[] = {
    {35.1, -14.54}, {49.6, -12.82}, {70.2, -10.84},  {99.2, -7.83},
    {140.3, -6.02}, {198.4, -4.03}, {280.6, -2.38},  {396.9, -1.42},
    {561.2, -0.68}, {793.7, -0.46}, {1122.5, -0.14}, {1587.4, -0.07},
};

// Corners that fit each published profile best under a single pole, and under
// two in cascade. Neither is a reading -- the record fits no corner -- and the
// pair is here so the shape can be told from the fit: the second-order column
// is the control.
constexpr float kLowPassBottomCornerHz = 234.5f;
constexpr float kLowPassMiddleCornerHz = 546.8f;
constexpr float kHighPassBottomCornerHz = 212.2f;
constexpr double kLowPassBottomControlHz = 817.2;
constexpr double kLowPassMiddleControlHz = 1469.4;
constexpr double kHighPassBottomControlHz = 93.3;

// How far a single section is allowed to sit from a published profile, per band
// and over the whole of one. The control has to clear the second figure, which
// is what says these two are reading the shape rather than the fit.
constexpr double kBandTolerance = 1.8;
constexpr double kProfileRmsTolerance = 0.8;
constexpr double kControlRmsFloor = 1.2;
constexpr double kControlBandFloor = 2.5;

// The two types' front ends agree to this median over the published bands, at
// the runs' own repeatability spread, which is the same figure.
constexpr double kSharedSectionDb = 0.08;

// The slopes the archive publishes for the high-pass shape, read on its own
// twelfth-octave grid over a one-octave window: eight readings across the two
// types and four cutoff settings. Its low-pass counterpart is not usable the
// same way -- it lands at 9 to 11 kHz, where the profile had not settled and
// the top band runs back up -- so only this shape carries a published slope.
constexpr double kHighPassSlopeLow = 5.46;
constexpr double kHighPassSlopeHigh = 6.25;

// Teeth at the byte's printed centre, which is the loop opened, and at its ends.
constexpr double kTeethAtPrintedZeroDb = 0.79;
constexpr double kTeethAtByteEndsDb = 23.4;

// How far a tooth may sit from where it is expected, as a fraction of the comb's
// own spacing: the width the archive publishes for the same reading, 0.030 ms
// on a 0.618 ms delay.
constexpr double kToothPlacement = 0.030 / 0.618;

// Success criterion for a modulated delay: the pre-filter corner, in hertz.
constexpr double kCornerRateTolerance = 0.03;

template <typename Processor, typename Config>
std::vector<double> response_db(const Config& config, double sample_rate) {
  Processor processor(config);
  processor.prepare(sample_rate, kFftLength);
  std::vector<float> impulse = sonare::test::generate_impulse(kFftLength);
  sonare::test::process(processor, impulse);
  sonare::FFT plan(kFftLength);
  std::vector<std::complex<float>> spectrum(static_cast<std::size_t>(plan.n_bins()));
  plan.forward(impulse.data(), spectrum.data());
  std::vector<double> db(spectrum.size());
  for (std::size_t i = 0; i < spectrum.size(); ++i) {
    // The 1e-30 guard is neither kEpsilon nor kSpectrumEpsilon: it floors a raw
    // |X| before a log, where either named epsilon would sit above a real null.
    db[i] = 20.0 * std::log10(static_cast<double>(std::abs(spectrum[i])) + 1e-30);
  }
  return db;
}

double bin_hz(std::size_t bin, double sample_rate) {
  return static_cast<double>(bin) * sample_rate / kFftLength;
}

std::size_t nearest_bin(double hz, double sample_rate) {
  return static_cast<std::size_t>(std::lround(hz * kFftLength / sample_rate));
}

double level_at(const std::vector<double>& db, double sample_rate, double hz) {
  return db[std::min(nearest_bin(hz, sample_rate), db.size() - 1)];
}

/// Magnitude of @p order identical one-pole sections at @p corner_hz, built the
/// way prepare() builds one. Used only for the control profile.
double section_db(double corner_hz, double hz, double sample_rate, int order, bool high_pass) {
  const double pole = std::exp(-kTwoPiD * corner_hz / sample_rate);
  const std::complex<double> z = std::polar(1.0, -kTwoPiD * hz / sample_rate);
  const std::complex<double> low = (1.0 - pole) / (1.0 - pole * z);
  const std::complex<double> one = high_pass ? 1.0 - low : low;
  return 20.0 * static_cast<double>(order) * std::log10(std::abs(one));
}

/// The twelfth-octave band centres the published profiles were read on: 113 of
/// them ending at 16 kHz, which is the measured unit's own Nyquist.
std::vector<double> band_centres() {
  std::vector<double> bands;
  bands.reserve(113);
  for (int k = 112; k >= 0; --k) {
    bands.push_back(16000.0 * std::pow(2.0, -k / 12.0));
  }
  return bands;
}

/// How fast a profile runs at its fastest, read the way the archive reads it: a
/// least-squares slope in decibels per octave over the thirteen bands centred on
/// each, keeping the largest.
double steepest_db_per_octave(const std::vector<double>& band_db,
                              const std::vector<double>& bands) {
  double steepest = 0.0;
  for (std::size_t i = 6; i + 6 < bands.size(); ++i) {
    double mean_x = 0.0;
    double mean_y = 0.0;
    for (std::size_t j = i - 6; j <= i + 6; ++j) {
      mean_x += std::log2(bands[j]);
      mean_y += band_db[j];
    }
    mean_x /= 13.0;
    mean_y /= 13.0;
    double numerator = 0.0;
    double denominator = 0.0;
    for (std::size_t j = i - 6; j <= i + 6; ++j) {
      const double dx = std::log2(bands[j]) - mean_x;
      numerator += dx * (band_db[j] - mean_y);
      denominator += dx * dx;
    }
    const double slope = numerator / denominator;
    if (std::fabs(slope) > std::fabs(steepest)) steepest = slope;
  }
  return steepest;
}

/// A chorus with its modulator stopped and its delay held at one millisecond, so
/// the only thing the response carries is the section in front of it.
ChorusConfig still_chorus(PreFilterMode mode, float corner_hz) {
  ChorusConfig config;
  config.rate_hz = 0.0f;
  config.depth_ms = 0.0f;
  config.center_delay_ms = 1.0f;
  config.dry_wet = 1.0f;
  config.pre_filter_hz = corner_hz;
  config.pre_filter_mode = mode;
  return config;
}

/// The same, as a flanger, with the loop gain asked for.
FlangerConfig still_flanger(float feedback, PreFilterMode mode, float corner_hz) {
  FlangerConfig config;
  config.rate_hz = 0.0f;
  config.depth_ms = 0.0f;
  config.center_delay_ms = 0.5f;
  config.feedback = feedback;
  config.dry_wet = 1.0f;
  config.pre_filter_hz = corner_hz;
  config.pre_filter_mode = mode;
  return config;
}

struct Residual {
  double rms;
  double worst;
};

Residual against_readings(const std::vector<double>& db, double sample_rate,
                          const BandReading* readings, std::size_t count) {
  Residual residual{0.0, 0.0};
  for (std::size_t i = 0; i < count; ++i) {
    const double error = level_at(db, sample_rate, readings[i].hz) - readings[i].db;
    residual.rms += error * error;
    residual.worst = std::max(residual.worst, std::fabs(error));
  }
  residual.rms = std::sqrt(residual.rms / static_cast<double>(count));
  return residual;
}

Residual control_residual(double corner_hz, bool high_pass, const BandReading* readings,
                          std::size_t count) {
  Residual residual{0.0, 0.0};
  for (std::size_t i = 0; i < count; ++i) {
    const double error =
        section_db(corner_hz, readings[i].hz, kMachineRate, 2, high_pass) - readings[i].db;
    residual.rms += error * error;
    residual.worst = std::max(residual.worst, std::fabs(error));
  }
  residual.rms = std::sqrt(residual.rms / static_cast<double>(count));
  return residual;
}

/// Local maxima of a comb between @p lo_hz and @p hi_hz, one per tooth.
std::vector<double> teeth(const std::vector<double>& db, double sample_rate, double lo_hz,
                          double hi_hz) {
  std::vector<double> found;
  const std::size_t lo = std::max<std::size_t>(1, nearest_bin(lo_hz, sample_rate));
  const std::size_t hi = std::min(nearest_bin(hi_hz, sample_rate), db.size() - 2);
  for (std::size_t i = lo; i <= hi; ++i) {
    if (db[i] <= db[i - 1] || db[i] < db[i + 1]) continue;
    const double hz = bin_hz(i, sample_rate);
    // One entry per tooth: two adjacent bins can both read as a local maximum
    // once the transform is in single precision.
    if (!found.empty() && hz < found.back() * 1.02) continue;
    found.push_back(hz);
  }
  return found;
}

double spread_db(const std::vector<double>& db, double sample_rate, double lo_hz, double hi_hz) {
  const std::size_t lo = std::max<std::size_t>(1, nearest_bin(lo_hz, sample_rate));
  const std::size_t hi = std::min(nearest_bin(hi_hz, sample_rate), db.size() - 1);
  double low = db[lo];
  double high = db[lo];
  for (std::size_t i = lo; i <= hi; ++i) {
    low = std::min(low, db[i]);
    high = std::max(high, db[i]);
  }
  return high - low;
}

/// Frequency at which a falling profile has lost half its power, read by
/// interpolating between the two bins that bracket it.
double half_power_hz(const std::vector<double>& db, double sample_rate) {
  const double target = 20.0 * std::log10(std::sqrt(0.5));
  for (std::size_t i = 1; i < db.size(); ++i) {
    if (db[i] > target) continue;
    const double before = db[i - 1];
    const double after = db[i];
    const double t = (before - target) / (before - after);
    return bin_hz(i - 1, sample_rate) + t * (bin_hz(i, sample_rate) - bin_hz(i - 1, sample_rate));
  }
  return 0.0;
}

void compare_profile(Tally& tally, PreFilterMode mode, float corner_hz, double control_corner_hz,
                     const BandReading* readings, std::size_t count, const std::string& what) {
  const std::vector<double> db = response_db<Chorus>(still_chorus(mode, corner_hz), kMachineRate);
  for (std::size_t i = 0; i < count; ++i) {
    tally.near(level_at(db, kMachineRate, readings[i].hz), readings[i].db, kBandTolerance,
               what + " at " + std::to_string(readings[i].hz) + " Hz");
  }
  const Residual measured = against_readings(db, kMachineRate, readings, count);
  tally.at_most(measured.rms, kProfileRmsTolerance, what + " over the whole published profile");

  // The control: the same readings refitted as two of these sections in
  // cascade. A comparison that could not tell one from two would clear this.
  const Residual control =
      control_residual(control_corner_hz, mode == PreFilterMode::kHighPass, readings, count);
  tally.at_least(control.rms, kControlRmsFloor, what + " -- two sections in cascade do not fit");
  tally.at_least(control.worst, kControlBandFloor,
                 what + " -- and miss at least one band by a wide margin");
  tally.at_least(control.rms / measured.rms, 2.0,
                 what + " -- one section fits at least twice as well as two");
}

}  // namespace

TEST_CASE(
    "the chorus/flanger pre-filter skirt and flanger feedback polarity match the measured "
    "structure",
    "[modulated-delay-structure]") {
  Tally tally;

  // --- the section in front of the delay, in both shapes --------------------
  compare_profile(tally, PreFilterMode::kLowPass, kLowPassBottomCornerHz, kLowPassBottomControlHz,
                  kLowPassBottomByte, std::size(kLowPassBottomByte),
                  "low pass at the bottom of the cutoff range");
  compare_profile(tally, PreFilterMode::kLowPass, kLowPassMiddleCornerHz, kLowPassMiddleControlHz,
                  kLowPassMiddleByte, std::size(kLowPassMiddleByte),
                  "low pass at the middle of the cutoff range");
  compare_profile(tally, PreFilterMode::kHighPass, kHighPassBottomCornerHz,
                  kHighPassBottomControlHz, kHighPassBottomByte, std::size(kHighPassBottomByte),
                  "high pass at the bottom of the cutoff range");

  // The one slope the archive publishes that a section alone can be held to.
  const std::vector<double> bands = band_centres();
  const std::vector<double> high_pass_db = response_db<Chorus>(
      still_chorus(PreFilterMode::kHighPass, kHighPassBottomCornerHz), kMachineRate);
  std::vector<double> high_pass_bands(bands.size());
  for (std::size_t i = 0; i < bands.size(); ++i) {
    high_pass_bands[i] = level_at(high_pass_db, kMachineRate, bands[i]);
  }
  const double slope = steepest_db_per_octave(high_pass_bands, bands);
  tally.at_least(slope, kHighPassSlopeLow,
                 "the high-pass skirt runs no gentler than the readings published for it");
  tally.at_most(slope, kHighPassSlopeHigh,
                "and no steeper: eight readings across two types span 5.46 to 6.25 dB/octave");

  // --- one section, reached from both types ---------------------------------
  // Swept at every setting the chorus's was published at, the two types return
  // one skirt to a median 0.08 dB, which is what the runs repeat themselves to.
  for (const auto mode : {PreFilterMode::kLowPass, PreFilterMode::kHighPass}) {
    const float corner_hz =
        mode == PreFilterMode::kLowPass ? kLowPassBottomCornerHz : kHighPassBottomCornerHz;
    const std::vector<double> chorus_db =
        response_db<Chorus>(still_chorus(mode, corner_hz), kMachineRate);
    const std::vector<double> flanger_db =
        response_db<Flanger>(still_flanger(0.0f, mode, corner_hz), kMachineRate);
    double worst = 0.0;
    for (const double hz : bands) {
      worst = std::max(worst, std::fabs(level_at(chorus_db, kMachineRate, hz) -
                                        level_at(flanger_db, kMachineRate, hz)));
    }
    tally.at_most(worst, kSharedSectionDb,
                  std::string("the two types' front ends are one section (") +
                      (mode == PreFilterMode::kLowPass ? "low pass)" : "high pass)"));
  }

  // --- the loop, and what its sign does to the teeth ------------------------
  // At the byte's printed centre there is no loop: the widest band of either
  // published record is under a decibel where the byte's ends move 23.4.
  const std::vector<double> loop_open =
      response_db<Flanger>(still_flanger(0.0f, PreFilterMode::kOff, 0.0f), kMachineRate);
  tally.at_most(spread_db(loop_open, kMachineRate, 200.0, 14000.0), kTeethAtPrintedZeroDb,
                "with the loop open the profile has no teeth in it");

  // A gain exists at which the loop reaches the depth the byte's ends measured.
  // Which gain the byte means is not claimed: the archive publishes no
  // conversion for that byte, so the structure goes in untranslated.
  constexpr float kDeepLoopGain = 0.88f;
  const std::vector<double> in_phase =
      response_db<Flanger>(still_flanger(kDeepLoopGain, PreFilterMode::kOff, 0.0f), kMachineRate);
  const std::vector<double> inverting =
      response_db<Flanger>(still_flanger(-kDeepLoopGain, PreFilterMode::kOff, 0.0f), kMachineRate);
  const double in_phase_depth = spread_db(in_phase, kMachineRate, 200.0, 14000.0);
  const double inverting_depth = spread_db(inverting, kMachineRate, 200.0, 14000.0);
  tally.at_least(in_phase_depth, kTeethAtByteEndsDb,
                 "the loop reaches the 23.4 dB of teeth the byte's ends measured");
  tally.near(inverting_depth, in_phase_depth, 0.5,
             "and reaches the same depth at the same magnitude with the sign flipped");

  const std::vector<double> in_phase_teeth = teeth(in_phase, kMachineRate, 200.0, 14000.0);
  const std::vector<double> inverting_teeth = teeth(inverting, kMachineRate, 200.0, 14000.0);
  tally.at_least(static_cast<double>(in_phase_teeth.size()), 5.0,
                 "the in-phase loop puts a readable run of teeth in the band");
  REQUIRE(in_phase_teeth.size() >= 5);
  REQUIRE(inverting_teeth.size() >= 5);
  const double spacing = (in_phase_teeth.back() - in_phase_teeth.front()) /
                         static_cast<double>(in_phase_teeth.size() - 1);
  for (const double hz : in_phase_teeth) {
    tally.at_most(std::fabs(std::remainder(hz - in_phase_teeth.front(), spacing)) / spacing,
                  kToothPlacement, "an in-phase tooth sits on the comb's own multiples");
  }
  for (const double hz : inverting_teeth) {
    tally.near(std::fabs(std::remainder(hz - in_phase_teeth.front(), spacing)) / spacing, 0.5,
               kToothPlacement, "an inverted tooth sits halfway between the in-phase ones");
  }
  // Read the other way round: where the in-phase loop resonates, the inverted
  // one cancels, so the two signs are not the same profile at any depth.
  for (const double hz : in_phase_teeth) {
    tally.at_least(level_at(in_phase, kMachineRate, hz) - level_at(inverting, kMachineRate, hz),
                   kTeethAtByteEndsDb, "an in-phase tooth is a null under the opposite sign");
  }

  // --- the modulators are not one -------------------------------------------
  // Two instances of one type, warmed by silence so their delay lines and
  // sections are both at rest and only the oscillator has moved on.
  constexpr int kBlock = 4800;  // 0.1 s at 48 kHz.
  const std::vector<float> signal =
      sonare::test::generate_sine(kBlock, 440.0f, static_cast<int>(kHostRate), 0.5f);
  const ChorusConfig modulating;
  Chorus advanced(modulating);
  Chorus at_rest(modulating);
  Chorus repeat_of_advanced(modulating);
  for (Chorus* instance : {&advanced, &at_rest, &repeat_of_advanced}) {
    instance->prepare(kHostRate, kBlock);
  }
  std::vector<float> silence(kBlock, 0.0f);
  sonare::test::process(advanced, silence);
  std::vector<float> silence_again(kBlock, 0.0f);
  sonare::test::process(repeat_of_advanced, silence_again);

  std::vector<float> from_advanced = signal;
  std::vector<float> from_rest = signal;
  std::vector<float> from_repeat = signal;
  sonare::test::process(advanced, from_advanced);
  sonare::test::process(at_rest, from_rest);
  sonare::test::process(repeat_of_advanced, from_repeat);
  std::vector<float> difference(from_advanced.size());
  for (std::size_t i = 0; i < difference.size(); ++i) {
    difference[i] = from_advanced[i] - from_rest[i];
  }
  tally.at_least(static_cast<double>(sonare::test::rms(difference)) /
                     static_cast<double>(sonare::test::rms(signal)),
                 0.01, "a second instance starts its own modulator rather than the first's");
  tally.same(from_advanced == from_repeat,
             "and two instances driven the same way stay identical, so the difference is state");

  // --- the corner is a frequency, not a coefficient -------------------------
  // What a modulated delay is asked to hold across a rate change: the corner in
  // hertz, to 3 per cent.
  constexpr float kCornerHz = 1000.0f;
  for (const double rate : {kMachineRate, kSecondHostRate, kHostRate}) {
    const std::vector<double> db =
        response_db<Chorus>(still_chorus(PreFilterMode::kLowPass, kCornerHz), rate);
    tally.near(half_power_hz(db, rate), kCornerHz, kCornerHz * kCornerRateTolerance,
               "the low-pass corner at " + std::to_string(static_cast<int>(rate)) + " Hz");
  }

  WARN("comparisons: " << tally.count());
  REQUIRE(tally.count() >= 60);
}
