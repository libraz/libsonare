/// @file insert_sample_rate_test.cpp
/// @brief Each GS-EFX-driven insert names one physical quantity it preserves,
///        and this case reads that quantity off the audio at 44100 Hz and at
///        48000 Hz.
///
/// Every quantity carries two assertions, and they are two properties rather
/// than one check and a spare. Reading a quantity against what the
/// configuration asked for, at each rate, says it is the right value there.
/// Reading the two rates against each other says it survives a rate change,
/// which is a different thing: a hold kept as a rounded count of samples can
/// round, at two rates, to counts whose nulls land a quarter of a per cent from
/// each other and nowhere near the rate that was set. Neither assertion
/// subsumes the other, and which of them will see a given defect is not
/// knowable in advance -- the same rounding that agrees across the rates for one
/// quantity diverges by several per cent for the next. Delete either half and
/// the pair stops being an instrument.
///
/// Where the preserved quantity restates a stored field, the second assertion
/// is against the field. Where it does not, it is against a prediction this
/// file computes from the field and the rate:
///
/// - phaser, first notch in Hz: computed. A cascade's cancellation is the
///   accumulated phase of its sections, warped differently at every rate, so
///   holding the corner does not hold the notch.
/// - stereo delay, damping half-power corner in Hz: computed. A discrete one
///   pole crosses half power above the corner it is built from.
/// - chorus / flanger, pre-filter half-power corner in Hz: computed, same pole.
/// - lofi, first aperture null in Hz: the hold rate itself.
/// - pitch shifter, beat period in seconds: the window over |ratio - 1|.
/// - rotary, acceleration time constant in seconds: the time constant itself.
/// - parametric EQ, a shelf's half-gain point and a peak's centre in Hz: the
///   corner itself, which the section's design places exactly at every rate.
///
/// Reach is an output: the case reports how many comparisons it made, because
/// a run that compared nothing looks exactly like a run that passed.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "core/fft.h"
#include "effects/delay/stereo_delay.h"
#include "effects/modulation/chorus.h"
#include "effects/modulation/flanger.h"
#include "effects/modulation/phaser.h"
#include "effects/modulation/pitch_shifter.h"
#include "effects/modulation/rotary.h"
#include "mastering/api/insert_factory.h"
#include "mastering/saturation/bitcrusher.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"

