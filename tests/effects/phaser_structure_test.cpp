/// @file phaser_structure_test.cpp
/// @brief The measured phaser structure: eight allpass stages, dry-sum mixing,
///        and a feedback loop whose gain lifts the inter-notch peaks.
///
/// Every expectation is a structural quantity -- a count of cancellations, a
/// ratio between them, a ceiling arithmetic fixes, a direction a loop gain
/// drives -- read off the processor's own impulse response. None is a recorded
/// waveform, and none could be: the contract this module keeps is the control
/// protocol and not the audio (src/midi/synth/docs/gs.md).
///
/// Each tolerance names where it comes from, and all of them come from the
/// measurement rather than from what happened to pass. Two recur: a feature is
/// located to a twelfth-octave band, so "unchanged" means inside the band it
/// was read in (5.95%), and a profile is read against a run floor of 1.92 dB.
///
/// Reach is an output: the case reports how many comparisons it made, because
/// a run that compared nothing looks exactly like a run that passed.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <string>
#include <vector>

#include "core/fft.h"
#include "effects/modulation/phaser.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"

namespace {

using sonare::constants::kPiD;
using sonare::effects::modulation::Phaser;
using sonare::effects::modulation::PhaserConfig;
using sonare::effects::modulation::PhaserMixMode;

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

// 0.34 s at 48 kHz. Long enough that the loop's ring is under 1e-30 by the end
// at the largest gain used here, and that 25 Hz is three bins above DC.
constexpr int kFftLength = 16384;

// The two rates the inserts are asked to hold a physical quantity across, plus
// the 32 kHz the profiles below were measured at.
constexpr double kMachineRate = 32000.0;
constexpr double kHostRate = 48000.0;

// Corner the record's lowest-notch reading of 1887.7 Hz implies for an
// eight-section cascade at 32 kHz, read in the tangent of frequency. The
// modulator is stopped (min_hz == max_hz), which is what makes a notch readable.
constexpr float kCornerHz = 7692.0f;

// Loop gains the measurement reads off the resonance byte at two settings: the
// middle of its range, and the top, where the profile reaches 18.87 dB.
constexpr float kLoopGainMidByte = 0.37f;
constexpr float kLoopGainTopByte = 0.885f;

// A twelfth-octave band, which is the resolution every profile below was read
// at, expressed as the fraction a feature may move and stay in its own band.
const double kOneTwelfthOctave = std::pow(2.0, 1.0 / 12.0) - 1.0;

// The run's own reproducibility floor in dB, from the record the three-setting
// resonance profiles were taken on.
constexpr double kProfileFloorDb = 1.92;

// How far the archive's own best model sits above the unit below 60 Hz on these
// same records: a lean it carries in rather than creates, 1.6 to 3.3 dB.
constexpr double kLowBandLeanDb = 3.3;

PhaserConfig held_still(int stages, float dry_wet, PhaserMixMode mode, float feedback) {
  PhaserConfig config;
  config.rate_hz = 0.0f;
  config.min_hz = kCornerHz;
  config.max_hz = kCornerHz;
  config.stages = stages;
  config.dry_wet = dry_wet;
  config.mix_mode = mode;
  config.feedback = feedback;
  return config;
}

/// Magnitude response in dB, read off the processor's own impulse response.
/// The 1e-30 guard is neither kEpsilon nor kSpectrumEpsilon: it floors a raw
/// |X| before a log, where either named epsilon would sit above a real null.
std::vector<double> response_db(const PhaserConfig& config, double sample_rate) {
  Phaser phaser(config);
  phaser.prepare(sample_rate, kFftLength);
  std::vector<float> impulse = sonare::test::generate_impulse(kFftLength);
  sonare::test::process(phaser, impulse);
  sonare::FFT plan(kFftLength);
  std::vector<std::complex<float>> spectrum(static_cast<std::size_t>(plan.n_bins()));
  plan.forward(impulse.data(), spectrum.data());
  std::vector<double> db(spectrum.size());
  for (std::size_t i = 0; i < spectrum.size(); ++i) {
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

/// Cancellations below @p max_hz: local minima deep enough that no inter-notch
/// ripple reaches them, reported one per feature.
std::vector<double> cancellations(const std::vector<double>& db, double sample_rate,
                                  double max_hz) {
  std::vector<double> found;
  const std::size_t last = std::min(nearest_bin(max_hz, sample_rate), db.size() - 2);
  for (std::size_t i = 2; i < last; ++i) {
    if (db[i] >= db[i - 1] || db[i] >= db[i + 1] || db[i] > -24.0) continue;
    const double hz = bin_hz(i, sample_rate);
    // One entry per feature: adjacent bins either side of a deep null can both
    // read as local minima once the transform is in single precision.
    if (!found.empty() && hz < found.back() * 1.02) continue;
    found.push_back(hz);
  }
  return found;
}

/// Frequency and level of the largest bin between @p lo_hz and @p hi_hz.
struct Peak {
  double hz;
  double db;
};

Peak band_peak(const std::vector<double>& db, double sample_rate, double lo_hz, double hi_hz) {
  const std::size_t lo = std::max<std::size_t>(1, nearest_bin(lo_hz, sample_rate));
  const std::size_t hi = std::min(nearest_bin(hi_hz, sample_rate), db.size() - 1);
  std::size_t best = lo;
  for (std::size_t i = lo; i <= hi; ++i) {
    if (db[i] > db[best]) best = i;
  }
  return {bin_hz(best, sample_rate), db[best]};
}

/// The lowest bin within @p window of @p hz, and how far it sits under the
/// nearer of the two shoulders the curve rises to either side of it.
struct Dip {
  double hz;
  double depth_db;
};

Dip dip_near(const std::vector<double>& db, double sample_rate, double hz, double window) {
  const std::size_t lo = std::max<std::size_t>(1, nearest_bin(hz * (1.0 - window), sample_rate));
  const std::size_t hi = std::min(nearest_bin(hz * (1.0 + window), sample_rate), db.size() - 2);
  std::size_t best = lo;
  for (std::size_t i = lo; i <= hi; ++i) {
    if (db[i] < db[best]) best = i;
  }
  std::size_t left = best;
  while (left > 1 && db[left - 1] >= db[left]) --left;
  std::size_t right = best;
  while (right + 2 < db.size() && db[right + 1] >= db[right]) ++right;
  return {bin_hz(best, sample_rate), std::min(db[left], db[right]) - db[best]};
}

/// Where an eight-section cascade's k-th cancellation sits relative to its own
/// corner, read in the tangent of frequency: tan((2k-1)*pi/16).
double section_ratio(int k) { return std::tan((2.0 * k - 1.0) * kPiD / 16.0); }

double warped(double hz, double sample_rate) { return std::tan(kPiD * hz / sample_rate); }

}  // namespace

TEST_CASE("the phaser's notch count and feedback-driven peaks match the measured structure",
          "[phaser-structure]") {
  Tally tally;

  // --- the section count is what puts cancellations in the band -------------
  // Eight first-order sections share one corner and cancel four times in the
  // audible band; a notch costs two sections, so four sections cancel twice.
  const std::vector<double> eight_open =
      response_db(held_still(8, 1.0f, PhaserMixMode::kDrySum, 0.0f), kHostRate);
  const std::vector<double> notches = cancellations(eight_open, kHostRate, kHostRate * 0.49);
  tally.same(notches.size() == 4, "eight sections cancel four times in the band (found " +
                                      std::to_string(notches.size()) + ")");
  REQUIRE(notches.size() == 4);

  const std::vector<double> four_open =
      response_db(held_still(4, 1.0f, PhaserMixMode::kDrySum, 0.0f), kHostRate);
  const std::vector<double> four_notches = cancellations(four_open, kHostRate, kHostRate * 0.49);
  tally.same(four_notches.size() == 2, "four sections cancel twice at the same corner (found " +
                                           std::to_string(four_notches.size()) + ")");

  // The same count at the rate the profiles were measured at, where the four
  // sit lower in the band because the bilinear warp is coarser.
  const std::vector<double> machine_open =
      response_db(held_still(8, 1.0f, PhaserMixMode::kDrySum, 0.0f), kMachineRate);
  const std::vector<double> machine_notches =
      cancellations(machine_open, kMachineRate, kMachineRate * 0.49);
  tally.same(machine_notches.size() == 4, "eight sections cancel four times at 32 kHz too (found " +
                                              std::to_string(machine_notches.size()) + ")");

  // --- the cancellations sit at the ratios the section count fixes ----------
  // Measured 3.30 and 2.20 times each other in the tangent of frequency,
  // against the 3.36 and 2.24 eight sections fix. The notch is located to 0.030
  // octaves, so a ratio of two such readings carries sqrt(2) * 0.030 = 3.0%.
  constexpr double kRatioTolerance = 0.030;  // in octaves, doubled below
  const double ratio_tolerance_fraction = std::pow(2.0, std::sqrt(2.0) * kRatioTolerance) - 1.0;
  const double measured_low_ratio = 3.30;
  const double measured_high_ratio = 2.20;
  const double first = warped(notches[0], kHostRate);
  const double second = warped(notches[1], kHostRate);
  const double third = warped(notches[2], kHostRate);
  const double fourth = warped(notches[3], kHostRate);
  tally.near(second / first, measured_low_ratio, measured_low_ratio * ratio_tolerance_fraction,
             "notch 2 over notch 1 in the tangent of frequency");
  tally.near(third / second, measured_high_ratio, measured_high_ratio * ratio_tolerance_fraction,
             "notch 3 over notch 2 in the tangent of frequency");
  tally.near(fourth / third, measured_low_ratio, measured_low_ratio * ratio_tolerance_fraction,
             "notch 4 over notch 3 in the tangent of frequency");
  // The same four against what the section count alone predicts, which is the
  // form the ratios above are the measurement of.
  for (int k = 1; k <= 4; ++k) {
    const double predicted =
        std::atan(warped(kCornerHz, kHostRate) * section_ratio(k)) * kHostRate / kPiD;
    tally.near(notches[static_cast<std::size_t>(k - 1)], predicted,
               predicted * kOneTwelfthOctave * 0.5,
               "notch " + std::to_string(k) + " against tan((2k-1)pi/16) times the corner");
  }

  // --- a dry sum is not any setting of a crossfade --------------------------
  // A cascade passes every frequency at unit gain, so summing it with the dry
  // signal reaches 20*log10(1 + mix) where the two arrive in phase: 6.02 dB at
  // the top of the mix, and no crossfade of the same pair can exceed the input.
  const double kSumCeilingDb = 20.0 * std::log10(2.0);
  const Peak sum_peak = band_peak(eight_open, kHostRate, 20.0, 20000.0);
  tally.near(sum_peak.db, kSumCeilingDb, 0.05,
             "a dry sum at full mix reaches the +6 dB a sum arithmetically tops out at");
  for (const float dry_wet : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
    const std::vector<double> crossfaded =
        response_db(held_still(8, dry_wet, PhaserMixMode::kCrossfade, 0.0f), kHostRate);
    const Peak peak = band_peak(crossfaded, kHostRate, 20.0, 20000.0);
    tally.at_most(
        peak.db, 0.05,
        "a crossfade at dryWet " + std::to_string(dry_wet) + " never exceeds the input it split");
  }

  // --- the loop lifts the peaks between the notches -------------------------
  // A sum alone cannot pass 6.02 dB; the profile reaches 18.87 dB, which is
  // what says the byte closes a loop rather than adding a second helping.
  double previous = -1e9;
  for (const float gain : {0.0f, 0.2f, 0.4f, 0.6f, 0.8f, kLoopGainTopByte}) {
    const std::vector<double> looped =
        response_db(held_still(8, 1.0f, PhaserMixMode::kDrySum, gain), kHostRate);
    const Peak peak = band_peak(looped, kHostRate, 20.0, 20000.0);
    tally.at_least(peak.db, previous + 0.1,
                   "loop gain " + std::to_string(gain) + " lifts the peak above the gain below it");
    previous = peak.db;
  }
  const std::vector<double> machine_top =
      response_db(held_still(8, 1.0f, PhaserMixMode::kDrySum, kLoopGainTopByte), kMachineRate);
  const Peak machine_top_peak = band_peak(machine_top, kMachineRate, 20.0, 15000.0);
  tally.near(machine_top_peak.db, 18.87, kProfileFloorDb,
             "at the top byte's loop gain the profile reaches the 18.87 dB measured");
  tally.at_least(machine_top_peak.db - kSumCeilingDb, 6.0,
                 "and stands well clear of what a sum alone could reach");

  // --- the loop fills the cancellations in and leaves them where they were --
  // Every one of the four shallows as the byte rises and not one deepens; the
  // resonances arrive between them rather than on them.
  const std::vector<double> mid_loop =
      response_db(held_still(8, 1.0f, PhaserMixMode::kDrySum, kLoopGainMidByte), kHostRate);
  for (std::size_t k = 0; k < notches.size(); ++k) {
    const std::string which = "notch " + std::to_string(k + 1);
    const Dip open = dip_near(eight_open, kHostRate, notches[k], 0.15);
    const Dip closed = dip_near(mid_loop, kHostRate, notches[k], 0.15);
    tally.at_most(std::fabs(closed.hz / notches[k] - 1.0), kOneTwelfthOctave,
                  which + " stays inside the twelfth-octave band it was read in");
    tally.at_most(closed.depth_db, open.depth_db,
                  which + " is shallower with the loop closed, never deeper");
  }

  // --- the return does not carry the bottom of the band ---------------------
  // One high-pass pole inside the loop and nowhere else in the chain: with the
  // loop nearly shut the lowest resonance is largest at 105.1 Hz and has fallen
  // thirteen decibels by 25; with it open the same span is flat.
  const Peak low_open = band_peak(machine_open, kMachineRate, 20.0, 400.0);
  const double open_fall = low_open.db - machine_open[nearest_bin(25.0, kMachineRate)];
  tally.at_most(open_fall, 0.05, "with the loop open the bottom of the band is untouched");

  const Peak low_closed = band_peak(machine_top, kMachineRate, 20.0, 400.0);
  tally.near(low_closed.hz, 105.1, 105.1 * kOneTwelfthOctave * 0.5,
             "with the loop nearly shut the lowest resonance is largest at 105.1 Hz");
  const double closed_fall = low_closed.db - machine_top[nearest_bin(25.0, kMachineRate)];
  tally.at_least(closed_fall, 13.0 - kLowBandLeanDb,
                 "and has fallen thirteen decibels by 25 Hz, less the model's own low-band lean");
  tally.at_least(closed_fall - open_fall, 9.0,
                 "the shortfall follows the loop gain, which is what puts the pole inside it");

  WARN("comparisons: " << tally.count());
  REQUIRE(tally.count() >= 30);
}
