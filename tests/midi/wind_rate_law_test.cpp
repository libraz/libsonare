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
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <functional>
#include <sstream>
#include <utility>
#include <vector>

#include "midi/control_value.h"
#include "midi/midi_event.h"
#include "midi/synth/bowed_string_voice.h"
#include "midi/synth/brass_voice.h"
#include "midi/synth/flute_voice.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/harpsichord_voice.h"
#include "midi/synth/ks_voice.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/piano_voice.h"
#include "midi/synth/pipe_organ_voice.h"
#include "midi/synth/pitch.h"
#include "midi/synth/plucked_string_voice.h"
#include "midi/synth/reed_voice.h"
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
using sonare::midi::synth::onepole_noise_rate_gain;
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

TEST_CASE(
    "onepole_noise_rate_gain is the identity at the voiced rate and diverges from "
    "noise_gain_at_rate where the corner is a sizeable fraction of Nyquist",
    "[midi][synth][rate_law]") {
  // Bit equality at the voiced rate, for every shipped corner: KS keyoff,
  // harpsichord chiff and jack noise.
  for (const float fc : {5200.0f, 2200.0f, 1500.0f}) {
    CHECK(onepole_noise_rate_gain(fc, kLossVoicedSr) == 1.0f);
  }
  CHECK(onepole_noise_rate_gain(2200.0f, 0.0) == 1.0f);
  CHECK(onepole_noise_rate_gain(0.0f, 48000.0) == 1.0f);
  CHECK(onepole_noise_rate_gain(-5.0f, 48000.0) == 1.0f);

  // Output variance falls as the rate rises, so the compensation rises with it.
  CHECK(onepole_noise_rate_gain(2200.0f, 2.0 * kLossVoicedSr) > 1.0f);
  CHECK(onepole_noise_rate_gain(2200.0f, 0.5 * kLossVoicedSr) < 1.0f);

  // 5200 Hz is 43% of Nyquist at 24 kHz and the two forms part company there
  // (0.037); 200 Hz is far below it and they agree (6e-5). Both sides, so the
  // divergence is the corner's doing rather than a blanket disagreement.
  const float approx_24k = noise_gain_at_rate(24000.0);
  CHECK(std::abs(onepole_noise_rate_gain(5200.0f, 24000.0) - approx_24k) > 0.02f);
  CHECK(std::abs(onepole_noise_rate_gain(200.0f, 24000.0) - approx_24k) < 0.0005f);
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

TEST_CASE("a live brightness move keeps each wind voice on the pitch a fresh note would sound",
          "[midi][synth][rate_law]") {
  // CC74 moves the bell pole under a sounding note; the loop's tuning
  // compensation has to follow it, or the note drifts off the pitch a note
  // voiced at that brightness from the start sounds.
  constexpr double kSr = 48000.0;
  constexpr double kMaxCents = 5.0;
  const auto brightness_of = [](NativeSynthPatch& p) -> float& {
    switch (p.mode) {
      case SynthEngineMode::kFlute:
        return p.flute.brightness;
      case SynthEngineMode::kReed:
        return p.reed.brightness;
      default:
        return p.brass.brightness;
    }
  };
  const std::pair<const char*, std::pair<uint8_t, uint8_t>> voices[] = {
      {"flute", {73, 84}}, {"clarinet", {71, 72}}, {"alto sax", {65, 72}}, {"trumpet", {56, 72}}};
  // The returned tail is 1.5 s; tone_mag reads 0.5-1.5 s of it.
  const auto render = [&](NativeSynthPatch patch, uint8_t note, int cc74) {
    NativeSynthConfig cfg;
    cfg.patch = patch;
    NativeSynth synth(cfg);
    synth.prepare(kSr, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, 100)));
    std::vector<float> l(static_cast<size_t>(0.5 * kSr)), r(l.size());
    float* head[2] = {l.data(), r.data()};
    synth.process(head, 2, static_cast<int>(l.size()));
    if (cc74 >= 0) {
      synth.on_event(
          0, event(sonare::midi::make_midi1_control_change(0, 0, 74, static_cast<uint8_t>(cc74))));
    }
    std::vector<float> tail_l(static_cast<size_t>(kRenderSeconds * kSr)), tail_r(tail_l.size());
    float* tail[2] = {tail_l.data(), tail_r.data()};
    synth.process(tail, 2, static_cast<int>(tail_l.size()));
    return tail_l;
  };
  std::ostringstream report;
  for (const auto& [name, pn] : voices) {
    const auto [program, note] = pn;
    const NativeSynthPatch base = sonare::midi::synth::gm_fallback_patch(0, program);
    const double f0 = static_cast<double>(note_to_hz(note));
    for (const int cc74 : {0, 127}) {
      NativeSynthPatch voiced = base;
      brightness_of(voiced) = static_cast<float>(cc74) / 127.0f;
      const double fresh = sounding_f0(render(voiced, note, -1), f0, kSr);
      const double moved = sounding_f0(render(base, note, cc74), f0, kSr);
      const double cents = 1200.0 * std::log2(moved / fresh);
      report << name << " CC74=" << cc74 << " fresh " << fresh << " Hz, moved " << moved << " Hz, "
             << cents << " cents\n";
      INFO(report.str());
      CHECK(std::fabs(cents) < kMaxCents);
    }
  }
}

