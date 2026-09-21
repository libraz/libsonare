/// @file wind_rate_law_test.cpp
/// @brief Two sample-rate laws of the wind exciters, each rendered through
///        NativeSynth at 24 / 48 / 96 kHz on a note that settles.
///
/// The jet law: the flute's jet convection time is a fraction of the PERIOD,
/// a duration, and not a fraction of the delay line's sample count. The line
/// is the period minus a loop compensation quoted in samples (a feedback
/// register plus the reflection pole's group delay), whose duration halves
/// every time the rate doubles, so a jet delay taken from the line length was
/// shorter in seconds at 24 kHz than at 96 kHz, and the jet's pull on the
/// sounding pitch and on the upper partials moved with it.
///
/// The noise law (string_loop.h's noise_gain_at_rate): a seeded per-sample
/// noise level voiced at 48 kHz keeps its power per Hz at any rate. The floor
/// between the harmonics is read with the noise on and with it off, so a
/// specimen whose noise the measurement cannot see fails rather than passes.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/pitch.h"
#include "midi/synth/string_loop.h"
#include "midi/ump.h"
#include "support/midi_render.h"
#include "util/constants.h"

namespace {

using sonare::constants::kTwoPiD;
using sonare::midi::synth::kLossVoicedSr;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::noise_gain_at_rate;
using sonare::midi::synth::note_to_hz;
using sonare::midi::synth::SynthEngineMode;
using sonare::test::event;

constexpr double kRates[] = {24000.0, 48000.0, 96000.0};
/// The analysis span: the second second of a 1.5 s note, past the onset.
constexpr double kRenderSeconds = 1.5;
constexpr double kAnalysisFrom = 0.5;

/// Left channel of @p patch sounding @p note at @p sr.
std::vector<float> render_left(const NativeSynthPatch& patch, uint8_t note, double sr) {
  NativeSynthConfig cfg;
  cfg.patch = patch;
  NativeSynth synth(cfg);
  synth.prepare(sr, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, 100)));
  const int total = static_cast<int>(kRenderSeconds * sr);
  std::vector<float> left(static_cast<size_t>(total));
  std::vector<float> right(static_cast<size_t>(total));
  float* chans[2] = {left.data(), right.data()};
  synth.process(chans, 2, total);
  return left;
}

/// Hann-windowed Goertzel magnitude over the analysis span, normalized so a
/// full-scale sine reads 1.
double tone_mag(const std::vector<float>& x, double freq, double sr) noexcept {
  const size_t from = static_cast<size_t>(kAnalysisFrom * sr);
  const size_t count = x.size() > from ? x.size() - from : 0;
  const double w = kTwoPiD * freq / sr;
  const double coeff = 2.0 * std::cos(w);
  double s1 = 0.0, s2 = 0.0;
  for (size_t i = 0; i < count; ++i) {
    const double hann = 0.5 - 0.5 * std::cos(kTwoPiD * static_cast<double>(i) / count);
    const double s0 = hann * x[from + i] + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  const double real = s1 - s2 * std::cos(w);
  const double imag = s2 * std::sin(w);
  return std::sqrt(real * real + imag * imag) / (static_cast<double>(count) / 4.0);
}

double to_db(double mag) noexcept { return 20.0 * std::log10(std::max(1.0e-12, mag)); }

/// Mean power (dB) between the harmonics of @p f0 inside [@p lo_hz, @p hi_hz]:
/// the noise floor the harmonics leave uncovered, read at three points in
/// each gap so one periodogram bin's own scatter is averaged down.
double between_harmonic_floor_db(const std::vector<float>& x, double f0, double sr, double lo_hz,
                                 double hi_hz) {
  double acc = 0.0;
  int n = 0;
  for (int k = 1; k < 1000; ++k) {
    for (const double frac : {0.3, 0.5, 0.7}) {
      const double f = (k + frac) * f0;
      if (f < lo_hz || f > hi_hz || f >= 0.45 * sr) continue;
      const double m = tone_mag(x, f, sr);
      acc += m * m;
      ++n;
    }
    if ((k + 0.3) * f0 > hi_hz) break;
  }
  REQUIRE(n >= 18);
  return 10.0 * std::log10(std::max(1.0e-24, acc / n));
}

NativeSynthPatch wind_patch(SynthEngineMode mode) {
  NativeSynthPatch p;
  p.mode = mode;
  p.cutoff_hz = 20000.0f;
  p.amp_env.attack_ms = 5.0f;
  p.amp_env.sustain = 1.0f;
  p.amp_env.release_ms = 100.0f;
  p.drift_cents = 0.0f;
  p.stereo_spread = 0.0f;
  return p;
}

/// The concert flute's own damping and brightness on a noiseless, vibrato-free
/// jet: a lossy bore that settles into one register.
NativeSynthPatch settled_flute() {
  NativeSynthPatch p = wind_patch(SynthEngineMode::kFlute);
  p.flute.breath_pressure = 0.6f;
  p.flute.vel_to_breath = 0.0f;
  p.flute.jet_ratio = 0.5f;
  p.flute.jet_reflection = 0.5f;
  p.flute.end_reflection = 0.5f;
  p.flute.brightness = 0.55f;
  p.flute.damping = 0.30f;
  p.flute.attack_ms = 5.0f;
  p.flute.release_ms = 50.0f;
  p.flute.breath_noise = 0.0f;
  p.flute.chiff = 0.0f;
  p.flute.vibrato_depth = 0.0f;
  return p;
}

}  // namespace