namespace {

using sonare::constants::kPiD;
using sonare::constants::kTwoPiD;
using sonare::effects::delay::StereoDelay;
using sonare::effects::delay::StereoDelayConfig;
using sonare::effects::modulation::Chorus;
using sonare::effects::modulation::ChorusConfig;
using sonare::effects::modulation::Flanger;
using sonare::effects::modulation::FlangerConfig;
using sonare::effects::modulation::Phaser;
using sonare::effects::modulation::PhaserConfig;
using sonare::effects::modulation::PhaserMixMode;
using sonare::effects::modulation::PitchShifter;
using sonare::effects::modulation::PitchShifterConfig;
using sonare::effects::modulation::PreFilterMode;
using sonare::effects::modulation::Rotary;
using sonare::effects::modulation::RotaryConfig;
using sonare::mastering::saturation::BitCrusher;
using sonare::mastering::saturation::BitCrusherConfig;
using sonare::mastering::saturation::QuantizerMode;

/// Counts every comparison it makes, so the case can report its own reach.
class Tally {
 public:
  void within(double got, double expected, double fraction, const std::string& what) {
    ++count_;
    const double tolerance = std::fabs(expected) * fraction;
    INFO(what << ": got " << got << ", expected " << expected << ", tolerance " << tolerance);
    CHECK(std::fabs(got - expected) <= tolerance);
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

// --- the two host rates -----------------------------------------------------

// Deliberately not a pair in integer ratio, and no quantity below is read at a
// rate that divides it: an integer ratio hides exactly the defect this case is
// for, because a coefficient carried across it lands on a whole number again.
constexpr double kHostRate = 48000.0;
constexpr double kAltHostRate = 44100.0;

// --- tolerances, one per preserved quantity ---------------------------------
// Each is the figure named beside its quantity in the success criterion; none
// is what happened to pass. The two one-pole corners carry the widest of them
// because a discrete pole's own half-power point moves with the rate.

constexpr double kNotchTolerance = 0.02;
constexpr double kDampingTolerance = 0.03;
constexpr double kNullTolerance = 0.01;
constexpr double kBeatTolerance = 0.01;
constexpr double kGlideTolerance = 0.03;
constexpr double kPreFilterTolerance = 0.03;
// The EQ positions are exact in the design at both rates, so this only has to
// clear the float coefficients; it still sits under both measured defects.
constexpr double kEqTolerance = 0.01;

// --- spectra ----------------------------------------------------------------

// 0.34 s at 48 kHz: the lowest feature read below sits near 300 Hz, which is a
// hundred bins up, and every impulse response used here is under 1e-30 by the
// end of the window.
constexpr int kFftLength = 16384;

struct Spectrum {
  std::vector<double> db;
  std::vector<double> phase;  ///< unwrapped, starting from zero at DC.
};

double bin_hz(std::size_t bin, double sample_rate) {
  return static_cast<double>(bin) * sample_rate / kFftLength;
}

/// Magnitude and unwrapped phase of @p config's own impulse response.
template <typename Processor, typename Config>
Spectrum impulse_spectrum(const Config& config, double sample_rate) {
  Processor processor(config);
  processor.prepare(sample_rate, kFftLength);
  std::vector<float> impulse = sonare::test::generate_impulse(kFftLength);
  sonare::test::process(processor, impulse);
  sonare::FFT plan(kFftLength);
  std::vector<std::complex<float>> spectrum(static_cast<std::size_t>(plan.n_bins()));
  plan.forward(impulse.data(), spectrum.data());
  Spectrum out;
  out.db.resize(spectrum.size());
  out.phase.resize(spectrum.size());
  double previous = 0.0;
  double unwrapped = 0.0;
  for (std::size_t i = 0; i < spectrum.size(); ++i) {
    // The 1e-30 guard is neither kEpsilon nor kSpectrumEpsilon: it floors a raw
    // |X| before a log, where either named epsilon would sit above a real null.
    out.db[i] = 20.0 * std::log10(static_cast<double>(std::abs(spectrum[i])) + 1e-30);
    const double raw = std::atan2(static_cast<double>(spectrum[i].imag()),
                                  static_cast<double>(spectrum[i].real()));
    double step = raw - previous;
    while (step > kPiD) step -= 2.0 * kPiD;
    while (step < -kPiD) step += 2.0 * kPiD;
    unwrapped += step;
    previous = raw;
    out.phase[i] = unwrapped;
  }
  return out;
}

/// Frequency where a falling curve first reaches @p target, interpolated
/// between the two bins that bracket it. Zero when it never does, which the
/// caller reports rather than absorbing.
double first_crossing_hz(const std::vector<double>& curve, double sample_rate, double target) {
  for (std::size_t i = 1; i < curve.size(); ++i) {
    if (curve[i] > target) continue;
    const double before = curve[i - 1];
    const double t = (before - target) / (before - curve[i]);
    return bin_hz(i - 1, sample_rate) + t * (bin_hz(i, sample_rate) - bin_hz(i - 1, sample_rate));
  }
  return 0.0;
}

constexpr double kHalfPowerDb = -3.0102999566398120;

/// Cancellations below @p max_hz: local minima too deep for any inter-notch
/// ripple to reach, reported one per feature. Reads the whole band rather than
/// a window around an expectation, so it can disagree with one.
std::vector<double> cancellations(const std::vector<double>& db, double sample_rate,
                                  double max_hz) {
  std::vector<double> found;
  const std::size_t last = std::min(
      static_cast<std::size_t>(std::lround(max_hz * kFftLength / sample_rate)), db.size() - 2);
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

// --- what each rate predicts from the stored quantity ------------------------

/// Half-power corner of the running-average pole `exp(-2 pi fc / sr)`, solved
/// rather than approximated. This is why the two one-pole corners below are not
/// compared against their own fields: at 5 kHz over 48 kHz the pole built from
/// a corner crosses half power 3.5 per cent above it, which is larger than the
/// quantity's whole tolerance.
double one_pole_half_power_hz(double corner_hz, double sample_rate) {
  const double pole = std::exp(-kTwoPiD * corner_hz / sample_rate);
  const double gap = 1.0 - pole;
  const double cos_w = 1.0 - gap * gap / (2.0 * pole);
  if (!(cos_w > -1.0) || !(cos_w < 1.0)) return 0.0;
  return std::acos(cos_w) * sample_rate / kTwoPiD;
}

/// Where the first cancellation of a cascade of @p sections first-order allpass
/// sections sharing @p corner_hz falls. Each section contributes
/// `-2 atan(tan(pi f / sr) / tan(pi fc / sr))`, so the cascade reaches antiphase
/// where `tan(pi f / sr) = tan(pi fc / sr) * tan(pi / (2 * sections))` -- a
/// position in the tangent of frequency, which is not the corner scaled.
double first_notch_hz(double corner_hz, int sections, double sample_rate) {
  const double warped = std::tan(kPiD * corner_hz / sample_rate);
  return std::atan(warped * std::tan(kPiD / (2.0 * sections))) * sample_rate / kPiD;
}

// --- phaser -----------------------------------------------------------------

// Eight sections is what a period module's phaser measures as, and the count
// fixes where the cancellations sit relative to the corner.
constexpr int kPhaserSections = 8;

// Corners the notch is read at. The top one is here for the opposite reason to
// the other three: its notch is not expected to hold across a rate change.
constexpr float kPhaserCornerHz[] = {1500.0f, 3000.0f, 6000.0f};
constexpr float kPhaserWarpedCornerHz = 9000.0f;

PhaserConfig held_still(float corner_hz, float dry_wet, PhaserMixMode mode) {
  PhaserConfig config;
  config.rate_hz = 0.0f;
  config.min_hz = corner_hz;
  config.max_hz = corner_hz;
  config.stages = kPhaserSections;
  config.dry_wet = dry_wet;
  config.mix_mode = mode;
  return config;
}

/// First frequency the allpass cascade alone reaches antiphase at, which is
/// where a dry sum cancels. Read off the phase because a magnitude null is
/// located to half a bin and the phase is not; the case checks the two agree.
double first_antiphase_hz(float corner_hz, double sample_rate) {
  // dryWet at one over a crossfade leaves the dry path at zero gain, so the
  // response is the cascade and nothing else.
  const Spectrum cascade =
      impulse_spectrum<Phaser>(held_still(corner_hz, 1.0f, PhaserMixMode::kCrossfade), sample_rate);
  return first_crossing_hz(cascade.phase, sample_rate, -kPiD);
}

// --- stereo delay -----------------------------------------------------------

constexpr float kDampingCornerHz[] = {250.0f, 1250.0f, 5000.0f};

/// Zero delay, no feedback, fully wet: every stage but the damping is an
/// identity and the smoothers rest at their targets from the first sample, so
/// the insert's impulse response is the damping filter's own.
StereoDelayConfig bare_delay(float damping_hz) {
  StereoDelayConfig config;
  config.delay_time_l_ms = 0.0f;
  config.delay_time_r_ms = 0.0f;
  config.feedback = 0.0f;
  config.dry_wet = 1.0f;
  config.damping_hz = damping_hz;
  return config;
}

// --- chorus and flanger -----------------------------------------------------

constexpr float kPreFilterCornerHz[] = {400.0f, 1000.0f, 3000.0f};

// Long enough that the section is well clear of the read-out, short enough that
// nothing wraps. The reading below divides one render by another taken at the
// same delay, so the line's linear interpolation -- a second frequency-dependent
// loss, and a tilt of its own -- leaves the ratio entirely.
constexpr float kStillDelayMs = 2.0f;

ChorusConfig still_chorus(PreFilterMode mode, float corner_hz) {
  ChorusConfig config;
  config.rate_hz = 0.0f;
  config.depth_ms = 0.0f;
  config.center_delay_ms = kStillDelayMs;
  config.dry_wet = 1.0f;
  config.pre_filter_hz = corner_hz;
  config.pre_filter_mode = mode;
  return config;
}

FlangerConfig still_flanger(PreFilterMode mode, float corner_hz) {
  FlangerConfig config;
  config.rate_hz = 0.0f;
  config.depth_ms = 0.0f;
  config.center_delay_ms = kStillDelayMs;
  config.feedback = 0.0f;
  config.dry_wet = 1.0f;
  config.pre_filter_hz = corner_hz;
  config.pre_filter_mode = mode;
  return config;
}

/// The pre-filter's own half-power corner, read as the difference between the
/// section in the path and the same chain with it out.
template <typename Processor, typename Config>
double pre_filter_corner_hz(const Config& filtered, const Config& bypassed, double sample_rate) {
  const Spectrum with = impulse_spectrum<Processor>(filtered, sample_rate);
  const Spectrum without = impulse_spectrum<Processor>(bypassed, sample_rate);
  std::vector<double> relative(with.db.size());
  for (std::size_t i = 0; i < relative.size(); ++i) relative[i] = with.db[i] - without.db[i];
  return first_crossing_hz(relative, sample_rate, kHalfPowerDb);
}

// --- lofi hold --------------------------------------------------------------

// Hold rates the aperture null is read at. The last is the shorter of the two
// holds the readings separate, which is a third of the rate they were taken at;
// the other two are chosen. None of the six (rate, hold) pairs divides evenly,
// and the case asserts that distance below rather than trusting the choice: a
// hold landing on a whole number of samples would let a hold stored as a count
// of samples pass both assertions.
constexpr float kHoldHz[] = {3100.0f, 5200.0f, 32000.0f / 3.0f};

// How far from a whole number of samples each hold has to sit, at each rate,
// before this instrument can see a count-rounding defect at all. A hold of
// four-and-a-fraction samples that is this far out moves its null by more than
// three per cent when rounded, against a one per cent tolerance.
constexpr double kCountDistance = 0.13;

// Probe length for one point of the aperture scan. The hold is periodic and
// starts from a known phase, so there is no transient to skip.
constexpr double kProbeSeconds = 0.05;

// The coarse scan reads the whole band rather than a window around the hold
// rate, so it can land somewhere the configuration did not ask for; the fine
// one refines inside a single step of it.
constexpr int kCoarsePoints = 96;
constexpr double kCoarseLowHz = 300.0;
constexpr int kFinePoints = 201;
constexpr double kFineSpan = 0.05;

// How deep a dip has to be before the scan will call it the first null, and how
// far the profile has to come back up afterwards before the scan stops looking.
// The aperture's shoulders between its nulls sit near 13 dB down, so the floor
// is well clear of one, and the scan takes the first null rather than the
// deepest: a hold with a fractional period puts its deepest null at two or three
// times the hold rate, which is a real feature and the wrong one.
constexpr double kNullFloorDb = -20.0;
constexpr double kNullTurnDb = 3.0;

BitCrusherConfig only_hold(float hold_hz) {
  BitCrusherConfig config;
  config.quantizer_mode = QuantizerMode::kOff;
  config.hold_hz = hold_hz;
  return config;
}

/// Magnitude the hold returns at one frequency, read by demodulating the probe
/// tone itself: the aperture at that frequency, with the images it folds
/// elsewhere left out.
double aperture_db(float hold_hz, double sample_rate, double probe_hz) {
  const std::size_t count = static_cast<std::size_t>(sample_rate * kProbeSeconds);
  const double step = kTwoPiD * probe_hz / sample_rate;
  std::vector<float> probe(count);
  for (std::size_t i = 0; i < count; ++i) {
    probe[i] = static_cast<float>(std::sin(step * static_cast<double>(i)));
  }
  BitCrusher processor(only_hold(hold_hz));
  processor.prepare(sample_rate, static_cast<int>(count));
  std::vector<float> held = probe;
  float* channels[] = {held.data()};
  processor.process(channels, 1, static_cast<int>(count));
  std::complex<double> in(0.0, 0.0);
  std::complex<double> out(0.0, 0.0);
  for (std::size_t i = 0; i < count; ++i) {
    const double angle = step * static_cast<double>(i);
    const std::complex<double> turn(std::cos(angle), -std::sin(angle));
    in += static_cast<double>(probe[i]) * turn;
    out += static_cast<double>(held[i]) * turn;
  }
  return 20.0 * std::log10(std::abs(out) / std::abs(in) + 1e-30);
}

struct Null {
  double hz;
  double depth_db;
};

/// The aperture's first null. Found by scanning the band from 300 Hz upward
/// until the profile has dipped past the floor and turned back up, then
/// refining inside one step of that scan.
Null first_null_hz(float hold_hz, double sample_rate) {
  const double top = sample_rate * 0.49;
  Null best{kCoarseLowHz, 0.0};
  for (int i = 0; i < kCoarsePoints; ++i) {
    const double hz =
        kCoarseLowHz * std::pow(top / kCoarseLowHz, static_cast<double>(i) / (kCoarsePoints - 1));
    const double db = aperture_db(hold_hz, sample_rate, hz);
    if (i == 0 || db < best.depth_db) {
      best = {hz, db};
    } else if (best.depth_db < kNullFloorDb && db > best.depth_db + kNullTurnDb) {
      break;
    }
  }
  const double centre = best.hz;
  for (int i = 0; i < kFinePoints; ++i) {
    const double hz =
        centre * (1.0 - kFineSpan + 2.0 * kFineSpan * static_cast<double>(i) / (kFinePoints - 1));
    const double db = aperture_db(hold_hz, sample_rate, hz);
    if (db < best.depth_db) {
      best = {hz, db};
    }
  }
  return best;
}

/// How far @p value sits from the nearest whole number.
double distance_from_whole(double value) { return std::fabs(value - std::round(value)); }

// --- pitch shifter ----------------------------------------------------------

// Windows the beat period is read at: the shipped default, and one of the
// lengths a mode byte was measured to select.
constexpr double kPitchWindowMs[] = {22.5, 63.78};
constexpr float kPitchSemitones = 7.0f;

// Frequency every probe is arranged to come out at, so the envelope reader
// meets the same conditions at either rate.
constexpr double kProbeOutHz = 220.0;
constexpr double kGrainsSkipped = 2.5;  ///< the buffer starts empty, so the first grain is startup.
constexpr int kBeatsRead = 6;
constexpr double kEnvelopeRateHz = 4000.0;

double pitch_ratio(float semitones) { return std::exp2(static_cast<double>(semitones) / 12.0); }

/// The law the record states: the output repeats once per window of drift.
double law_beat_period_s(float semitones, double window_ms) {
  return window_ms * 0.001 / std::fabs(pitch_ratio(semitones) - 1.0);
}

PitchShifterConfig shifted(float semitones, double window_ms) {
  PitchShifterConfig config;
  config.semitones = semitones;
  config.dry_wet = 1.0f;
  config.window_ms = static_cast<float>(window_ms);
  return config;
}

/// Probe tone for a window and ratio: the multiple of `1 / window` that lands
/// the shifted output nearest kProbeOutHz. A multiple of `1 / window` is what
/// puts the two taps -- which sit one window apart -- in phase, so the splice
/// ripple the reader follows is at its deepest.
double probe_tone_hz(double window_ms, double ratio) {
  const double window_s = window_ms * 0.001;
  return std::max(1.0, std::round(kProbeOutHz / ratio * window_s)) / window_s;
}

/// Quadrature magnitude of a single-tone buffer, block-averaged. The
/// quarter-period offset is interpolated rather than rounded: a rounded one
/// leaves a ripple at twice @p out_hz, which puts small maxima on the flank of
/// the autocorrelation peak below and biases it early.
std::vector<double> envelope(const std::vector<float>& x, std::size_t skip, double out_hz,
                             double sample_rate, int decimate) {
  const double quarter = std::max(1.0, sample_rate / (4.0 * out_hz));
  const std::size_t whole = static_cast<std::size_t>(quarter);
  const double frac = quarter - static_cast<double>(whole);
  std::vector<double> env;
  double acc = 0.0;
  int held = 0;
  for (std::size_t i = skip; i + whole + 1 < x.size(); ++i) {
    const double a = x[i];
    const double b = x[i + whole] * (1.0 - frac) + x[i + whole + 1] * frac;
    acc += std::sqrt(a * a + b * b);
    if (++held == decimate) {
      env.push_back(acc / decimate);
      acc = 0.0;
      held = 0;
    }
  }
  return env;
}

/// First periodicity of @p env in samples, by normalised autocorrelation past
/// the zero-lag lobe, with the peak parabolically refined. The search reaches a
/// third of the envelope, which is over twice the period a render is sized for,
/// so returning double the true period is a result this can produce. Returns 0
/// when nothing periodic is found.
double first_period(const std::vector<double>& env, std::size_t lag_lo) {
  const std::size_t lag_hi = env.size() / 3;
  if (lag_hi < lag_lo + 2) return 0.0;
  double mean = 0.0;
  for (const double v : env) mean += v;
  mean /= static_cast<double>(env.size());
  std::vector<double> e(env.size());
  for (std::size_t i = 0; i < env.size(); ++i) e[i] = env[i] - mean;
  const std::size_t span = e.size() - lag_hi - 1;
  double e0 = 0.0;
  for (std::size_t i = 0; i < span; ++i) e0 += e[i] * e[i];
  if (e0 <= 0.0) return 0.0;
  std::vector<double> r(lag_hi + 1, 0.0);
  for (std::size_t l = 0; l <= lag_hi; ++l) {
    double num = 0.0;
    double den = 0.0;
    for (std::size_t i = 0; i < span; ++i) {
      num += e[i] * e[i + l];
      den += e[i + l] * e[i + l];
    }
    r[l] = den > 0.0 ? num / std::sqrt(e0 * den) : 0.0;
  }
  // The first stretch that clears 0.5 after the zero-lag lobe has died, taken
  // whole: its largest lag is the period. Reading the first local maximum
  // instead would stop on any wrinkle riding the peak's leading flank.
  bool dipped = false;
  std::size_t peak = 0;
  for (std::size_t l = lag_lo; l + 1 <= lag_hi; ++l) {
    if (r[l] < 0.1) dipped = true;
    if (!dipped) continue;
    if (r[l] > 0.5) {
      if (peak == 0 || r[l] > r[peak]) peak = l;
    } else if (peak != 0) {
      break;
    }
  }
  if (peak == 0 || peak + 1 > lag_hi) return 0.0;
  const double denom = r[peak - 1] - 2.0 * r[peak] + r[peak + 1];
  const double delta = denom != 0.0 ? 0.5 * (r[peak - 1] - r[peak + 1]) / denom : 0.0;
  return static_cast<double>(peak) + delta;
}

/// Beat period of @p window_ms in seconds, read off a render long enough to
/// carry kBeatsRead of it. The law sizes the render; it does not reach the
/// estimator.
double beat_period_s(float semitones, double window_ms, double sample_rate) {
  const PitchShifterConfig config = shifted(semitones, window_ms);
  const double ratio = pitch_ratio(semitones);
  const double tone_hz = probe_tone_hz(window_ms, ratio);
  const double skip_s = 2.0 * kGrainsSkipped * window_ms * 0.001;
  const std::size_t skip = static_cast<std::size_t>(sample_rate * skip_s);
  const int samples = static_cast<int>(
      sample_rate * (skip_s + kBeatsRead * law_beat_period_s(semitones, window_ms)) + 4096.0);
  const int decimate = static_cast<int>(std::max(1.0, std::round(sample_rate / kEnvelopeRateHz)));
  const double env_rate = sample_rate / decimate;
  PitchShifter shifter(config);
  shifter.prepare(sample_rate, samples);
  std::vector<float> buf = sonare::test::generate_sine(samples, static_cast<float>(tone_hz),
                                                       static_cast<int>(sample_rate), 0.5f);
  sonare::test::process(shifter, buf);
  const std::vector<double> env = envelope(buf, skip, tone_hz * ratio, sample_rate, decimate);
  return first_period(env, static_cast<std::size_t>(env_rate * 0.003)) / env_rate;
}

// --- rotary -----------------------------------------------------------------

// Time constants the glide is read at, in seconds, spanning a factor of
// twenty-five.
constexpr float kGlideTauS[] = {0.01f, 0.05f, 0.25f};
constexpr float kGlideFromHz = 3.0f;
constexpr float kGlideToHz = 9.0f;

// Stimulus for the one rotary reading taken off the audio rather than the rate.
constexpr float kGlideStimulusHz = 220.0f;

RotaryConfig gliding(float tau_s) {
  RotaryConfig config;
  config.rate_hz = kGlideFromHz;
  config.drum_rate_hz = kGlideFromHz;
  config.accel_tau_s = tau_s;
  config.decel_tau_s = tau_s;
  return config;
}

/// How long the horn rotor takes to close all but one part in e of the gap to a
/// new target, in seconds. Read off the rate the rotor is actually turning at,
/// which the per-sample coefficient prepare() derives drives: a coefficient
/// carried across a rate change moves this reading, where reading the field
/// back would not.
double glide_tau_s(float tau_s, double sample_rate) {
  Rotary rotary(gliding(tau_s));
  rotary.prepare(sample_rate, 1);
  REQUIRE(rotary.set_parameter(0, kGlideToHz));
  const double gap = static_cast<double>(kGlideToHz) - static_cast<double>(kGlideFromHz);
  const double remaining = gap / std::exp(1.0);
  const int limit = static_cast<int>(sample_rate * 4.0 * static_cast<double>(tau_s)) + 8;
  float sample = 0.0f;
  float* channels[] = {&sample};
  double before = gap;
  for (int n = 1; n <= limit; ++n) {
    sample = 0.0f;
    rotary.process(channels, 1, 1);
    const double after = static_cast<double>(kGlideToHz) - rotary.horn_rate_hz();
    if (after <= remaining) {
      const double t = (before - remaining) / (before - after);
      return (static_cast<double>(n) - 1.0 + t) / sample_rate;
    }
    before = after;
  }
  return 0.0;
}

// --- parametric EQ ----------------------------------------------------------

// Band-type selectors, in the order the insert's params decode them.
constexpr int kEqPeak = 0;
constexpr int kEqLowShelf = 1;
constexpr int kEqHighShelf = 2;

// The output stage's tone pair at both shelf orders and one peaking section.
// Built from the same JSON keys the GS layer writes, so the reading covers the
// path it drives; the first-order pair is the one it writes.
struct EqProbe {
  const char* what;
  int type;
  float corner_hz;
  int slope_db_oct;
};
constexpr EqProbe kEqProbes[] = {
    {"first-order low shelf", kEqLowShelf, 161.0f, 6},
    {"first-order high shelf", kEqHighShelf, 6987.0f, 6},
    {"second-order low shelf", kEqLowShelf, 161.0f, 12},
    {"second-order high shelf", kEqHighShelf, 6987.0f, 12},
    {"peak", kEqPeak, 1585.0f, 12},
};
constexpr double kEqGainDb = 12.0;

std::vector<float> eq_impulse(const EqProbe& probe, double sample_rate) {
  std::ostringstream json;
  json << "{\"band0.type\":" << probe.type << ",\"band0.frequencyHz\":" << probe.corner_hz
       << ",\"band0.gainDb\":" << kEqGainDb << ",\"band0.slopeDbOct\":" << probe.slope_db_oct
       << "}";
  auto eq = sonare::mastering::api::make_insert("eq.parametric", json.str());
  REQUIRE(eq != nullptr);
  eq->prepare(sample_rate, kFftLength);
  std::vector<float> response = sonare::test::generate_impulse(kFftLength);
  sonare::test::process(*eq, response);
  return response;
}

/// The response's gain at one frequency, summed directly rather than read off a
/// bin, so a position is not quantised to the transform's grid.
double response_db(const std::vector<float>& response, double hz, double sample_rate) {
  std::complex<double> sum = 0.0;
  const double w = kTwoPiD * hz / sample_rate;
  for (std::size_t n = 0; n < response.size(); ++n) {
    sum += static_cast<double>(response[n]) * std::polar(1.0, -w * static_cast<double>(n));
  }
  return 20.0 * std::log10(std::abs(sum) + 1e-30);
}

/// Where a shelf's gain crosses half its full cut or boost, bisected in log
/// frequency over three octaves either side of the corner. Zero where the
/// response does not straddle that level, which is also a band never applied.
double half_gain_hz(const std::vector<float>& response, float corner_hz, double sample_rate) {
  double lo = static_cast<double>(corner_hz) / 8.0;
  double hi = std::min(static_cast<double>(corner_hz) * 8.0, sample_rate * 0.49);
  const double half = kEqGainDb / 2.0;
  const double at_lo = response_db(response, lo, sample_rate) - half;
  if (at_lo * (response_db(response, hi, sample_rate) - half) >= 0.0) return 0.0;
  for (int i = 0; i < 60; ++i) {
    const double mid = std::sqrt(lo * hi);
    if ((response_db(response, mid, sample_rate) - half) * at_lo > 0.0) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return std::sqrt(lo * hi);
}

/// Where a peaking section's gain is highest, by golden section in log
/// frequency over an octave either side of the corner.
double peak_centre_hz(const std::vector<float>& response, float corner_hz, double sample_rate) {
  const double golden = (std::sqrt(5.0) - 1.0) / 2.0;
  double lo = std::log(static_cast<double>(corner_hz) / 2.0);
  double hi = std::log(static_cast<double>(corner_hz) * 2.0);
  for (int i = 0; i < 80; ++i) {
    const double a = hi - golden * (hi - lo);
    const double b = lo + golden * (hi - lo);
    if (response_db(response, std::exp(a), sample_rate) >
        response_db(response, std::exp(b), sample_rate)) {
      hi = b;
    } else {
      lo = a;
    }
  }
  return std::exp((lo + hi) / 2.0);
}

std::string at_rate(double sample_rate) {
  return " at " + std::to_string(static_cast<int>(sample_rate)) + " Hz";
}

std::string with_value(const std::string& what, double value) {
  std::ostringstream label;
  label << what << " (" << value << ")";
  return label.str();
}

}  // namespace

TEST_CASE("each insert's named physical quantity is the one asked for, at 44100 and 48000 Hz",
          "[insert-sample-rate]") {
  Tally tally;
  const double rates[] = {kAltHostRate, kHostRate};

  // --- phaser: the first notch, in hertz ------------------------------------
  // The notch is not a field read back -- it is where eight sections' phase has
  // accumulated to antiphase -- so the per-rate assertion is against the
  // position the section count and the bilinear warp fix, computed here.
  for (const float corner : kPhaserCornerHz) {
    double measured[2] = {0.0, 0.0};
    for (std::size_t r = 0; r < 2; ++r) {
      const double rate = rates[r];
      const std::string where = "the phaser's first notch, corner " +
                                std::to_string(static_cast<int>(corner)) + " Hz" + at_rate(rate);
      measured[r] = first_antiphase_hz(corner, rate);
      tally.at_least(measured[r], 1.0, where + " is readable at all");

      // Against the corner asked for, per rate: the notch is where the section
      // count and this rate's warp put it. The cross-rate check below does not
      // reach this, because two rates can carry one wrong notch and agree.
      const double predicted = first_notch_hz(corner, kPhaserSections, rate);
      tally.within(measured[r], predicted, kNotchTolerance,
                   where + ", against the position the corner and the section count fix");

      // And the phase reading names the audible feature: the deepest
      // cancellation a full-band scan of the dry sum finds is the same one.
      const Spectrum summed =
          impulse_spectrum<Phaser>(held_still(corner, 1.0f, PhaserMixMode::kDrySum), rate);
      const std::vector<double> notches = cancellations(summed.db, rate, rate * 0.49);
      tally.same(notches.size() == static_cast<std::size_t>(kPhaserSections / 2),
                 where + ": eight sections cancel four times in the band (found " +
                     std::to_string(notches.size()) + ")");
      if (!notches.empty()) {
        tally.at_most(std::fabs(notches[0] - measured[r]), 2.0 * bin_hz(1, rate),
                      where + ": the dry sum cancels where the phase reaches antiphase");
      }
    }
    // And the two host rates have to agree with each other, which is what a
    // corner in hertz buys: stability across a rate change, as distinct from
    // being the right frequency at either of them. Neither check subsumes the
    // other, and which one sees a given defect is not knowable in advance.
    tally.within(measured[0], measured[1], kNotchTolerance,
                 "the phaser's first notch lands on one frequency at both rates, corner " +
                     std::to_string(static_cast<int>(corner)) + " Hz");
  }

  // The notch is not sample-rate invariant just because the corner is, and this
  // is where it stops being: at 9 kHz the two rates' warps put the first notch
  // 2.5 per cent apart, over the tolerance the three corners above pass under.
  // Pinned rather than left out, so the limit is an assertion and not a gap.
  {
    double measured[2] = {0.0, 0.0};
    for (std::size_t r = 0; r < 2; ++r) {
      measured[r] = first_antiphase_hz(kPhaserWarpedCornerHz, rates[r]);
      tally.within(measured[r], first_notch_hz(kPhaserWarpedCornerHz, kPhaserSections, rates[r]),
                   kNotchTolerance,
                   "the phaser's first notch at a 9 kHz corner" + at_rate(rates[r]));
    }
    const double predicted_apart =
        std::fabs(first_notch_hz(kPhaserWarpedCornerHz, kPhaserSections, rates[0]) -
                  first_notch_hz(kPhaserWarpedCornerHz, kPhaserSections, rates[1])) /
        measured[1];
    tally.at_least(predicted_apart, kNotchTolerance,
                   "a 9 kHz corner's notch is expected to move between the rates");
    tally.within(std::fabs(measured[0] - measured[1]) / measured[1], predicted_apart, 0.25,
                 "and it moves by what the warp says, not by more");
  }

  // --- stereo delay: the damping's half-power corner, in hertz ---------------
  for (const float corner : kDampingCornerHz) {
    double measured[2] = {0.0, 0.0};
    for (std::size_t r = 0; r < 2; ++r) {
      const double rate = rates[r];
      const std::string where = "the delay's damping corner, asked for " +
                                std::to_string(static_cast<int>(corner)) + " Hz" + at_rate(rate);
      const Spectrum response = impulse_spectrum<StereoDelay>(bare_delay(corner), rate);
      measured[r] = first_crossing_hz(response.db, rate, kHalfPowerDb);
      tally.at_least(measured[r], 1.0, where + " is readable at all");
      // Against what was asked for, per rate; the cross-rate check below is the
      // other half of the pair and neither one covers the other.
      tally.within(measured[r], one_pole_half_power_hz(corner, rate), kDampingTolerance,
                   where + ", against the pole the corner and the rate build");
    }
    tally.within(measured[0], measured[1], kDampingTolerance,
                 "the delay's damping corner lands on one frequency at both rates, asked for " +
                     std::to_string(static_cast<int>(corner)) + " Hz");
  }

  // --- chorus and flanger: the pre-filter's half-power corner, in hertz ------
  // The same section, run per instance, so both types are read.
  for (const float corner : kPreFilterCornerHz) {
    const std::string asked = std::to_string(static_cast<int>(corner)) + " Hz";
    double chorus_measured[2] = {0.0, 0.0};
    double flanger_measured[2] = {0.0, 0.0};
    for (std::size_t r = 0; r < 2; ++r) {
      const double rate = rates[r];
      const double predicted = one_pole_half_power_hz(corner, rate);
      chorus_measured[r] =
          pre_filter_corner_hz<Chorus>(still_chorus(PreFilterMode::kLowPass, corner),
                                       still_chorus(PreFilterMode::kOff, corner), rate);
      flanger_measured[r] =
          pre_filter_corner_hz<Flanger>(still_flanger(PreFilterMode::kLowPass, corner),
                                        still_flanger(PreFilterMode::kOff, corner), rate);
      tally.at_least(chorus_measured[r], 1.0,
                     "the chorus pre-filter corner, asked for " + asked + at_rate(rate) +
                         ", is readable at all");
      // Against what was asked for, per rate; the cross-rate check below is the
      // other half of the pair and neither one covers the other.
      tally.within(chorus_measured[r], predicted, kPreFilterTolerance,
                   "the chorus pre-filter corner, asked for " + asked + at_rate(rate) +
                       ", against the pole the corner and the rate build");
      tally.within(flanger_measured[r], predicted, kPreFilterTolerance,
                   "the flanger pre-filter corner, asked for " + asked + at_rate(rate) +
                       ", against the same pole");
    }
    tally.within(
        chorus_measured[0], chorus_measured[1], kPreFilterTolerance,
        "the chorus pre-filter corner lands on one frequency at both rates, asked for " + asked);
    tally.within(
        flanger_measured[0], flanger_measured[1], kPreFilterTolerance,
        "the flanger pre-filter corner lands on one frequency at both rates, asked for " + asked);
  }

  // --- lofi: the first aperture null, in hertz -------------------------------
  for (const float hold : kHoldHz) {
    const std::string asked = with_value("hold", hold);
    double measured[2] = {0.0, 0.0};
    for (std::size_t r = 0; r < 2; ++r) {
      const double rate = rates[r];
      // Without this the pair below would pass on a hold that happens to be a
      // whole number of samples at both rates, where a hold kept as a count is
      // the same hold and nothing here could tell.
      tally.at_least(distance_from_whole(rate / static_cast<double>(hold)), kCountDistance,
                     asked + at_rate(rate) + " is not a whole number of samples");
      const Null found = first_null_hz(hold, rate);
      measured[r] = found.hz;
      tally.at_most(found.depth_db, kNullFloorDb,
                    "the lofi profile the reading landed on, " + asked + at_rate(rate) +
                        ", is a null and not a shoulder");
      // Against what was asked for, per rate; the cross-rate check below is the
      // other half of the pair and neither one covers the other.
      tally.within(measured[r], static_cast<double>(hold), kNullTolerance,
                   "the lofi aperture null, " + asked + at_rate(rate) +
                       ", against the rate the hold is stored as");
    }
    tally.within(measured[0], measured[1], kNullTolerance,
                 "the lofi aperture null lands on one frequency at both rates, " + asked);
  }

  // --- pitch shifter: the beat period, in seconds ---------------------------
  for (const double window_ms : kPitchWindowMs) {
    const std::string asked = with_value("window", window_ms) + " ms";
    const double expected = law_beat_period_s(kPitchSemitones, window_ms);
    double measured[2] = {0.0, 0.0};
    for (std::size_t r = 0; r < 2; ++r) {
      const double rate = rates[r];
      measured[r] = beat_period_s(kPitchSemitones, window_ms, rate);
      tally.at_least(measured[r], 1e-4,
                     "the pitch shifter's beat, " + asked + at_rate(rate) + ", is readable at all");
      // Against what was asked for, per rate; the cross-rate check below is the
      // other half of the pair and neither one covers the other.
      tally.within(measured[r], expected, kBeatTolerance,
                   "the pitch shifter's beat, " + asked + at_rate(rate) +
                       ", against the window over |ratio - 1|");
    }
    tally.within(measured[0], measured[1], kBeatTolerance,
                 "the pitch shifter's beat period in seconds is the same at both rates, " + asked);
  }

  // --- rotary: the acceleration time constant, in seconds -------------------
  for (const float tau : kGlideTauS) {
    const std::string asked = with_value("tau", tau) + " s";
    double measured[2] = {0.0, 0.0};
    for (std::size_t r = 0; r < 2; ++r) {
      const double rate = rates[r];
      measured[r] = glide_tau_s(tau, rate);
      tally.at_least(measured[r], 1e-6,
                     "the rotary's glide, " + asked + at_rate(rate) + ", settles at all");
      // Against what was asked for, per rate; the cross-rate check below is the
      // other half of the pair and neither one covers the other.
      tally.within(measured[r], static_cast<double>(tau), kGlideTolerance,
                   "the rotary's acceleration time constant, " + asked + at_rate(rate) +
                       ", against the time constant asked for");
    }
    tally.within(measured[0], measured[1], kGlideTolerance,
                 "the rotary's acceleration time constant lasts as long at both rates, " + asked);
  }

  // The glide reaches the audio and not only the rotor's own accessor: two
  // instances differing in nothing but the time constant render differently
  // while the glide is in flight.
  {
    const int samples = static_cast<int>(kHostRate * 0.1);
    std::vector<float> immediate =
        sonare::test::generate_sine(samples, kGlideStimulusHz, static_cast<int>(kHostRate), 0.5f);
    std::vector<float> glided = immediate;
    Rotary at_once(gliding(0.0f));
    Rotary slowly(gliding(kGlideTauS[2]));
    at_once.prepare(kHostRate, samples);
    slowly.prepare(kHostRate, samples);
    REQUIRE(at_once.set_parameter(0, kGlideToHz));
    REQUIRE(slowly.set_parameter(0, kGlideToHz));
    sonare::test::process(at_once, immediate);
    sonare::test::process(slowly, glided);
    tally.same(immediate != glided, "the glide reaches the audio, not only the rotor's rate");
  }

  // --- parametric EQ: shelf half-gain points and a peak's centre, in hertz --
  for (const EqProbe& probe : kEqProbes) {
    const std::string asked = std::string(probe.what) + ", asked for " +
                              std::to_string(static_cast<int>(probe.corner_hz)) + " Hz";
    const bool shelf = probe.type != kEqPeak;
    double measured[2] = {0.0, 0.0};
    for (std::size_t r = 0; r < 2; ++r) {
      const double rate = rates[r];
      const std::vector<float> response = eq_impulse(probe, rate);
      measured[r] = shelf ? half_gain_hz(response, probe.corner_hz, rate)
                          : peak_centre_hz(response, probe.corner_hz, rate);
      tally.at_least(measured[r], 1.0, "the EQ " + asked + at_rate(rate) + ", is readable at all");
      if (!shelf) {
        tally.within(response_db(response, measured[r], rate), kEqGainDb, kEqTolerance,
                     "the EQ " + asked + at_rate(rate) + ", reaches its gain at the centre");
      }
      // Against what was asked for, per rate; the cross-rate check below is the
      // other half of the pair and neither one covers the other.
      tally.within(measured[r], static_cast<double>(probe.corner_hz), kEqTolerance,
                   "the EQ " + asked + at_rate(rate) + ", against the corner asked for");
    }
    tally.within(measured[0], measured[1], kEqTolerance,
                 "the EQ " + asked + ", lands on one frequency at both rates");
  }

  WARN("comparisons: " << tally.count());
  REQUIRE(tally.count() >= 87);
}