namespace {

using sonare::midi::synth::loop_budget;
using sonare::midi::synth::repay_interpolation;

/// Fundamental amplitude of @p x over [from_s, from_s + 0.25) at @p freq.
double fundamental_in_window(const std::vector<float>& x, double freq, double sr, double from_s) {
  const size_t from = static_cast<size_t>(from_s * sr);
  const size_t count = static_cast<size_t>(0.25 * sr);
  const double w = kTwoPiD * freq / sr;
  double re = 0.0;
  double im = 0.0;
  for (size_t i = 0; i < count && from + i < x.size(); ++i) {
    const double window = 0.5 - 0.5 * std::cos(kTwoPiD * static_cast<double>(i) / count);
    re += window * x[from + i] * std::cos(w * static_cast<double>(i));
    im += window * x[from + i] * std::sin(w * static_cast<double>(i));
  }
  return std::sqrt(re * re + im * im);
}

/// Fundamental T60 read from two windows of a string note, in seconds.
double fundamental_t60(const NativeSynthPatch& patch, uint8_t note, double sr) {
  NativeSynthConfig cfg;
  cfg.patch = patch;
  cfg.dc_block = false;
  NativeSynth synth(cfg);
  synth.prepare(sr, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, 100)));
  const int total = static_cast<int>(1.6 * sr);
  std::vector<float> left(static_cast<size_t>(total));
  std::vector<float> right(static_cast<size_t>(total));
  float* chans[2] = {left.data(), right.data()};
  synth.process(chans, 2, total);
  const double f0 = note_to_hz(note);
  const double early = fundamental_in_window(left, f0, sr, 0.1);
  const double late = fundamental_in_window(left, f0, sr, 1.2);
  if (!(early > 0.0) || !(late > 0.0)) return 0.0;
  const double db_per_s = 20.0 * std::log10(early / late) / 1.1;
  return 60.0 / db_per_s;
}

}  // namespace

TEST_CASE("loop_budget reports its floor and repays the read only off the voiced rate",
          "[midi][synth][wind]") {
  const auto roomy = loop_budget(100.0f, 2.0f, 1.0f);
  CHECK_FALSE(roomy.floored);
  CHECK(roomy.delay == 98.0f);
  CHECK(roomy.achieved_period == 100.0f);

  // A period shorter than the floor plus the carried delay sounds at the floor, and says so.
  const auto pinned = loop_budget(3.0f, 2.5f, 1.0f);
  CHECK(pinned.floored);
  CHECK(pinned.delay == 1.0f);
  CHECK(pinned.achieved_period == 3.5f);

  // Relative to the voiced rate the repayment is exactly nothing there and positive below it.
  CHECK(loop_budget(20.4f, 2.0f, 1.0f, kLossVoicedSr).interp_gain == 1.0f);
  const float low_rate = loop_budget(3.4f, 1.5f, 1.0f, 8000.0).interp_gain;
  CHECK(low_rate > 1.05f);
  CHECK(loop_budget(3.4f, 1.5f, 1.0f).interp_gain >= low_rate);

  // The boost never lifts a loop past what its sub-fundamental ring bound allows.
  CHECK(repay_interpolation(0.99f, 4.0f) <= 0.9999f);
}

TEST_CASE("loop_budget asks for an oversampling factor when the period is floored or short",
          "[midi][synth][wind]") {
  using sonare::midi::synth::kMaxLoopOversample;
  using sonare::midi::synth::kMinHeldPeriod;
  using sonare::midi::synth::settle_loop_oversample;

  CHECK(loop_budget(100.0f, 2.0f, 1.0f).oversample == 1);
  // Above the held floor, clearing the read's floor exactly, the loop runs as it is.
  CHECK(loop_budget(kMinHeldPeriod + 0.5f, kMinHeldPeriod - 0.5f, 1.0f).oversample == 1);
  // At or under the held floor the loop is lifted until its period clears it, at the voiced
  // rate as anywhere else: 3 samples take 3x, the floor itself 1x.
  CHECK(loop_budget(3.0f, 2.0f, 1.0f).oversample == 3);
  CHECK(loop_budget(kMinHeldPeriod, 2.0f, 1.0f).oversample == 1);
  CHECK(loop_budget(6.0f, 1.0f, 1.0f, kLossVoicedSr).oversample == 2);

  // 1.9 samples against a register and a one-sample read: the read's floor alone would take 2x
  // (the register does not stretch with the factor), the held floor takes 5x.
  const auto floored = loop_budget(1.9f, 1.0f, 1.0f);
  CHECK(floored.floored);
  CHECK(floored.oversample == 5);
  // The held rate is the note's alone. At 8 kHz the note's period is 11.4 samples at the voiced
  // rate, so reaching that rate (6x) is all it takes; at 44.1 kHz the same 1.9 samples are 2.07
  // there, lifted 4x, which the host reaches at 5x; at 96 kHz they are 0.95, lifted the most a
  // loop is asked for (kMaxLoopOversample), which the host reaches at 4x.
  CHECK(loop_budget(1.9f, 1.0f, 1.0f, 8000.0).oversample == 6);
  CHECK(loop_budget(1.9f, 1.0f, 1.0f, 44100.0).oversample == 5);
  CHECK(loop_budget(1.9f, 1.0f, 1.0f, 96000.0).oversample == 4);
  // A 3-sample period at 24 kHz is 6 at the voiced rate and lifted 2x there: the 96 kHz loop a
  // 48 kHz host renders for the same note, so the two rates draw the same thing.
  CHECK(loop_budget(3.0f, 1.0f, 1.0f, 24000.0).oversample == 4);
  CHECK(loop_budget(100.0f, 2.0f, 1.0f, 8000.0).oversample == 1);
  // A period no factor can rescue is bounded rather than unbounded.
  CHECK(loop_budget(0.1f, 1.0f, 1.0f).oversample == kMaxLoopOversample);

  // The search confirms each candidate against the voice itself and keeps the smallest that
  // clears: a compensation that grows faster than the estimate takes one more step.
  const auto grown = [](int f) {
    return loop_budget(7.0f * static_cast<float>(f),
                       f == 1 ? 6.7f : 1.0f + 6.05f * static_cast<float>(f), 1.0f);
  };
  int built_for = 0;
  const int factor = settle_loop_oversample([&](int f) {
    built_for = f;
    return grown(f);
  });
  CHECK(factor == built_for);
  CHECK(factor > grown(1).oversample);
  CHECK_FALSE(grown(factor).floored);
  CHECK(grown(factor - 1).floored);
  CHECK(settle_loop_oversample([](int) { return loop_budget(50.0f, 2.0f, 1.0f); }) == 1);
}

