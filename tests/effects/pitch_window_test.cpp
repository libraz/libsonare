/// @file pitch_window_test.cpp
/// @brief The measured pitch-shifter structure: a pitch byte is not a rate but
///        a read-out ratio, and the audible beat period is the window divided
///        by how far the ratio sits from unity.
///
/// Every expectation is a structural quantity -- a period, a ratio between two
/// periods, the agreement of one period read at two sample rates -- measured
/// off the processor's own output. None is a recorded waveform, and none could
/// be: the contract this module keeps is the control protocol and not the audio
/// (src/midi/synth/docs/gs.md).
///
/// The five window lengths below are the states a mode byte was measured to
/// select. They are inputs here, not expectations: this case asserts the law
/// `beat_period = window / |ratio - 1|` holds at each of them, not that a
/// device's state 2 is 63.78 ms. What the record leaves open -- whether the
/// distance between the two stored read-outs follows the mode byte or is a
/// constant -- is asserted nowhere below.
///
/// Reach is an output: the case reports how many comparisons it made, because
/// a run that compared nothing looks exactly like a run that passed.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

#include "effects/modulation/pitch_shifter.h"
#include "support/audio_fixtures.h"

namespace {

using sonare::effects::modulation::PitchShifter;
using sonare::effects::modulation::PitchShifterConfig;
using sonare::test::generate_sine;

/// Counts every comparison it makes, so the case can report its own reach.
class Tally {
 public:
  void within(double got, double reading, double fraction, const std::string& what) {
    ++count_;
    const double tolerance = std::fabs(reading) * fraction;
    INFO(what << ": got " << got << ", expected " << reading << ", tolerance " << tolerance);
    CHECK(std::fabs(got - reading) <= tolerance);
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

// The five windows a mode byte was measured to select, in milliseconds. Their
// provenance differs and the record says so: states 0-3 are read at a major
// third, where every rate falls inside the reading's own band; state 4 named no
// rate there, rests on the octave alone, and is published as not known to the
// precision of the other four.
constexpr double kModeWindowMs[] = {32.00, 42.68, 63.78, 85.27, 128.0};

// The window the shipped default produces, measured off the implementation
// before the window became a configuration field. Spelled out here rather than
// read from the header, so moving the header's default is caught.
constexpr float kShippedWindowMs = 22.5f;

// Tolerance on every period below, as a fraction. It bounds the estimator
// here, not the reading: the archive's own figures are tighter (0.046% between
// shifters, 0.31% worst between shifts) and the reader below lands inside 0.5%.
constexpr double kPeriodTolerance = 0.01;

// Tolerance on the distance between the two read-outs. The reader below lands
// inside 0.04% on every window here, so this is loose against the estimator and
// fiftyfold tight against the error it exists to catch, a factor of two.
constexpr double kSeparationTolerance = 0.01;

// Shift the separation is read at. Near unity the taps close on a stored sample
// slowly enough that each reads it once, so the pair in the output is the pair
// in the line; at wider shifts a tap re-reads across its own wrap and the
// output carries more than two.
constexpr float kSeparationSemitones[] = {-2.0f, 2.0f};

// Rates the beat period is required to come out the same at.
constexpr double kHostRate = 48000.0;
constexpr double kAltRate = 44100.0;

// The grain buffer starts empty and a tap reads a whole grain back, so the
// startup transient runs one grain -- two windows -- long. Two and a half of
// them are dropped before anything is read.
constexpr double kGrainsSkipped = 2.5;
constexpr int kBeatsRead = 8;  ///< beats a render is made long enough to carry.

double pitch_ratio(float semitones) { return std::exp2(static_cast<double>(semitones) / 12.0); }

// Frequency every probe is arranged to come out at, so the envelope reader
// below meets the same conditions whatever the ratio under test.
constexpr double kProbeOutHz = 220.0;

/// Probe tone for a window and ratio: the multiple of `1 / window` that lands
/// the shifted output nearest kProbeOutHz. A multiple of `1 / window` is what
/// puts the two taps -- which sit one window apart -- in phase, so the splice
/// ripple is at its deepest. The unit was read by following the partials of a
/// held note; one placed partial does here what several did there.
double probe_tone_hz(double window_ms, double ratio) {
  const double window_s = window_ms * 0.001;
  return std::max(1.0, std::round(kProbeOutHz / ratio * window_s)) / window_s;
}

std::vector<float> render(const PitchShifterConfig& config, double sample_rate, double tone_hz,
                          int samples) {
  PitchShifter shifter(config);
  shifter.prepare(sample_rate, samples);
  std::vector<float> buf =
      generate_sine(samples, static_cast<float>(tone_hz), static_cast<int>(sample_rate), 0.5f);
  float* channels[] = {buf.data()};
  shifter.process(channels, 1, samples);
  return buf;
}

/// Quadrature magnitude of a single-tone buffer, block-averaged down to about
/// 6 kHz. The quarter-period offset is interpolated rather than rounded: a
/// rounded one leaves a ripple at twice @p out_hz, which puts small maxima on
/// the flank of the autocorrelation peak below and biases it early.
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
  for (double v : env) mean += v;
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

/// Beat period of @p config in seconds, read off a render long enough to carry
/// kBeatsRead of @p expected_s. The expectation sizes the render; it does not
/// reach the estimator.
double beat_period_s(const PitchShifterConfig& config, double sample_rate, double expected_s) {
  const double ratio = pitch_ratio(config.semitones);
  const double tone_hz = probe_tone_hz(static_cast<double>(config.window_ms), ratio);
  const double skip_s = 2.0 * kGrainsSkipped * static_cast<double>(config.window_ms) * 0.001;
  const std::size_t skip = static_cast<std::size_t>(sample_rate * skip_s);
  const int samples = static_cast<int>(sample_rate * (skip_s + kBeatsRead * expected_s)) + 4096;
  const int decimate = static_cast<int>(std::max(1.0, std::round(sample_rate / 6000.0)));
  const double env_rate = sample_rate / decimate;
  const std::vector<float> wet = render(config, sample_rate, tone_hz, samples);
  const std::vector<double> env = envelope(wet, skip, tone_hz * ratio, sample_rate, decimate);
  return first_period(env, static_cast<std::size_t>(env_rate * 0.003)) / env_rate;
}

/// The output instants at which the taps read one stored impulse, in samples.
/// Both taps sweep the delay line at the pitch ratio, so a separation of one
/// window in the line arrives `window / ratio` later. The processor is
/// pre-rolled on silence first, only so neither crossfade gain sits near zero
/// when its tap arrives; the impulse itself is the whole measurement.
std::vector<double> impulse_read_outs(const PitchShifterConfig& config, double sample_rate) {
  const double grain_s = 2.0 * static_cast<double>(config.window_ms) * 0.001;
  const std::size_t lead = static_cast<std::size_t>(sample_rate * grain_s * 0.25);
  const int samples = static_cast<int>(sample_rate * grain_s * 2.5) + static_cast<int>(lead);
  PitchShifter shifter(config);
  shifter.prepare(sample_rate, samples);
  std::vector<float> buf(static_cast<std::size_t>(samples), 0.0f);
  buf[lead] = 1.0f;
  float* channels[] = {buf.data()};
  shifter.process(channels, 1, samples);
  // Everything but the read-outs is exactly zero, so a read-out is a run of
  // non-zero samples -- two wide, because the read interpolates -- and the
  // run's centroid is where it landed.
  std::vector<double> at;
  std::size_t i = lead;
  while (i < buf.size()) {
    if (buf[i] == 0.0f) {
      ++i;
      continue;
    }
    double weight = 0.0;
    double moment = 0.0;
    while (i < buf.size() && buf[i] != 0.0f) {
      weight += std::fabs(buf[i]);
      moment += std::fabs(buf[i]) * static_cast<double>(i);
      ++i;
    }
    at.push_back(moment / weight);
  }
  return at;
}

PitchShifterConfig shifted(float semitones, float window_ms) {
  PitchShifterConfig config;
  config.semitones = semitones;
  config.dry_wet = 1.0f;
  config.window_ms = window_ms;
  return config;
}

/// The law the record states: the output repeats once per window of drift.
double law_period_s(float semitones, double window_ms) {
  return window_ms * 0.001 / std::fabs(pitch_ratio(semitones) - 1.0);
}

/// Reads the beat period and checks it against the law, returning what it read.
double check_law(Tally& tally, float semitones, double window_ms, double sample_rate,
                 const std::string& what) {
  const double expected = law_period_s(semitones, window_ms);
  const double got =
      beat_period_s(shifted(semitones, static_cast<float>(window_ms)), sample_rate, expected);
  std::ostringstream label;
  label << what << " (" << semitones << " semitones, " << window_ms << " ms window, " << sample_rate
        << " Hz)";
  tally.within(got, expected, kPeriodTolerance, label.str());
  return got;
}

}  // namespace

TEST_CASE("the pitch shifter's window length and beat period match the measured structure",
          "[pitch-window]") {
  Tally tally;

  // A pitch byte drives no oscillator: at one window, the beat period tracks
  // the distance the ratio sits from unity. The four ratios below span the
  // coarse byte's own sweep, a quarter to double, which is |r - 1| 0.25 to 1.00.
  double octave_period = 0.0;
  double fifth_period = 0.0;
  for (float semitones : {-24.0f, -12.0f, 7.0f, 12.0f}) {
    const double got = check_law(tally, semitones, kModeWindowMs[2], kHostRate,
                                 "a pitch byte is a read-out ratio, not a rate");
    if (semitones == 12.0f) octave_period = got;
    if (semitones == 7.0f) fifth_period = got;
  }
  // Inverse proportion, not merely the same direction: the two measured periods
  // stand in the ratio the two |r - 1| do.
  tally.within(fifth_period / octave_period, 1.0 / std::fabs(pitch_ratio(7.0f) - 1.0),
               kPeriodTolerance, "the fifth beats slower than the octave in inverse proportion");

  // The mode byte moves the window and nothing else, over a factor of four.
  for (double window_ms : kModeWindowMs) {
    check_law(tally, 12.0f, window_ms, kHostRate, "each window the mode byte selects");
  }

  // The two taps sit one whole window apart in the delay line, not half of one.
  // Reading the beat period alone cannot see this: that law is self-consistent
  // under either convention as long as the same one is used on both sides.
  for (double window_ms :
       {static_cast<double>(kShippedWindowMs), kModeWindowMs[0], kModeWindowMs[1], kModeWindowMs[2],
        kModeWindowMs[3], kModeWindowMs[4]}) {
    for (float semitones : kSeparationSemitones) {
      const std::vector<double> at =
          impulse_read_outs(shifted(semitones, static_cast<float>(window_ms)), kHostRate);
      std::ostringstream label;
      label << "one stored sample is read out twice, a whole window apart and not half a one ("
            << semitones << " semitones, " << window_ms << " ms window)";
      tally.same(at.size() == 2, label.str() + ": two read-outs reached");
      if (at.size() != 2) continue;
      tally.within((at[1] - at[0]) / kHostRate, window_ms * 0.001 / pitch_ratio(semitones),
                   kSeparationTolerance, label.str());
    }
  }

  // The window is held in milliseconds, so the beat period in seconds -- the
  // quantity this insert preserves -- is the same at two host rates.
  for (double window_ms : {kModeWindowMs[0], kModeWindowMs[2], kModeWindowMs[4]}) {
    const double expected = law_period_s(12.0f, window_ms);
    const double host =
        beat_period_s(shifted(12.0f, static_cast<float>(window_ms)), kHostRate, expected);
    const double alt =
        beat_period_s(shifted(12.0f, static_cast<float>(window_ms)), kAltRate, expected);
    std::ostringstream label;
    label << "the " << window_ms << " ms window returns one beat period in seconds";
    tally.within(alt, host, kPeriodTolerance, label.str());
    tally.within(host, expected, kPeriodTolerance, label.str() + ", at 48000 Hz");
    tally.within(alt, expected, kPeriodTolerance, label.str() + ", at 44100 Hz");
  }

  // The default is the window the shipped implementation already had, so the
  // path every GM bounce takes through this insert has not moved.
  const PitchShifterConfig fallback = shifted(7.0f, PitchShifterConfig{}.window_ms);
  PitchShifterConfig spelled_out = fallback;
  spelled_out.window_ms = kShippedWindowMs;
  const int samples = static_cast<int>(kHostRate * 0.4);
  const double default_tone_hz = probe_tone_hz(kShippedWindowMs, pitch_ratio(7.0f));
  tally.same(render(fallback, kHostRate, default_tone_hz, samples) ==
                 render(spelled_out, kHostRate, default_tone_hz, samples),
             "the default window renders bit-identically to the same value spelled out");
  for (float semitones : {-12.0f, 7.0f, 12.0f}) {
    check_law(tally, semitones, kShippedWindowMs, kHostRate, "the default window");
  }
  check_law(tally, 7.0f, kShippedWindowMs, kAltRate, "the default window");

  WARN("comparisons: " << tally.count());
  REQUIRE(tally.count() >= 44);
}