TEST_CASE("noise_gain_at_rate is the identity at the voiced rate and holds power per Hz elsewhere",
          "[midi][synth][rate_law]") {
  // Bit equality, not Approx: the whole bank is voiced at 48 kHz and the
  // goldens rendered there must not move.
  CHECK(noise_gain_at_rate(kLossVoicedSr) == 1.0f);
  // Doubling the rate halves the power per Hz of a unit-variance draw, so the
  // level rises by sqrt(2) to hold it.
  CHECK(noise_gain_at_rate(2.0 * kLossVoicedSr) == static_cast<float>(std::sqrt(2.0)));
  CHECK(noise_gain_at_rate(0.5 * kLossVoicedSr) == static_cast<float>(std::sqrt(0.5)));
  CHECK(noise_gain_at_rate(0.0) == 1.0f);
}

namespace {

/// The sounding fundamental: the strongest tone within 3% of @p f0_nominal,
/// found to 0.01%. A loop's pitch is pulled by its jet, so the ladder has to
/// be read at the pitch that sounds, not at the note that was asked for.
double sounding_f0(const std::vector<float>& x, double f0_nominal, double sr) {
  double best = 0.0, at = f0_nominal;
  for (double f = 0.97 * f0_nominal; f <= 1.03 * f0_nominal; f += 1.0e-4 * f0_nominal) {
    const double m = tone_mag(x, f, sr);
    if (m > best) {
      best = m;
      at = f;
    }
  }
  return at;
}

}  // namespace

TEST_CASE(
    "the flute's jet delay is a duration, so neither its pitch nor its ladder follows the rate",
    "[midi][synth][flute][rate_law]") {
  const NativeSynthPatch patch = settled_flute();
  // Noise off so the ladder is the loop's own; damped so the note settles.
  REQUIRE(patch.flute.breath_noise == 0.0f);
  REQUIRE(patch.flute.chiff == 0.0f);
  REQUIRE(patch.flute.damping > 0.0f);

  // A jet delay taken from the line length at the running rate ran 445 us at
  // 24 kHz against 454 us at 96 kHz on note 84, and pulled the pitch +4.3,
  // +2.8 and +1.9 cents at the three rates; held as a duration it reads +2.6,
  // +2.8, +2.8. On note 72 the same defect moved h5 by 4.1 dB across the rates
  // against 0.3 dB held.
  constexpr double kMaxPitchSpreadCents = 1.0;
  constexpr double kMaxLadderSpreadDb = 1.5;
  constexpr int kTopHarmonic = 5;
  for (const uint8_t note : {uint8_t{72}, uint8_t{84}}) {
    const double f0 = static_cast<double>(note_to_hz(note));
    std::ostringstream report;
    double cents_lo = 0.0, cents_hi = 0.0;
    double ladder_lo[kTopHarmonic + 1] = {};
    double ladder_hi[kTopHarmonic + 1] = {};
    for (size_t ri = 0; ri < 3; ++ri) {
      const double sr = kRates[ri];
      const std::vector<float> x = render_left(patch, note, sr);
      const double f0s = sounding_f0(x, f0, sr);
      const double cents = 1200.0 * std::log2(f0s / f0);
      const double h1 = tone_mag(x, f0s, sr);
      report << "note " << int{note} << " sr " << sr << " f0 " << f0s << " (" << cents
             << " cents) h1 " << h1;
      // The note has to be sounding for the ladder to mean anything.
      REQUIRE(h1 > 1.0e-3);
      if (ri == 0) cents_lo = cents_hi = cents;
      cents_lo = std::min(cents_lo, cents);
      cents_hi = std::max(cents_hi, cents);
      for (int h = 2; h <= kTopHarmonic; ++h) {
        const double rel = to_db(tone_mag(x, h * f0s, sr)) - to_db(h1);
        report << " h" << h << " " << rel;
        if (ri == 0) ladder_lo[h] = ladder_hi[h] = rel;
        ladder_lo[h] = std::min(ladder_lo[h], rel);
        ladder_hi[h] = std::max(ladder_hi[h], rel);
      }
      report << "\n";
    }
    INFO(report.str() << "note " << int{note} << " pitch spread " << (cents_hi - cents_lo)
                      << " cents");
    CHECK(cents_hi - cents_lo < kMaxPitchSpreadCents);
    for (int h = 2; h <= kTopHarmonic; ++h) {
      INFO(report.str() << "note " << int{note} << " h" << h << " spread "
                        << (ladder_hi[h] - ladder_lo[h]) << " dB");
      CHECK(ladder_hi[h] - ladder_lo[h] < kMaxLadderSpreadDb);
    }
  }
}