TEST_CASE("the loop decimator is transparent at factor 1 and a host-Nyquist lowpass above it",
          "[midi][synth][wind]") {
  using sonare::midi::synth::LoopDecimator;

  LoopDecimator unit;
  unit.configure(1);
  float next = 0.25f;
  CHECK(unit.run([&] { return next; }) == 0.25f);

  for (const int factor : {2, 3, 5, 8}) {
    LoopDecimator dec;
    dec.configure(factor);
    // Internal sine at fraction `rel` of the HOST Nyquist, run long enough to fill the filter.
    const auto gain_at = [&](double rel) {
      dec.configure(factor);
      const double w = kTwoPiD * 0.5 * rel / factor;
      double n = 0.0, peak = 0.0;
      for (int host = 0; host < 400; ++host) {
        const float y = dec.run([&] { return static_cast<float>(std::sin(w * n++)); });
        if (host > 300) peak = std::max(peak, static_cast<double>(std::fabs(y)));
      }
      return peak;
    };
    CAPTURE(factor);
    CHECK(gain_at(0.05) == Catch::Approx(1.0).margin(0.01));
    CHECK(gain_at(0.7) == Catch::Approx(1.0).margin(0.03));
    // Everything the internal rate carries above the host Nyquist is gone before the decimation.
    CHECK(gain_at(1.4) < 0.1);
  }
}

TEST_CASE("a Karplus-Strong fundamental keeps its requested t60 across rates",
          "[midi][synth][ks][wind]") {
  NativeSynthPatch patch;
  patch.mode = SynthEngineMode::kKarplusStrong;
  patch.ks.decay_s = 1.2f;
  patch.ks.decay_stretch = 0.0f;
  patch.ks.brightness = 1.0f;
  for (const uint8_t note : {72, 84, 96}) {
    for (const double sr : {8000.0, 24000.0, 48000.0, 96000.0}) {
      const double t60 = fundamental_t60(patch, note, sr);
      CAPTURE(static_cast<int>(note), sr, t60);
      CHECK(t60 > 1.2 * 0.9);
      CHECK(t60 < 1.2 * 1.1);
    }
  }
}

TEST_CASE("a default conical reed keeps speaking at a low rate", "[midi][synth][wind]") {
  NativeSynthPatch patch;
  patch.mode = SynthEngineMode::kReed;
  patch.reed.conical = true;
  patch.reed.breath_noise = 0.0f;
  patch.reed.chiff = 0.0f;
  for (const uint8_t note : {97, 98, 99}) {
    NativeSynthConfig cfg;
    cfg.patch = patch;
    cfg.dc_block = false;
    NativeSynth synth(cfg);
    synth.prepare(8000.0, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, 100)));
    const int total = 8 * 8000;
    std::vector<float> left(static_cast<size_t>(total));
    std::vector<float> right(static_cast<size_t>(total));
    float* chans[2] = {left.data(), right.data()};
    synth.process(chans, 2, total);
    const double sounding = fundamental_in_window(left, note_to_hz(note), 8000.0, 7.0);
    CAPTURE(static_cast<int>(note), sounding);
    CHECK(sounding > 1.0e-3);
  }
}

namespace {

double window_rms(const std::vector<float>& x, double from_s, double to_s, double sr) {
  const size_t from = static_cast<size_t>(from_s * sr);
  const size_t to = std::min(x.size(), static_cast<size_t>(to_s * sr));
  double acc = 0.0;
  for (size_t i = from; i < to; ++i) acc += static_cast<double>(x[i]) * x[i];
  return to > from ? std::sqrt(acc / static_cast<double>(to - from)) : 0.0;
}

/// Held RMS before the note-off and RMS three seconds after it, with an outer amplitude release
/// long enough that the VCA cannot be what silences the voice.
std::pair<double, double> held_and_released_rms(NativeSynthPatch patch, uint8_t note, double sr) {
  patch.amp_env.sustain = 1.0f;
  patch.amp_env.release_ms = 5000.0f;
  NativeSynthConfig cfg;
  cfg.patch = patch;
  cfg.dc_block = false;
  NativeSynth synth(cfg);
  synth.prepare(sr, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, 100)));
  const int off_at = static_cast<int>(1.0 * sr);
  const int total = static_cast<int>(5.0 * sr);
  std::vector<float> left(static_cast<size_t>(total));
  std::vector<float> right(static_cast<size_t>(total));
  int done = 0;
  bool sent = false;
  while (done < total) {
    if (!sent && done >= off_at) {
      synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, note, 0)));
      sent = true;
    }
    const int block = std::min(256, total - done);
    float* chans[2] = {left.data() + done, right.data() + done};
    synth.process(chans, 2, block);
    done += block;
  }
  return {window_rms(left, 0.5, 1.0, sr), window_rms(left, 4.0, 4.5, sr)};
}

}  // namespace

TEST_CASE("a wind bore rings down after the breath is released, whatever feeds it",
          "[midi][synth][wind]") {
  struct Specimen {
    const char* label;
    NativeSynthPatch patch;
    uint8_t note;
  };
  std::vector<Specimen> specimens;
  {
    NativeSynthPatch p;
    p.mode = SynthEngineMode::kFlute;
    p.flute.release_ms = 80.0f;
    specimens.push_back({"flute", p, 72});
  }
  for (const uint8_t note : {48, 60, 72}) {
    NativeSynthPatch p;
    p.mode = SynthEngineMode::kPipeOrgan;
    p.pipe_organ.release_damp_s = 1.0f;
    specimens.push_back({"pipe organ", p, note});
  }
  {
    NativeSynthPatch p;
    p.mode = SynthEngineMode::kPipeOrgan;
    specimens.push_back({"pipe organ", p, 60});
  }
  {
    NativeSynthPatch p;
    p.mode = SynthEngineMode::kBrass;
    p.brass.lip_aperture = 0.9f;
    p.brass.release_ms = 20.0f;
    specimens.push_back({"brass lip valve", p, 60});
  }
  {
    NativeSynthPatch p;
    p.mode = SynthEngineMode::kReed;
    p.reed.closing_pressure = 1.88816f;
    p.reed.release_ms = 20.0f;
    specimens.push_back({"beating reed", p, 60});
  }
  for (const Specimen& specimen : specimens) {
    for (const double sr : {44100.0, 48000.0, 96000.0}) {
      const auto [held, late] = held_and_released_rms(specimen.patch, specimen.note, sr);
      CAPTURE(specimen.label, sr, held, late);
      REQUIRE(held > 1.0e-3);
      CHECK(late < held * 1.0e-3);
    }
  }
}

TEST_CASE("a resonator keeps the size of its centre response at every sample rate",
          "[midi][synth][wind]") {
  using sonare::midi::synth::resonator_gain_at_rate;
  for (const double freq : {196.0, 2500.0}) {
    auto centre = [&](double sr) {
      const double r = std::exp(-6.907755279 / (sr * 0.8));
      const double w = kTwoPiD * freq / sr;
      const float gain = resonator_gain_at_rate(static_cast<float>(r), static_cast<float>(w), sr);
      const double a1 = 2.0 * r * std::cos(w);
      const double a2 = -r * r;
      const double re = 1.0 - a1 * std::cos(w) - a2 * std::cos(2.0 * w);
      const double im = a1 * std::sin(w) + a2 * std::sin(2.0 * w);
      return static_cast<double>(gain) / std::sqrt(re * re + im * im);
    };
    const double voiced = centre(kLossVoicedSr);
    for (const double sr : {24000.0, 44100.0, 96000.0}) {
      CAPTURE(freq, sr);
      CHECK(centre(sr) == Catch::Approx(voiced).epsilon(0.01));
    }
    const double r48 = std::exp(-6.907755279 / (kLossVoicedSr * 0.8));
    CHECK(resonator_gain_at_rate(static_cast<float>(r48), 1.0f, kLossVoicedSr) ==
          static_cast<float>(1.0f - static_cast<float>(r48)));
  }
}