namespace {

struct NoiseSpecimen {
  const char* name;
  NativeSynthPatch on;
  NativeSynthPatch off;
  uint8_t note;
};

std::vector<NoiseSpecimen> noise_specimens() {
  std::vector<NoiseSpecimen> out;
  {
    NativeSynthPatch p = settled_flute();
    NoiseSpecimen s{"flute", p, p, 72};
    s.on.flute.breath_noise = 1.0f;
    s.off.flute.breath_noise = 0.0f;
    out.push_back(s);
  }
  {
    NativeSynthPatch p = wind_patch(SynthEngineMode::kReed);
    p.reed.vel_to_breath = 0.0f;
    p.reed.chiff = 0.0f;
    NoiseSpecimen s{"reed", p, p, 60};
    s.on.reed.breath_noise = 1.0f;
    s.off.reed.breath_noise = 0.0f;
    out.push_back(s);
  }
  {
    NativeSynthPatch p = wind_patch(SynthEngineMode::kBrass);
    p.brass.vel_to_breath = 0.0f;
    p.brass.chiff = 0.0f;
    NoiseSpecimen s{"brass", p, p, 60};
    s.on.brass.breath_noise = 1.0f;
    s.off.brass.breath_noise = 0.0f;
    out.push_back(s);
  }
  {
    NativeSynthPatch p = wind_patch(SynthEngineMode::kFreeReed);
    NoiseSpecimen s{"free_reed", p, p, 60};
    s.on.free_reed.breath_noise = 1.0f;
    s.off.free_reed.breath_noise = 0.0f;
    out.push_back(s);
  }
  {
    NativeSynthPatch p = wind_patch(SynthEngineMode::kVocal);
    NoiseSpecimen s{"vocal", p, p, 60};
    s.on.vocal.breath_noise = 1.0f;
    s.off.vocal.breath_noise = 0.0f;
    out.push_back(s);
  }
  return out;
}

}  // namespace

TEST_CASE("a seeded breath-noise level voiced at 48 kHz keeps its power per Hz at any rate",
          "[midi][synth][rate_law]") {
  // The floor between the harmonics, noise on against noise off. The off
  // reading is the control: the on reading has to clear it by this much at
  // every rate, or the specimen's noise is not what is being measured. At
  // 5 dB the control still sits inside the on reading by at most 1.2 dB; the
  // free reed's own slot flow puts its 24 kHz control there.
  constexpr double kMinReachDb = 5.0;
  // One draw per sample at a fixed level loses 3 dB of power per Hz per
  // doubling of the rate: unheld, brass read 10.2 dB of spread and reed 6.2,
  // both falling with the rate. Held, what remains is each loop's own rate
  // residual, up to 3.7 dB on brass and in neither direction consistently --
  // a shape no 1/sr term produces. The flute's unheld 2.6 dB sits inside this
  // bound, so its row is carried by the class rather than separating on its
  // own.
  constexpr double kMaxFloorSpreadDb = 4.5;
  constexpr double kBandLoHz = 1500.0;
  constexpr double kBandHiHz = 6000.0;

  for (const NoiseSpecimen& s : noise_specimens()) {
    const double f0 = static_cast<double>(note_to_hz(s.note));
    std::ostringstream report;
    double lo = 0.0, hi = 0.0;
    for (size_t ri = 0; ri < 3; ++ri) {
      const double sr = kRates[ri];
      const double on =
          between_harmonic_floor_db(render_left(s.on, s.note, sr), f0, sr, kBandLoHz, kBandHiHz);
      const double off =
          between_harmonic_floor_db(render_left(s.off, s.note, sr), f0, sr, kBandLoHz, kBandHiHz);
      report << s.name << " sr " << sr << " floor on " << on << " dB off " << off << " dB\n";
      INFO(report.str());
      REQUIRE(on - off >= kMinReachDb);
      if (ri == 0) lo = hi = on;
      lo = std::min(lo, on);
      hi = std::max(hi, on);
    }
    INFO(report.str() << s.name << " floor spread " << (hi - lo) << " dB");
    CHECK(hi - lo < kMaxFloorSpreadDb);
  }
}