namespace {

using sonare::midi::Velocity16;

/// The strongest tone of the last half of @p x within 6% of @p f0, found to 0.05%, as cents from
/// @p f0; its magnitude (a full-scale sine reads 1) through @p magnitude when asked for. The Hann
/// window is built once, so a scan of a few hundred bins stays cheap.
double strongest_tone_cents(const std::vector<float>& x, double f0, double sr,
                            double* magnitude = nullptr) {
  const size_t from = x.size() / 2;
  const size_t count = x.size() - from;
  std::vector<double> hann(count);
  for (size_t i = 0; i < count; ++i) {
    hann[i] = 0.5 - 0.5 * std::cos(kTwoPiD * static_cast<double>(i) / static_cast<double>(count));
  }
  double best = -1.0, at = f0;
  for (double rel = 0.94; rel <= 1.06; rel += 5.0e-4) {
    const double w = kTwoPiD * f0 * rel / sr;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (size_t i = 0; i < count; ++i) {
      const double s0 = hann[i] * x[from + i] + coeff * s1 - s2;
      s2 = s1;
      s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    if (power > best) {
      best = power;
      at = f0 * rel;
    }
  }
  if (magnitude != nullptr) {
    *magnitude = std::sqrt(std::max(0.0, best)) / (static_cast<double>(count) / 4.0);
  }
  return 1200.0 * std::log2(at / f0);
}

bool finite_and_sounding(const std::vector<float>& x) {
  double energy = 0.0;
  for (const float v : x) {
    if (!std::isfinite(v)) return false;
    energy += static_cast<double>(v) * v;
  }
  return energy / static_cast<double>(x.size()) > 1.0e-8;
}

/// Highest note whose fundamental stays a semitone under the Nyquist of @p sr.
int highest_in_band_note(double sr) {
  int note = 60;
  while (note < 126 && note_to_hz(static_cast<uint8_t>(note + 1)) <= 0.5 * sr / 1.0594631) ++note;
  return note;
}

/// At 8, 16 and 24 kHz a short or floored loop runs at the very rate the 48 kHz host lifts the
/// same note to, so those renders are the reference's own; 44.1 kHz reaches 88.2 kHz and above
/// instead, and its tolerance carries the difference between the two discretisations.
constexpr double kLowRates[] = {8000.0, 16000.0, 24000.0, 44100.0};
constexpr double kReferenceRate = 48000.0;
constexpr double kCoreSeconds = 1.6;
/// A second, longer render of the 48 kHz reference: a note whose reference pitch moves by more
/// than this between the two is in an unstable region and has no pitch to compare against.
constexpr double kLongSeconds = 2.4;
constexpr double kReferenceDriftCents = 3.0;
/// Fundamental magnitude (a full-scale sine reads 1) under which a render is not sounding the
/// note, whatever else it emits: a marginal oscillation under 1% of full scale reads no tuning.
constexpr double kSoundingH1 = 1.0e-2;
/// Fundamental magnitude under which a loop's last half is the numerical floor rather than its
/// own ringing: a plucked string decayed to 1e-4 still reads its period to the scan step, a
/// Karplus-Strong loop at 1e-10 reads anything.
constexpr double kLoopFloorH1 = 1.0e-5;
/// A render whose fundamental has fallen to this fraction of the reference's is a marginal
/// oscillation at that rate, whose pitch is not the loop's: the flue pipe's note 126 at 44.1 kHz
/// sits at 0.009 against 0.19 and 16 cents off.
constexpr double kMarginalH1Ratio = 0.1;
/// Smallest rise of the sounding pitch from one semitone to the next (a semitone is 100).
constexpr double kMinStepCents = 20.0;

/// What a core rendered and the oversampling factor it settled on.
struct Rendered {
  std::vector<float> x;
  int factor = 1;
};

/// Every note whose loop is floored at the tested rate (the voice settles on a factor above 1)
/// sounds where the same voice sounds it at 48 kHz, which is where the bank is voiced, and a
/// higher note sounds a higher pitch than the one below it. Both fail when the loop is pinned at
/// its floor, which sounds several notes at one pitch.
///
/// The reference is the voice at 48 kHz and not the nominal pitch because the voicing itself
/// tunes tens of cents off at the top of the compass at every rate. A note whose fundamental is
/// at the numerical floor (kLoopFloorH1) at either rate or marginal at the tested one
/// (kMarginalH1Ratio), or whose 48 kHz pitch differs by more than kReferenceDriftCents between
/// a 1.6 s and a 2.4 s render, is skipped: there is no pitch to agree with.
///
/// @p tolerance_cents holds one bound per entry of kLowRates: the largest deviation measured
/// from the 48 kHz pitch plus the estimator's resolution, 0.87 cents of scan step plus one
/// Hann bin of the 0.8 s analysis window (2164 / f0 cents) at the lowest note covered. A floored
/// or short loop runs at the held rate of its note, so at 8, 16 and 24 kHz it is the 48 kHz
/// render and the measured deviation is 0; at 44.1 kHz it is the same loop at 88.2 kHz or above.
/// A rate at which no note of the voice reaches a comparison carries the resolution alone.
/// @p render is (note, rate, seconds) -> Rendered; zero seconds only reports the factor.
template <class Render>
void require_floored_notes_hold_pitch(
    const std::array<double, std::size(kLowRates)>& tolerance_cents, int highest_note,
    Render render) {
  int tested_in_total = 0;
  for (size_t rate_index = 0; rate_index < std::size(kLowRates); ++rate_index) {
    const double sr = kLowRates[rate_index];
    double previous = -1.0;
    for (int note = 36; note <= std::min(highest_note, highest_in_band_note(sr)); ++note) {
      const uint8_t n = static_cast<uint8_t>(note);
      if (render(n, sr, 0.0).factor == 1) continue;
      const double f0 = note_to_hz(n);
      double reference_h1 = 0.0;
      const double reference = strongest_tone_cents(render(n, kReferenceRate, kCoreSeconds).x, f0,
                                                    kReferenceRate, &reference_h1);
      if (reference_h1 <= kLoopFloorH1) continue;
      const double drift = std::abs(
          strongest_tone_cents(render(n, kReferenceRate, kLongSeconds).x, f0, kReferenceRate) -
          reference);
      if (drift > kReferenceDriftCents) continue;
      const Rendered r = render(n, sr, kCoreSeconds);
      double h1 = 0.0;
      const double cents = strongest_tone_cents(r.x, f0, sr, &h1);
      CAPTURE(sr, note, r.factor, f0, cents, reference, h1);
      REQUIRE(finite_and_sounding(r.x));
      if (h1 <= kLoopFloorH1 || h1 < kMarginalH1Ratio * reference_h1) continue;
      CHECK(std::abs(cents - reference) <= tolerance_cents[rate_index]);
      const double sounding = f0 * std::exp2(cents / 1200.0);
      CHECK(sounding > previous * std::exp2(kMinStepCents / 1200.0));
      previous = sounding;
      ++tested_in_total;
    }
  }
  CHECK(tested_in_total > 0);
}

}  // namespace

TEST_CASE("a cylindrical reed sounds each note whose loop is floored at a low rate",
          "[midi][synth][wind]") {
  sonare::midi::synth::ReedPatchParams params;
  params.conical = false;
  params.breath_noise = 0.0f;
  params.chiff = 0.0f;
  const auto render = [&](uint8_t note, double sr, double seconds) {
    std::vector<float> slab(static_cast<size_t>(sonare::midi::synth::reed_slab_capacity(sr)));
    sonare::midi::synth::ReedVoiceCore core;
    core.attach(slab.data(), sonare::midi::synth::reed_buffer_capacity(sr));
    core.start(params, sr, note, Velocity16::from7(100), 0x5eedu);
    Rendered out;
    out.factor = core.oversample();
    out.x.resize(static_cast<size_t>(seconds * sr));
    for (float& v : out.x) v = core.render(1.0f);
    return out;
  };
  require_floored_notes_hold_pitch({5.1, 3.0, 2.3, 1.8}, 127, render);
}

TEST_CASE("a bowed string sounds each note whose lines are floored at a low rate",
          "[midi][synth][wind]") {
  sonare::midi::synth::BowedStringPatchParams params;
  params.rosin = 0.0f;
  const auto render = [&](uint8_t note, double sr, double seconds) {
    std::vector<float> slab(
        static_cast<size_t>(sonare::midi::synth::bowed_string_slab_capacity(sr)));
    sonare::midi::synth::BowedStringVoiceCore core;
    core.attach(slab.data(), sonare::midi::synth::bowed_string_buffer_capacity(sr));
    core.start(params, sr, note, Velocity16::from7(100), 0x5eedu);
    Rendered out;
    out.factor = core.oversample();
    out.x.resize(static_cast<size_t>(seconds * sr));
    for (float& v : out.x) v = core.render(1.0f);
    return out;
  };
  // Above note 106 the 48 kHz reference is not a bowed pitch.
  require_floored_notes_hold_pitch({3.5, 2.4, 1.8, 0.9}, 106, render);
}

TEST_CASE("a flute sounds each note whose bore or jet is floored at a low rate",
          "[midi][synth][wind]") {
  sonare::midi::synth::FlutePatchParams params;
  params.breath_noise = 0.0f;
  params.chiff = 0.0f;
  params.vibrato_depth = 0.0f;
  const auto render = [&](uint8_t note, double sr, double seconds) {
    std::vector<float> slab(static_cast<size_t>(sonare::midi::synth::flute_slab_capacity(sr)));
    sonare::midi::synth::FluteVoiceCore core;
    core.attach(slab.data(), sonare::midi::synth::flute_buffer_capacity(sr));
    core.start(params, sr, note, Velocity16::from7(100), 0x5eedu);
    Rendered out;
    out.factor = core.oversample();
    out.x.resize(static_cast<size_t>(seconds * sr));
    for (float& v : out.x) v = core.render(1.0f);
    return out;
  };
  require_floored_notes_hold_pitch({3.0, 2.0, 1.6, 1.8}, 127, render);
}

TEST_CASE("a flue pipe sounds each note whose bore or jet is floored at a low rate",
          "[midi][synth][wind]") {
  sonare::midi::synth::PipeOrganPatchParams params;
  const auto render = [&](uint8_t note, double sr, double seconds) {
    std::vector<float> slab(static_cast<size_t>(sonare::midi::synth::pipe_organ_slab_capacity(sr)));
    sonare::midi::synth::PipeOrganVoiceCore core;
    core.attach(slab.data(), sonare::midi::synth::pipe_organ_buffer_capacity(sr));
    core.start(params, sr, note, Velocity16::from7(100), 0x5eedu);
    Rendered out;
    out.factor = core.oversample();
    out.x.resize(static_cast<size_t>(seconds * sr));
    for (float& v : out.x) v = core.render(1.0f);
    return out;
  };
  require_floored_notes_hold_pitch({3.0, 2.0, 1.6, 2.6}, 127, render);
}

TEST_CASE("a Karplus-Strong string sounds each note whose loop is short at a low rate",
          "[midi][synth][wind]") {
  sonare::midi::synth::KsPatchParams params;
  const auto render = [&](uint8_t note, double sr, double seconds) {
    std::vector<float> slab(static_cast<size_t>(sonare::midi::synth::ks_slab_capacity(sr)));
    sonare::midi::synth::KsVoiceCore core;
    core.attach(slab.data(), sonare::midi::synth::ks_buffer_capacity(sr));
    core.start(params, sr, note, Velocity16::from7(100), 0x5eedu);
    Rendered out;
    out.factor = core.oversample();
    out.x.resize(static_cast<size_t>(seconds * sr));
    for (float& v : out.x) v = core.render(1.0f);
    return out;
  };
  require_floored_notes_hold_pitch({3.0, 2.0, 3.3, 0.9}, 127, render);
}

TEST_CASE("a plucked string sounds each note whose loop is short at a low rate",
          "[midi][synth][wind]") {
  sonare::midi::synth::PluckedStringPatchParams params;
  const auto render = [&](uint8_t note, double sr, double seconds) {
    std::vector<float> slab(
        static_cast<size_t>(sonare::midi::synth::plucked_string_slab_capacity(sr)));
    sonare::midi::synth::PluckedStringVoiceCore core;
    core.attach(slab.data(), sonare::midi::synth::plucked_string_buffer_capacity(sr));
    core.start(params, sr, note, Velocity16::from7(100), 0x5eedu);
    Rendered out;
    out.factor = core.oversample();
    out.x.resize(static_cast<size_t>(seconds * sr));
    for (float& v : out.x) v = core.render(1.0f);
    return out;
  };
  require_floored_notes_hold_pitch({3.0, 2.0, 1.6, 0.9}, 127, render);
}

TEST_CASE("a brass bore sounds each note whose loop is short at a low rate",
          "[midi][synth][wind]") {
  sonare::midi::synth::BrassPatchParams params;
  params.breath_noise = 0.0f;
  params.chiff = 0.0f;
  const auto render = [&](uint8_t note, double sr, double seconds) {
    std::vector<float> slab(static_cast<size_t>(sonare::midi::synth::brass_slab_capacity(sr)));
    sonare::midi::synth::BrassVoiceCore core;
    core.attach(slab.data(), sonare::midi::synth::brass_buffer_capacity(sr));
    core.start(params, sr, note, Velocity16::from7(100), 0x5eedu);
    Rendered out;
    out.factor = core.oversample();
    out.x.resize(static_cast<size_t>(seconds * sr));
    for (float& v : out.x) v = core.render(1.0f);
    return out;
  };
  require_floored_notes_hold_pitch({3.0, 2.0, 1.6, 0.9}, 127, render);
}

TEST_CASE("a harpsichord sounds each note whose strings are short at a low rate",
          "[midi][synth][wind]") {
  sonare::midi::synth::HarpsichordPatchParams params;
  params.eight_b = true;
  params.four = true;
  const auto render = [&](uint8_t note, double sr, double seconds) {
    std::vector<float> slab(
        static_cast<size_t>(sonare::midi::synth::harpsichord_slab_capacity(sr)));
    sonare::midi::synth::HarpsichordVoiceCore core;
    core.attach(slab.data(), sonare::midi::synth::harpsichord_buffer_capacity(sr));
    core.start(params, sr, note, Velocity16::from7(100), 0x5eedu);
    Rendered out;
    out.factor = core.oversample();
    out.x.resize(static_cast<size_t>(seconds * sr));
    for (float& v : out.x) v = core.render(1.0f);
    return out;
  };
  require_floored_notes_hold_pitch({5.1, 3.0, 2.3, 0.9}, 127, render);
}

TEST_CASE("a piano sounds each note whose strings are short at a low rate", "[midi][synth][wind]") {
  sonare::midi::synth::PianoPatchParams params;
  const auto render = [&](uint8_t note, double sr, double seconds) {
    std::vector<float> slab(static_cast<size_t>(sonare::midi::synth::piano_slab_capacity(sr)));
    sonare::midi::synth::PianoVoiceCore core;
    core.attach(slab.data(), sonare::midi::synth::piano_string_capacity(sr));
    core.start(params, sr, note, Velocity16::from7(100), 0x5eedu);
    Rendered out;
    out.factor = core.oversample();
    out.x.resize(static_cast<size_t>(seconds * sr));
    for (float& v : out.x) v = core.render(1.0f);
    return out;
  };
  require_floored_notes_hold_pitch({3.9, 2.0, 1.6, 0.9}, 127, render);
}

namespace {

/// One wind voice at kReferenceRate: every note of its compass (@p compass_low to
/// @p compass_top) has to sound its fundamental, and every note from the compass up to the top
/// of the band that does has to sit within @p tolerance_cents of equal temperament.
struct CompassVoice {
  const char* name;
  std::function<Rendered(uint8_t)> render;
  int compass_low;
  int compass_top;
  double tolerance_cents;
};

}  // namespace

TEST_CASE("each wind voice sounds its equal-tempered fundamental across its compass at 48 kHz",
          "[midi][synth][wind]") {
  // The tolerances are one scan step of the estimator (0.87 cents) over the largest residual
  // left across the compass once every term the loop compensation can name is in it: the
  // cone's highpass lead enters by a regime-dependent share (within 5.2 cents at the shipped
  // 0.75), the cylinder's 3 Hz blocker leaves +3.5 (its lead is not compensated: the square
  // wave is timed by its edges), the flute's jet lock sits on the period (within 4.3), the brass
  // lip's +2.6 is its named trim (within 2.6), and the organ carries its deliberate per-pipe detune
  // of up to 4 cents (within 8.7 at the bottom of a 16' rank). Before the compensations were
  // complete the cone sat +8.6 across its top octave, the flute -5 to -8 and +11 at note 48, the
  // brass -4.3. The reeds take a bright bell as well as the shipped 0.5, which stops the cylinder
  // speaking above note 108; at 1.0 both drop notes, at 0.9 they reach 124 and 126. The jet cannot
  // drive a flute bore under seven samples at any rate (silent at 96 kHz too), so the flute's
  // compass ends at note 116.
  struct ReedSpecimen {
    const char* name;
    bool conical;
    float brightness;
    int compass_low;
    int compass_top;
    double tolerance_cents;
  };
  const ReedSpecimen reeds[] = {{"cylindrical reed, shipped bell", false, 0.5f, 48, 108, 4.4},
                                {"cylindrical reed, bright bell", false, 0.9f, 100, 124, 4.4},
                                {"conical reed, shipped bell", true, 0.5f, 44, 109, 6.1},
                                {"conical reed, bright bell", true, 0.9f, 100, 126, 6.1}};
  std::vector<CompassVoice> voices;
  for (const ReedSpecimen& reed : reeds) {
    sonare::midi::synth::ReedPatchParams params;
    params.conical = reed.conical;
    params.brightness = reed.brightness;
    params.breath_noise = 0.0f;
    params.chiff = 0.0f;
    voices.push_back(
        {reed.name,
         [params](uint8_t note) {
           std::vector<float> slab(
               static_cast<size_t>(sonare::midi::synth::reed_slab_capacity(kReferenceRate)));
           sonare::midi::synth::ReedVoiceCore core;
           core.attach(slab.data(), sonare::midi::synth::reed_buffer_capacity(kReferenceRate));
           core.start(params, kReferenceRate, note, Velocity16::from7(100), 0x5eedu);
           Rendered out;
           out.factor = core.oversample();
           out.x.resize(static_cast<size_t>(kCoreSeconds * kReferenceRate));
           for (float& v : out.x) v = core.render(1.0f);
           return out;
         },
         reed.compass_low, reed.compass_top, reed.tolerance_cents});
  }
  {
    sonare::midi::synth::FlutePatchParams params;
    params.breath_noise = 0.0f;
    params.chiff = 0.0f;
    params.vibrato_depth = 0.0f;
    voices.push_back(
        {"flute",
         [params](uint8_t note) {
           std::vector<float> slab(
               static_cast<size_t>(sonare::midi::synth::flute_slab_capacity(kReferenceRate)));
           sonare::midi::synth::FluteVoiceCore core;
           core.attach(slab.data(), sonare::midi::synth::flute_buffer_capacity(kReferenceRate));
           core.start(params, kReferenceRate, note, Velocity16::from7(100), 0x5eedu);
           Rendered out;
           out.factor = core.oversample();
           out.x.resize(static_cast<size_t>(kCoreSeconds * kReferenceRate));
           for (float& v : out.x) v = core.render(1.0f);
           return out;
         },
         48, 116, 5.2});
  }
  {
    sonare::midi::synth::BrassPatchParams params;
    params.breath_noise = 0.0f;
    params.chiff = 0.0f;
    voices.push_back(
        {"brass",
         [params](uint8_t note) {
           std::vector<float> slab(
               static_cast<size_t>(sonare::midi::synth::brass_slab_capacity(kReferenceRate)));
           sonare::midi::synth::BrassVoiceCore core;
           core.attach(slab.data(), sonare::midi::synth::brass_buffer_capacity(kReferenceRate));
           core.start(params, kReferenceRate, note, Velocity16::from7(100), 0x5eedu);
           Rendered out;
           out.factor = core.oversample();
           out.x.resize(static_cast<size_t>(kCoreSeconds * kReferenceRate));
           for (float& v : out.x) v = core.render(1.0f);
           return out;
         },
         36, 100, 3.5});
  }
  {
    sonare::midi::synth::PipeOrganPatchParams params;
    voices.push_back({"pipe organ",
                      [params](uint8_t note) {
                        std::vector<float> slab(static_cast<size_t>(
                            sonare::midi::synth::pipe_organ_slab_capacity(kReferenceRate)));
                        sonare::midi::synth::PipeOrganVoiceCore core;
                        core.attach(slab.data(), sonare::midi::synth::pipe_organ_buffer_capacity(
                                                     kReferenceRate));
                        core.start(params, kReferenceRate, note, Velocity16::from7(100), 0x5eedu);
                        Rendered out;
                        out.factor = core.oversample();
                        out.x.resize(static_cast<size_t>(kCoreSeconds * kReferenceRate));
                        for (float& v : out.x) v = core.render(1.0f);
                        return out;
                      },
                      36, 100, 9.6});
  }
  for (const CompassVoice& voice : voices) {
    for (int note = voice.compass_low; note <= highest_in_band_note(kReferenceRate); ++note) {
      const uint8_t n = static_cast<uint8_t>(note);
      const Rendered r = voice.render(n);
      REQUIRE(std::all_of(r.x.begin(), r.x.end(), [](float v) { return std::isfinite(v); }));
      double h1 = 0.0;
      const double cents = strongest_tone_cents(r.x, note_to_hz(n), kReferenceRate, &h1);
      // Above its compass a voice may still emit something, but not the note.
      const bool sounding = h1 > kSoundingH1;
      CAPTURE(voice.name, note, r.factor, h1, cents);
      if (note <= voice.compass_top) CHECK(sounding);
      if (sounding) CHECK(std::abs(cents) <= voice.tolerance_cents);
    }
  }
}
