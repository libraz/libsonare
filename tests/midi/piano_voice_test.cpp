/// @file piano_voice_test.cpp
/// @brief Extended waveguide piano (midi/synth/piano_voice): stiff-string
///        inharmonicity (stretched partials, growing up the keyboard),
///        two-stage coupled-string decay, felt-hammer velocity -> brightness,
///        damper note-off and deterministic rendering through the GM
///        acoustic-piano fallback.

#include "midi/synth/piano_voice.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "core/fft.h"
#include "midi/midi_event.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/piano_voice_math.h"
#include "midi/ump.h"
#include "support/audio_fixtures.h"
#include "support/midi_render.h"
#include "util/constants.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::gm_fallback_patch;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::PianoPatchParams;
using sonare::midi::synth::PianoVoiceCore;
using sonare::midi::synth::SynthEngineMode;

constexpr double kRate = 48000.0;

using sonare::test::event;
using sonare::test::render_left;

std::vector<float> render_patch(const NativeSynthPatch& patch, uint8_t note, uint8_t velocity,
                                int num_samples) {
  NativeSynthConfig cfg;
  cfg.patch = patch;
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, velocity)));
  return render_left(synth, num_samples);
}

float rms(const std::vector<float>& buf, size_t from, size_t to) {
  double acc = 0.0;
  size_t n = 0;
  for (size_t i = from; i < to && i < buf.size(); ++i) {
    acc += static_cast<double>(buf[i]) * buf[i];
    ++n;
  }
  return n > 0 ? static_cast<float>(std::sqrt(acc / static_cast<double>(n))) : 0.0f;
}

/// Hann-windowed power spectrum of buf[from, from+kFft) (long window for the
/// fine partial-frequency reads the inharmonicity checks need).
constexpr int kFft = 32768;
std::vector<double> power_spectrum(const std::vector<float>& buf, size_t from) {
  return sonare::test::power_spectrum(buf, from, kFft);
}

/// Strongest spectral peak within [freq_lo, freq_hi], refined parabolically.
double peak_hz_in(const std::vector<double>& power, double freq_lo, double freq_hi) {
  const int lo = std::max(2, static_cast<int>(std::lround(freq_lo / kRate * kFft)));
  const int hi = std::min(static_cast<int>(power.size()) - 2,
                          static_cast<int>(std::lround(freq_hi / kRate * kFft)));
  int best = -1;
  double best_power = 0.0;
  for (int b = lo; b <= hi; ++b) {
    if (power[static_cast<size_t>(b)] > best_power) {
      best_power = power[static_cast<size_t>(b)];
      best = b;
    }
  }
  if (best < 0 || best_power <= 0.0) return 0.0;
  const double l = std::log(power[static_cast<size_t>(best - 1)] + 1.0e-30);
  const double c = std::log(power[static_cast<size_t>(best)] + 1.0e-30);
  const double r = std::log(power[static_cast<size_t>(best + 1)] + 1.0e-30);
  const double denom = l - 2.0 * c + r;
  const double delta = denom != 0.0 ? 0.5 * (l - r) / denom : 0.0;
  return (static_cast<double>(best) + delta) * kRate / kFft;
}

/// Partial-n frequency of a tone with fundamental near @p f0 (searched within
/// +-quarter-f0 of the stretched estimate).
double partial_hz(const std::vector<double>& power, double f0, int n) {
  const double centre = f0 * n;
  return peak_hz_in(power, centre * 0.97, centre * 1.06);
}

float note_hz(int note) { return 440.0f * std::exp2((note - 69.0f) / 12.0f); }

double peak_power_in(const std::vector<double>& power, double freq_lo, double freq_hi) {
  const int lo = std::max(1, static_cast<int>(std::lround(freq_lo / kRate * kFft)));
  const int hi = std::min(static_cast<int>(power.size()) - 1,
                          static_cast<int>(std::lround(freq_hi / kRate * kFft)));
  double best = 0.0;
  for (int b = lo; b <= hi; ++b) best = std::max(best, power[static_cast<size_t>(b)]);
  return best;
}

float piano_nominal_hz(int note) {
  using sonare::midi::synth::piano_stretch_cents;
  return note_hz(note) * std::exp2(piano_stretch_cents(static_cast<uint8_t>(note)) / 1200.0f);
}

std::vector<float> render_core_ratio(const PianoPatchParams& params, uint8_t note,
                                     float pitch_ratio, int settle_samples = 12000) {
  PianoVoiceCore core;
  std::vector<float> slab(static_cast<size_t>(sonare::midi::synth::piano_slab_capacity(kRate)),
                          0.0f);
  core.attach(slab.data(), sonare::midi::synth::piano_string_capacity(kRate));
  core.start(params, kRate, note, sonare::midi::Velocity16::from7(110), 0x5049414E4FULL);
  for (int i = 0; i < settle_samples; ++i) static_cast<void>(core.render(pitch_ratio));
  std::vector<float> late(static_cast<size_t>(kFft));
  for (float& sample : late) sample = core.render(pitch_ratio);
  return late;
}

std::vector<float> render_core_live_ratio(const PianoPatchParams& params, uint8_t note,
                                          float initial_ratio, float settled_ratio) {
  PianoVoiceCore core;
  std::vector<float> slab(static_cast<size_t>(sonare::midi::synth::piano_slab_capacity(kRate)),
                          0.0f);
  core.attach(slab.data(), sonare::midi::synth::piano_string_capacity(kRate));
  core.start(params, kRate, note, sonare::midi::Velocity16::from7(110), 0x5049414E4FULL);
  for (int i = 0; i < 12000; ++i) static_cast<void>(core.render(initial_ratio));
  // Change the host ratio after the hammer has left, then leave a separate
  // settling interval so the FFT window measures the updated resonators.
  for (int i = 0; i < 12000; ++i) static_cast<void>(core.render(settled_ratio));
  std::vector<float> late(static_cast<size_t>(kFft));
  for (float& sample : late) sample = core.render(settled_ratio);
  return late;
}

std::vector<float> render_core(PianoVoiceCore& core, int samples) {
  std::vector<float> out(static_cast<size_t>(samples));
  for (float& sample : out) sample = core.render(1.0f);
  return out;
}

double decay_rate_db_per_s(const std::vector<float>& tone) {
  constexpr size_t kWindow = 9600;  // 200 ms at 48 kHz.
  const float early = rms(tone, 0, kWindow);
  const float late = rms(tone, 3 * kWindow, 4 * kWindow);
  return 20.0 * std::log10(static_cast<double>(early) / std::max(1.0e-20f, late)) /
         (3.0 * static_cast<double>(kWindow) / kRate);
}

}  // namespace

TEST_CASE("piano partials stretch sharp and the stretch grows with partial number",
          "[midi][synth][piano]") {
  const NativeSynthPatch& piano = gm_fallback_patch(0, 0);
  REQUIRE(piano.mode == SynthEngineMode::kPiano);

  // C5: high enough for measurable stiffness, low enough for many partials.
  const int note = 72;
  const double f0 = note_hz(note);
  const std::vector<float> tone = render_patch(piano, note, 110, 48000);
  const std::vector<double> power = power_spectrum(tone, 2048);

  // The fundamental itself stays accurately tuned (within ~6 cents)...
  const double p1 = partial_hz(power, f0, 1);
  REQUIRE(p1 > 0.0);
  REQUIRE(std::fabs(p1 / f0 - 1.0) < 0.0035);

  // ...while the upper partials land sharp of the harmonic grid, with the
  // stretch growing in n (the stiff-string f_n = n*f0*sqrt(1+B*n^2) shape).
  const double p2 = partial_hz(power, f0, 2);
  const double p3 = partial_hz(power, f0, 3);
  REQUIRE(p2 > 0.0);
  REQUIRE(p3 > 0.0);
  const double stretch2 = p2 / (2.0 * p1) - 1.0;
  const double stretch3 = p3 / (3.0 * p1) - 1.0;
  REQUIRE(stretch2 > 0.001);
  REQUIRE(stretch3 > 1.5 * stretch2);
}

TEST_CASE("piano inharmonicity grows up the keyboard", "[midi][synth][piano]") {
  const NativeSynthPatch& piano = gm_fallback_patch(0, 0);
  auto second_partial_stretch = [&](int note) {
    const double f0 = note_hz(note);
    const std::vector<float> tone = render_patch(piano, static_cast<uint8_t>(note), 110, 48000);
    const std::vector<double> power = power_spectrum(tone, 2048);
    const double p1 = partial_hz(power, f0, 1);
    const double p2 = partial_hz(power, f0, 2);
    REQUIRE(p1 > 0.0);
    REQUIRE(p2 > 0.0);
    return p2 / (2.0 * p1) - 1.0;
  };
  const double low = second_partial_stretch(48);   // C3
  const double high = second_partial_stretch(84);  // C6
  REQUIRE(high > 2.0 * low);
}

TEST_CASE("the synthesized inharmonicity tracks the physical B(note) curve",
          "[midi][synth][piano]") {
  using sonare::midi::synth::piano_inharmonicity_b;
  const NativeSynthPatch& piano = gm_fallback_patch(0, 0);

  // Fit B from the low partials: f_n / (n * f1) = sqrt(1 + B*n^2), so
  // B = ((f_n / (n*f1))^2 - 1) / n^2, averaged over n = 2..4.
  auto measured_b = [&](int note) {
    const double f0 = note_hz(note);
    const std::vector<float> tone = render_patch(piano, static_cast<uint8_t>(note), 110, 48000);
    const std::vector<double> power = power_spectrum(tone, 2048);
    const double f1 = partial_hz(power, f0, 1);
    REQUIRE(f1 > 0.0);
    double acc = 0.0;
    int count = 0;
    for (int n = 2; n <= 4; ++n) {
      const double fn = partial_hz(power, f0, n);
      if (fn <= 0.0) continue;
      const double ratio = fn / (static_cast<double>(n) * f1);
      acc += (ratio * ratio - 1.0) / static_cast<double>(n * n);
      ++count;
    }
    REQUIRE(count > 0);
    return acc / count;
  };

  // C5: the measured stretch must land in the same order of magnitude as the
  // intended physical coefficient (endpoint-matched, so not exact).
  const double target_c5 = piano_inharmonicity_b(72);
  const double meas_c5 = measured_b(72);
  INFO("target B(C5)=" << target_c5 << " measured=" << meas_c5);
  REQUIRE(meas_c5 > 0.3 * target_c5);
  REQUIRE(meas_c5 < 3.0 * target_c5);

  // The fitted B rises with register, as the curve dictates.
  const double meas_c4 = measured_b(60);
  const double meas_c6 = measured_b(84);
  INFO("measured B: C4=" << meas_c4 << " C5=" << meas_c5 << " C6=" << meas_c6);
  REQUIRE(meas_c6 > meas_c4);
}

TEST_CASE("inharmonicity turns back up below the bass break", "[midi][synth][piano]") {
  using sonare::midi::synth::piano_inharmonicity_b;
  // A wound bass string is a heavy core on a scale that cannot be made long
  // enough for it, so B stops falling around C2 and climbs again toward A0.
  // A curve that keeps falling (or floors out) reads several times too
  // flexible at the bottom of the keyboard and the bass loses its growl.
  const float at_break = piano_inharmonicity_b(36);
  REQUIRE(at_break < piano_inharmonicity_b(24));
  REQUIRE(at_break < piano_inharmonicity_b(48));
  REQUIRE(piano_inharmonicity_b(21) > 2.0f * at_break);
  // Rising monotonically on each side of the break.
  for (int n = 36; n < 96; ++n) REQUIRE(piano_inharmonicity_b(n + 1) > piano_inharmonicity_b(n));
  for (int n = 21; n < 36; ++n) REQUIRE(piano_inharmonicity_b(n + 1) < piano_inharmonicity_b(n));
  // The two branches meet without a step.
  REQUIRE(piano_inharmonicity_b(35) / piano_inharmonicity_b(36) < 1.08f);
  // Held at the top key rather than extrapolated.
  REQUIRE(piano_inharmonicity_b(127) == piano_inharmonicity_b(108));
}

TEST_CASE("piano tuning follows a stretched (Railsback) octave curve", "[midi][synth][piano]") {
  using sonare::midi::synth::piano_stretch_cents;
  // A4 is the anchor; the curve is sharp in the treble, flat in the bass, and
  // grows toward both extremes. The bounds are what a tuned concert grand
  // measures rather than a round number: across three of them the top note
  // runs +34 to +57 cents and the bottom note -10 to -15, so a curve that
  // stayed inside a couple of tens of cents at the top would be the one out of
  // range. They are here to catch a curve that has run away, not to pin a
  // value -- an octave of detune at C8 is a bug, forty cents is a piano.
  REQUIRE(piano_stretch_cents(69) == 0.0f);                     // A4 anchor
  REQUIRE(piano_stretch_cents(96) > 1.0f);                      // C7 sharp
  REQUIRE(piano_stretch_cents(108) > piano_stretch_cents(96));  // grows up top
  REQUIRE(piano_stretch_cents(48) < 0.0f);                      // C3 flat
  REQUIRE(piano_stretch_cents(21) < piano_stretch_cents(48));   // flatter down low
  REQUIRE(piano_stretch_cents(108) <= 80.0f);
  REQUIRE(std::fabs(piano_stretch_cents(21)) <= 25.0f);
  // Above the top key the curve is held rather than extrapolated: a fourth
  // power run out to note 127 would ask for nearly three semitones.
  REQUIRE(piano_stretch_cents(127) == piano_stretch_cents(108));

  // Spectrally: a treble fundamental lands measurably sharp of equal
  // temperament (the stretch is FFT-resolvable up high).
  const NativeSynthPatch& piano = gm_fallback_patch(0, 0);
  const int note = 96;  // C7
  const double et = note_hz(note);
  const std::vector<float> tone = render_patch(piano, static_cast<uint8_t>(note), 100, 48000);
  const std::vector<double> power = power_spectrum(tone, 2048);
  const double f1 = peak_hz_in(power, et * 0.99, et * 1.02);
  REQUIRE(f1 > 0.0);
  const double cents = 1200.0 * std::log2(f1 / et);
  INFO("C7 measured stretch = " << cents << " cents (intended " << piano_stretch_cents(note)
                                << ")");
  // What the render has to agree with is the curve, not a fixed number: the
  // point of the spectral check is that the tuning the voice was asked for is
  // the tuning that comes out of it, so a fitted curve moving must not turn
  // this red on its own.
  REQUIRE(cents > 1.5);  // clearly sharp of ET
  REQUIRE(std::fabs(cents - piano_stretch_cents(note)) < 2.0);
}

TEST_CASE("the unison string count is graded across the keyboard", "[midi][synth][piano]") {
  using sonare::midi::synth::piano_unison_strings;
  // Single wound strings in the deep bass, wound bichords through the
  // bass-tenor, plain trichords from the tenor break up.
  REQUIRE(piano_unison_strings(21) == 1);   // A0
  REQUIRE(piano_unison_strings(29) == 1);   // F1
  REQUIRE(piano_unison_strings(30) == 2);   // F#1
  REQUIRE(piano_unison_strings(47) == 2);   // B2
  REQUIRE(piano_unison_strings(48) == 3);   // C3
  REQUIRE(piano_unison_strings(108) == 3);  // C8
}

TEST_CASE("coupled unison strings produce a two-stage decay", "[midi][synth][piano]") {
  const NativeSynthPatch& piano = gm_fallback_patch(0, 0);
  // 4 seconds of a held C4.
  const std::vector<float> tone = render_patch(piano, 60, 110, 192000);

  // Log-RMS decay rate (dB/s) over two windows: the prompt sound decays
  // clearly faster than the aftersound.
  auto decay_rate_db_per_s = [&](size_t from, size_t to) {
    const float head = rms(tone, from, from + 9600);
    const float tail = rms(tone, to - 9600, to);
    REQUIRE(head > 0.0f);
    REQUIRE(tail > 0.0f);
    const double seconds = static_cast<double>(to - 9600 - from) / kRate;
    return 20.0 * std::log10(static_cast<double>(head) / tail) / seconds;
  };
  const double early = decay_rate_db_per_s(4800, 48000);    // 0.1 - 1.0 s
  const double late = decay_rate_db_per_s(120000, 192000);  // 2.5 - 4.0 s
  REQUIRE(early > 0.0);
  // The bound comes from the instrument rather than from what this voice
  // happened to do. Measured over these same two windows on three separately
  // captured concert grands, a C4's late rate is 0.63, 0.32 and 0.15 of its
  // early one -- and C4 is where a piano's double decay is WEAKEST, which is
  // why a tighter bound here reads as a stronger test and is really a bound on
  // one recording. It is not decoration: with the two-stage contrast forced to
  // zero this voice returns 0.91, so the assertion still separates a coupled
  // unison from a single recirculating gain.
  REQUIRE(late < 0.8 * early);
}

TEST_CASE("the treble rings on while the key is held", "[midi][synth][piano]") {
  // The aftersound of a concert grand has no register trend worth speaking of
  // from the bottom of the keyboard to note 90: measured on three of them it
  // sits between 9 and 50 s of t60 across that whole span, and only then falls
  // off. A taper that starts from the middle instead gave C6 a t60 near 1.5 s,
  // which is a note gone before the key comes up — and no per-note shape metric
  // could see it, because they are all normalised by the note's own level.
  //
  // Scored against C4 rather than against an absolute rate, so the patch's own
  // decay time can be retuned without rewriting the bound.
  const NativeSynthPatch& piano = gm_fallback_patch(0, 0);
  auto survives_two_seconds = [&](uint8_t note) {
    const std::vector<float> tone = render_patch(piano, note, 96, 120000);  // 2.5 s held
    const float early = rms(tone, 4800, 14400);                             // 0.10 - 0.30 s
    const float late = rms(tone, 105600, 115200);                           // 2.20 - 2.40 s
    REQUIRE(early > 0.0f);
    return 20.0 * std::log10(static_cast<double>(late) / early);
  };
  const double c4 = survives_two_seconds(60);
  const double c6 = survives_two_seconds(84);
  // C6 may fall away faster than C4, but not by another 20 dB over two seconds.
  REQUIRE(c6 > c4 - 20.0);
  // And it must still be there at all — a voice freed mid-note reads as silence.
  REQUIRE(c6 > -60.0);
}

TEST_CASE("the top octave is not consumed by its own dispersion", "[midi][synth][piano]") {
  // The stiff-string allpass cascade sits inside the waveguide loop, so its
  // phase delay competes with the string's period for the same round trip. At
  // the top of the keyboard the period runs out first and the cascade stops
  // buying anything, while still costing the loop the note. Dispersion is faded
  // out over the top octave for that reason, and the property that matters is
  // that the top note outlasts the strike rather than that any one knob is set
  // a particular way.
  const NativeSynthPatch& piano = gm_fallback_patch(0, 0);
  const std::vector<float> top = render_patch(piano, 108, 96, 48000);  // 1 s held
  const float attack = rms(top, 0, 4800);                              // 0 - 0.10 s
  const float held = rms(top, 24000, 33600);                           // 0.50 - 0.70 s
  REQUIRE(attack > 0.0f);
  REQUIRE(held > 0.0f);
  REQUIRE(20.0 * std::log10(static_cast<double>(held) / attack) > -40.0);
}

TEST_CASE("top modal piano partials follow a live pitch ratio", "[midi][synth][piano]") {
  const PianoPatchParams params = gm_fallback_patch(0, 0).piano;
  constexpr std::array<float, 3> kRatios = {0.5f, 1.0f, 2.0f};

  for (const uint8_t note : {uint8_t{100}, uint8_t{108}}) {
    const double nominal = piano_nominal_hz(note);
    for (const float ratio : kRatios) {
      const std::vector<float> late = render_core_ratio(params, note, ratio);
      const std::vector<double> power = power_spectrum(late, 0);
      const double target = nominal * ratio;
      const double target_power = peak_power_in(power, target * 0.97, target * 1.03);
      const double nominal_power = peak_power_in(power, nominal * 0.97, nominal * 1.03);
      const double peak = peak_hz_in(power, target * 0.97, target * 1.03);
      INFO("note=" << static_cast<int>(note) << " ratio=" << ratio << " target=" << target
                   << " peak=" << peak << " target power=" << target_power
                   << " nominal power=" << nominal_power);
      CHECK(rms(late, 0, late.size()) > 1.0e-7f);
      CHECK(target_power > 0.0);
      CHECK(nominal_power > 0.0);
      CHECK(peak > 0.0);
      CHECK(std::fabs(std::log2(peak / target)) < 0.025);
      if (ratio != 1.0f) CHECK(target_power > 1.05 * nominal_power);
    }
  }
}

TEST_CASE("piano modal drive gain follows the bent physical period", "[midi][synth][piano]") {
  constexpr float kWeight = 0.37f;
  constexpr std::array<float, 3> kRatios = {0.5f, 1.0f, 1.5f};
  constexpr std::array<float, 3> kPeriods = {17.0f, 29.0f, 53.0f};

  for (const float period : kPeriods) {
    for (const float ratio : kRatios) {
      const float omega0 = sonare::constants::kTwoPi / period;
      const float w = omega0 * ratio;
      const float gain =
          sonare::midi::synth::piano_detail::piano_modal_drive_gain(kWeight, ratio, w, period);
      // A physical impulse response has amplitude gain/sin(w); after bending,
      // its period is P/ratio. The normalized impulse weight must therefore be
      // invariant at 2*weight, independently of the timbre or pitch choice.
      const float recovered_weight = gain / std::sin(w) * (period / ratio);
      INFO("omega0=" << omega0 << " ratio=" << ratio << " period=" << period
                     << " recovered=" << recovered_weight);
      CHECK(std::fabs(recovered_weight - 2.0f * kWeight) < 2.0e-5f);
    }
  }
}

TEST_CASE("a live pitch-ratio update retunes the top modal fundamental", "[midi][synth][piano]") {
  const PianoPatchParams params = gm_fallback_patch(0, 0).piano;
  constexpr uint8_t kNote = 100;
  constexpr float kInitialRatio = 1.0f;
  constexpr float kSettledRatio = 0.5f;
  const double nominal = piano_nominal_hz(kNote);
  const double target = nominal * kSettledRatio;

  const std::vector<float> late =
      render_core_live_ratio(params, kNote, kInitialRatio, kSettledRatio);
  const std::vector<double> power = power_spectrum(late, 0);
  const double target_power = peak_power_in(power, target * 0.97, target * 1.03);
  const double nominal_power = peak_power_in(power, nominal * 0.97, nominal * 1.03);
  const double peak = peak_hz_in(power, target * 0.97, target * 1.03);
  INFO("nominal=" << nominal << " target=" << target << " peak=" << peak
                  << " target power=" << target_power << " nominal power=" << nominal_power);
  REQUIRE(rms(late, 0, late.size()) > 1.0e-7f);
  REQUIRE(target_power > 1.0e-8);
  REQUIRE(target_power > 20.0 * nominal_power);
  REQUIRE(peak > 0.0);
  REQUIRE(std::fabs(std::log2(peak / target)) < 0.025);
}

TEST_CASE("top modal history stays clear across an out-of-band public pitch round trip",
          "[midi][synth][piano]") {
  const PianoPatchParams params = gm_fallback_patch(0, 0).piano;
  constexpr uint8_t kNote = 108;
  constexpr int kDriveSamples = 12000;
  constexpr int kRoundTripSamples = 4096;
  constexpr float kOutRatio = 8.0f;

  PianoVoiceCore a;
  PianoVoiceCore b;
  std::vector<float> slab_a(static_cast<size_t>(sonare::midi::synth::piano_slab_capacity(kRate)),
                            0.0f);
  std::vector<float> slab_b(static_cast<size_t>(sonare::midi::synth::piano_slab_capacity(kRate)),
                            0.0f);
  a.attach(slab_a.data(), sonare::midi::synth::piano_string_capacity(kRate));
  b.attach(slab_b.data(), sonare::midi::synth::piano_string_capacity(kRate));
  a.start(params, kRate, kNote, sonare::midi::Velocity16::from7(110), 0x5049414E4FULL);
  b.start(params, kRate, kNote, sonare::midi::Velocity16::from7(110), 0x5049414E4FULL);

  std::vector<float> pre(static_cast<size_t>(kDriveSamples));
  for (int i = 0; i < kDriveSamples; ++i) {
    pre[static_cast<size_t>(i)] = a.render(1.0f);
    b.render(1.0f);
  }
  REQUIRE(rms(pre, 0, pre.size()) > 1.0e-7f);

  // Both voices leave the modal bank out of band after the excitation has
  // finished. This transition must clear every active pole history.
  for (int i = 0; i < kDriveSamples; ++i) {
    a.render(kOutRatio);
    b.render(kOutRatio);
  }

  // A returns to the audible ratio while B stays out of band. With no new
  // excitation and the top loop disabled, A must not resurrect old modes.
  std::vector<float> returned_diff(static_cast<size_t>(kRoundTripSamples));
  for (int i = 0; i < kRoundTripSamples; ++i) {
    returned_diff[static_cast<size_t>(i)] = a.render(1.0f) - b.render(kOutRatio);
  }
  REQUIRE(rms(returned_diff, 0, returned_diff.size()) < 1.0e-7f);
}

TEST_CASE("the modal crossover does not retain an unbent fundamental", "[midi][synth][piano]") {
  const PianoPatchParams params = gm_fallback_patch(0, 0).piano;
  const double nominal = piano_nominal_hz(95);
  for (const float ratio : {std::exp2(-2.0f / 12.0f), std::exp2(2.0f / 12.0f)}) {
    const std::vector<float> late = render_core_ratio(params, 95, ratio);
    const std::vector<double> power = power_spectrum(late, 0);
    const double target = nominal * ratio;
    const double bent_power = peak_power_in(power, target * 0.97, target * 1.03);
    const double unbent_power = peak_power_in(power, nominal * 0.97, nominal * 1.03);
    INFO("ratio=" << ratio << " bent power=" << bent_power << " unbent power=" << unbent_power);
    CHECK(rms(late, 0, late.size()) > 1.0e-7f);
    CHECK(bent_power > 0.0);
    CHECK(unbent_power < 1.0e-4 * bent_power);
  }
}

TEST_CASE("an initial downbend brings the covered top modal sixth partial into the band",
          "[midi][synth][piano]") {
  const PianoPatchParams params = gm_fallback_patch(0, 0).piano;
  constexpr uint8_t kNote = 108;
  constexpr float kRatio = 0.5f;
  const double nominal = piano_nominal_hz(kNote);
  const double b = sonare::midi::synth::piano_inharmonicity_b(kNote);
  const double modal_f1 = std::sqrt(1.0 + b);
  const double sixth = 6.0 * nominal * std::sqrt(1.0 + b * 36.0) / modal_f1;
  const double target = sixth * kRatio;

  // Keep the fixture meaningful: the nominal sixth is outside the admitted
  // top-modal band, while the initial downbend must bring it back in.
  CHECK(sixth > 21600.0);
  CHECK(target < 21600.0);

  const std::vector<float> late = render_core_ratio(params, kNote, kRatio, 6000);
  const std::vector<double> power = power_spectrum(late, 0);
  const double target_power = peak_power_in(power, target * 0.985, target * 1.015);
  const double peak = peak_hz_in(power, target * 0.985, target * 1.015);
  const double fundamental_power =
      peak_power_in(power, nominal * kRatio * 0.97, nominal * kRatio * 1.03);
  INFO("target sixth=" << target << " peak=" << peak << " target power=" << target_power
                       << " fundamental power=" << fundamental_power);
  REQUIRE(rms(late, 0, late.size()) > 1.0e-7f);
  REQUIRE(fundamental_power > 1.0e-4);
  REQUIRE(target_power > 1.0e-8);
  // Test that the restored partial rises above its local spectral floor;
  // its level relative to the fundamental is timbre, not pitch admission.
  const double local_floor = std::max(peak_power_in(power, target * 0.95, target * 0.96),
                                      peak_power_in(power, target * 1.04, target * 1.05));
  REQUIRE(target_power > 20.0 * local_floor);
  REQUIRE(peak > 0.0);
  REQUIRE(std::fabs(std::log2(peak / target)) < 0.02);
}

TEST_CASE("the felt hammer maps velocity to brightness", "[midi][synth][piano]") {
  const NativeSynthPatch& piano = gm_fallback_patch(0, 0);
  // The attack window only (the steady state is dominated by the slowly
  // accumulating fundamental resonance regardless of the strike).
  const std::vector<float> forte = render_patch(piano, 60, 127, 4096);
  const std::vector<float> piano_dyn = render_patch(piano, 60, 30, 4096);
  // Share of strike energy above the fundamental: the shorter (and
  // stiffer-felt) forte contact puts clearly more weight in the upper
  // partials than the soft strike.
  auto overtone_fraction = [](const std::vector<float>& tone) {
    const std::vector<double> power = power_spectrum(tone, 0);
    const int split = static_cast<int>(std::lround(392.0 / kRate * kFft));  // 1.5 * C4
    double low = 0.0;
    double high = 0.0;
    for (int b = 1; b < static_cast<int>(power.size()); ++b) {
      (b >= split ? high : low) += power[static_cast<size_t>(b)];
    }
    const double total = low + high;
    return total > 0.0 ? high / total : 0.0;
  };
  const double forte_overtones = overtone_fraction(forte);
  const double soft_overtones = overtone_fraction(piano_dyn);
  REQUIRE(forte_overtones > 1.8 * soft_overtones);
}

TEST_CASE("the velocity felt-dynamics gate is off by default and widens the pp<->ff spread",
          "[midi][synth][piano]") {
  // Share of strike energy above the fundamental (higher = brighter).
  auto overtone_fraction = [](const std::vector<float>& tone) {
    const std::vector<double> power = power_spectrum(tone, 0);
    const int split = static_cast<int>(std::lround(392.0 / kRate * kFft));  // 1.5 * C4
    double low = 0.0;
    double high = 0.0;
    for (int b = 1; b < static_cast<int>(power.size()); ++b) {
      (b >= split ? high : low) += power[static_cast<size_t>(b)];
    }
    const double total = low + high;
    return total > 0.0 ? high / total : 0.0;
  };

  NativeSynthPatch off = gm_fallback_patch(0, 0);
  off.piano.hammer_dynamics = 0.0f;  // gate off: intrinsic Hertz scaling only
  // The off path renders deterministically (no gate-induced perturbation).
  REQUIRE(render_patch(off, 60, 100, 4096) == render_patch(off, 60, 100, 4096));

  NativeSynthPatch on = off;
  on.piano.hammer_dynamics = 0.6f;
  // The parameter is live: turning the gate on changes the rendered timbre.
  REQUIRE(render_patch(on, 60, 100, 4096) != render_patch(off, 60, 100, 4096));

  // The forte-vs-piano brightness ratio is larger with the gate on than off:
  // the extra felt compression widens the dynamic timbre spread.
  auto forte_over_piano = [&](const NativeSynthPatch& p) {
    const double forte = overtone_fraction(render_patch(p, 60, 120, 4096));
    const double soft = overtone_fraction(render_patch(p, 60, 35, 4096));
    REQUIRE(soft > 0.0);
    return forte / soft;
  };
  REQUIRE(forte_over_piano(on) > forte_over_piano(off));
}

TEST_CASE("the soft pedal voices una corda darker and quieter", "[midi][synth][piano]") {
  const NativeSynthPatch& piano = gm_fallback_patch(0, 0);
  // Same note and velocity, soft pedal (CC67) engaged vs not. The split is
  // fixed (note fixed), so this isolates the felt voicing, not register.
  auto attack = [&](bool soft) {
    NativeSynthConfig cfg;
    cfg.patch = piano;
    NativeSynth synth(cfg);
    synth.prepare(kRate, 256);
    if (soft) {
      synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 67, 127)));
    }
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 110)));  // C4
    return render_left(synth, 4096);
  };
  // Band energies bracketing the felt-stiffness lowpass: the soft pedal's
  // softer felt drops the cutoff hard, so the una-corda attack must lose far
  // more of the felt band (above ~6x C4) than of the low band — comparing
  // the two ratios cancels any colouring common to both renders (board,
  // radiation, loop damping).
  auto band_energy = [](const std::vector<float>& tone, double lo_hz, double hi_hz) {
    const std::vector<double> power = power_spectrum(tone, 0);
    const int lo = static_cast<int>(std::lround(lo_hz / kRate * kFft));
    const int hi = std::min(static_cast<int>(std::lround(hi_hz / kRate * kFft)),
                            static_cast<int>(power.size()));
    double e = 0.0;
    for (int b = std::max(1, lo); b < hi; ++b) e += power[static_cast<size_t>(b)];
    return e;
  };
  const std::vector<float> normale = attack(false);
  const std::vector<float> soft = attack(true);
  const double lo_ratio = band_energy(soft, 100.0, 785.0) / band_energy(normale, 100.0, 785.0);
  const double hf_ratio = band_energy(soft, 1570.0, 4000.0) / band_energy(normale, 1570.0, 4000.0);
  INFO("soft/normale energy ratios: low=" << lo_ratio << " felt-band=" << hf_ratio);
  // Una corda softens the attack in the felt band (a dead CC67 flag reads
  // ~1.0 here). The exact margin depends on how much of the band the felt
  // pulse carries at this register, so this guards the wiring, not a size.
  REQUIRE(hf_ratio < 0.85);
  REQUIRE(lo_ratio < 0.85);
  // ...and a touch quieter at the attack.
  REQUIRE(rms(soft, 0, 4096) < rms(normale, 0, 4096));
}

TEST_CASE("the damper kills the string at note-off", "[midi][synth][piano]") {
  const NativeSynthPatch& piano = gm_fallback_patch(0, 0);
  NativeSynthConfig cfg;
  cfg.patch = piano;

  NativeSynth held_synth(cfg);
  held_synth.prepare(kRate, 256);
  held_synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 110)));
  const std::vector<float> held = render_left(held_synth, 96000);

  NativeSynth damped_synth(cfg);
  damped_synth.prepare(kRate, 256);
  damped_synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 110)));
  std::vector<float> head(24000, 0.0f);
  std::vector<float> head_r(24000, 0.0f);
  float* chans[2] = {head.data(), head_r.data()};
  damped_synth.process(chans, 2, 24000);
  damped_synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
  const std::vector<float> tail = render_left(damped_synth, 72000);

  const float held_late = rms(held, 76800, 96000);    // 1.6 - 2.0 s
  const float damped_late = rms(tail, 52800, 72000);  // same absolute window
  REQUIRE(held_late > 0.0f);
  REQUIRE(damped_late < 0.1f * held_late);
}

TEST_CASE("a softly struck string rings on longer under the damper", "[midi][synth][piano]") {
  // Damper felt loses energy in proportion to how far the string drives it, so
  // the same note released from a soft blow takes measurably longer to stop
  // than from a hard one. Measured on a concert grand it is a factor of two to
  // three across the velocity range; a linear damper would take exactly as
  // long. Scored as the ratio of what survives a fixed window after note-off to
  // what was there when the key came up, which removes the strike level the two
  // renders do not share.
  const NativeSynthPatch& piano = gm_fallback_patch(0, 0);
  auto survives = [&](uint8_t velocity) {
    NativeSynthConfig cfg;
    cfg.patch = piano;
    NativeSynth synth(cfg);
    synth.prepare(kRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 48, velocity)));
    std::vector<float> head(48000, 0.0f);
    std::vector<float> head_r(48000, 0.0f);
    float* chans[2] = {head.data(), head_r.data()};
    synth.process(chans, 2, 48000);  // 1 s held
    const float at_release = rms(head, 43200, 48000);
    synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 48, 0)));
    const std::vector<float> tail = render_left(synth, 48000);
    REQUIRE(at_release > 0.0f);
    return rms(tail, 33600, 38400) / at_release;  // 0.70 - 0.80 s after note-off
  };

  const float soft = survives(24);
  const float hard = survives(120);
  REQUIRE(hard > 0.0f);
  REQUIRE(soft > 2.0f * hard);
}

TEST_CASE("the sustain pedal adds sympathetic resonance", "[midi][synth][piano]") {
  const NativeSynthPatch& piano = gm_fallback_patch(0, 0);
  // Steady-state energy of a held note (no note-off, so the sustain pedal's
  // own note-hold cannot be the difference — only the sympathetic bank is).
  auto held_energy = [&](bool pedal_down) {
    NativeSynthConfig cfg;
    cfg.patch = piano;
    NativeSynth synth(cfg);
    synth.prepare(kRate, 256);
    if (pedal_down) {
      synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, 127)));
    }
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 48, 110)));  // C3
    const std::vector<float> tone = render_left(synth, 96000);                  // 2 s held
    return rms(tone, 48000, 96000);  // 1.0 - 2.0 s steady window
  };
  const float dry = held_energy(false);
  const float wet = held_energy(true);
  REQUIRE(dry > 0.0f);
  INFO("steady RMS: dry=" << dry << " wet=" << wet << " ratio=" << wet / dry);
  // The lifted dampers ring the shared sympathetic bank, adding energy.
  REQUIRE(wet > 1.03f * dry);
}

TEST_CASE("the shared soundboard adds a modal body resonance", "[midi][synth][piano]") {
  NativeSynthPatch piano = gm_fallback_patch(0, 0);
  auto render_with_board = [&](float mix) {
    piano.piano.soundboard = mix;
    return render_patch(piano, 60, 100, 48000);  // C4, 1 s
  };
  const std::vector<float> off = render_with_board(0.0f);
  const std::vector<float> on = render_with_board(0.30f);

  // The unity-peak resonator bank colours rather than blows up: the output
  // stays finite and within a sane factor of the board-off render.
  float peak_on = 0.0f;
  for (float s : on) peak_on = std::max(peak_on, std::fabs(s));
  REQUIRE(std::isfinite(peak_on));
  const float rms_off = rms(off, 0, 48000);
  const float rms_on = rms(on, 0, 48000);
  REQUIRE(rms_off > 0.0f);
  REQUIRE(rms_on < 2.5f * rms_off);

  // Body energy below the played fundamental (C4 ~262 Hz): the board's low
  // modes radiate there, where the dry string itself has almost nothing.
  auto sub_fundamental_energy = [](const std::vector<float>& tone) {
    const std::vector<double> power = power_spectrum(tone, 0);
    const int lo = static_cast<int>(std::lround(80.0 / kRate * kFft));
    const int hi = static_cast<int>(std::lround(220.0 / kRate * kFft));
    double acc = 0.0;
    for (int b = lo; b < hi; ++b) acc += power[static_cast<size_t>(b)];
    return acc;
  };
  const double body_off = sub_fundamental_energy(off);
  const double body_on = sub_fundamental_energy(on);
  INFO("sub-fundamental energy: off=" << body_off << " on=" << body_on);
  REQUIRE(body_on > 1.5 * body_off);
}

TEST_CASE("the half pedal damps held notes at an intermediate rate", "[midi][synth][piano]") {
  const NativeSynthPatch& piano = gm_fallback_patch(0, 0);
  // Strike C3, set the sustain pedal to a depth, release the key, then measure
  // the tail. A fuller pedal lifts the damper further, so the note rings longer.
  auto tail_after = [&](bool pedal, uint8_t depth) {
    NativeSynthConfig cfg;
    cfg.patch = piano;
    NativeSynth synth(cfg);
    synth.prepare(kRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 48, 110)));  // C3
    render_left(synth, 12000);                                                  // 0.25 s held
    if (pedal) synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, depth)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 48, 0)));
    const std::vector<float> tail = render_left(synth, 96000);  // 2 s tail
    return rms(tail, 19200, 38400);                             // 0.4 - 0.8 s after note-off
  };
  const float none = tail_after(false, 0);   // pedal up -> full damp
  const float half = tail_after(true, 90);   // half pedal -> partial damp
  const float full = tail_after(true, 127);  // full pedal -> rings freely
  REQUIRE(none > 0.0f);
  // Graded: the half pedal rings clearly longer than a full damp, and the full
  // pedal clearly longer than the half.
  REQUIRE(half > 5.0f * none);
  REQUIRE(full > 2.0f * half);
}

TEST_CASE("the sostenuto pedal holds only the notes down when it engages", "[midi][synth][piano]") {
  const NativeSynthPatch& piano = gm_fallback_patch(0, 0);
  // Strike C4, optionally work the sostenuto pedal, release the key, then
  // measure the 1.5-2.0 s tail. A captured note keeps ringing; an uncaptured
  // one is damped at key-up.
  auto cc66 = [] { return sonare::midi::make_midi1_control_change(0, 0, 66, 127); };
  auto tail_rms = [&](bool press_while_held, bool press_before_note) {
    NativeSynthConfig cfg;
    cfg.patch = piano;
    NativeSynth synth(cfg);
    synth.prepare(kRate, 256);
    if (press_before_note) synth.on_event(0, event(cc66()));
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 110)));
    render_left(synth, 12000);  // 0.25 s with the key down
    if (press_while_held) synth.on_event(0, event(cc66()));
    synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
    const std::vector<float> tail = render_left(synth, 96000);  // 2 s tail
    return rms(tail, 72000, 96000);                             // 1.5 - 2.0 s
  };
  const float captured = tail_rms(true, false);   // pedal pressed while held
  const float released = tail_rms(false, false);  // no pedal -> damped
  const float late = tail_rms(false, true);       // note struck after the press
  REQUIRE(released > 0.0f);
  // The note held when the pedal engaged keeps ringing far above the damped
  // baseline...
  REQUIRE(captured > 5.0f * released);
  // ...while a note struck after the press is not captured (unlike sustain).
  REQUIRE(late < 3.0f * released);
}

TEST_CASE("piano rendering is deterministic", "[midi][synth][piano]") {
  const NativeSynthPatch& piano = gm_fallback_patch(0, 0);
  const std::vector<float> first = render_patch(piano, 60, 100, 8192);
  const std::vector<float> second = render_patch(piano, 60, 100, 8192);
  float peak = 0.0f;
  for (float s : first) peak = std::max(peak, std::fabs(s));
  REQUIRE(peak > 0.01f);
  REQUIRE(first == second);
}

TEST_CASE("a half pedal damps by its position, not by how many CC64 messages sent it",
          "[midi][synth][piano]") {
  // A real continuous pedal repeats CC64 at the same or nearby positions; each
  // message must re-state the contact, never add another one on top.
  const auto render_with_repeats = [](int repeats) {
    NativeSynthConfig cfg;
    cfg.patch = gm_fallback_patch(0, 0);
    NativeSynth synth(cfg);
    synth.prepare(kRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
    render_left(synth, 4800);
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, 127)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
    render_left(synth, 4800);
    for (int i = 0; i < repeats; ++i) {
      synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, 90)));
    }
    return render_left(synth, 96000);
  };
  const std::vector<float> once = render_with_repeats(1);
  const std::vector<float> twenty = render_with_repeats(20);
  const float once_tail = rms(once, 72000, 96000);
  const float twenty_tail = rms(twenty, 72000, 96000);
  INFO("tail rms once " << once_tail << " twenty " << twenty_tail);
  REQUIRE(once_tail > 1.0e-5f);  // non-vacuity: the half-pedalled note still rings
  REQUIRE(twenty_tail == once_tail);
}

TEST_CASE("the piano core follows a half-pedal back toward its natural decay",
          "[midi][synth][piano]") {
  const PianoPatchParams params = gm_fallback_patch(0, 0).piano;

  // CC64=90 is a contact strength of 37/63, and CC64=110 is 17/63. The
  // second contact must replace the first one. Comparing decay slopes avoids
  // requiring lost energy to come back when the damper retreats.
  const auto rate_after_contacts = [&](float first, float second) {
    PianoVoiceCore core;
    std::vector<float> slab(static_cast<size_t>(sonare::midi::synth::piano_slab_capacity(kRate)),
                            0.0f);
    core.attach(slab.data(), sonare::midi::synth::piano_string_capacity(kRate));
    core.start(params, kRate, 60, sonare::midi::Velocity16::from7(110), 0x504544u);
    render_core(core, 12000);  // let the hammer leave the string
    core.damp(first);
    render_core(core, 4800);  // the weaker contact arrives later
    if (second >= 0.0f) core.damp(second);
    return decay_rate_db_per_s(render_core(core, 38400));
  };

  const float half = rate_after_contacts(37.0f / 63.0f, -1.0f);
  const float shallower = rate_after_contacts(37.0f / 63.0f, 17.0f / 63.0f);
  const float natural = rate_after_contacts(37.0f / 63.0f, 0.0f);
  INFO("core decay rates: half=" << half << " shallower=" << shallower << " natural=" << natural);
  REQUIRE(shallower + 0.3f < half);
  REQUIRE(natural + 0.3f < shallower);
}

TEST_CASE("full piano damper contact never lengthens a faster natural decay",
          "[midi][synth][piano]") {
  PianoPatchParams params = gm_fallback_patch(0, 0).piano;
  // Exercise the endpoint ordering the shipped patch normally does not reach:
  // both natural stages are already faster than this patch's full-damper t60.
  params.decay_fast_s = 0.5f;
  params.decay_slow_s = 0.8f;
  params.release_damp_s = 10.0f;

  const auto rate_after = [&](float contact_strength) {
    PianoVoiceCore core;
    std::vector<float> slab(static_cast<size_t>(sonare::midi::synth::piano_slab_capacity(kRate)),
                            0.0f);
    core.attach(slab.data(), sonare::midi::synth::piano_string_capacity(kRate));
    core.start(params, kRate, 60, sonare::midi::Velocity16::from7(110), 0x504545u);
    render_core(core, 1200);
    if (contact_strength < 0.0f)
      core.release();
    else
      core.damp(contact_strength);
    return decay_rate_db_per_s(render_core(core, 38400));
  };

  const float release_rate = rate_after(-1.0f);
  const float natural_rate = rate_after(0.0f);
  const float half_contact_rate = rate_after(0.5f);
  const float full_contact_rate = rate_after(1.0f);
  INFO("short-natural rates: release=" << release_rate << " natural=" << natural_rate
                                       << " half contact=" << half_contact_rate
                                       << " full contact=" << full_contact_rate);
  REQUIRE(half_contact_rate + 0.3f >= release_rate);
  REQUIRE(full_contact_rate + 0.3f >= natural_rate);
}

TEST_CASE("NativeSynth applies reverse half-pedal travel to a released key-up piano note",
          "[midi][synth][piano]") {
  auto rate_after_cc64 = [](uint8_t first_depth, uint8_t second_depth) {
    NativeSynthPatch patch = gm_fallback_patch(0, 0);
    // Keep this assertion about the string voice. The shared board is covered
    // separately and can otherwise mask a decay-rate change in the voice.
    patch.piano.soundboard = 0.0f;
    NativeSynthConfig cfg;
    cfg.patch = patch;
    NativeSynth synth(cfg);
    synth.prepare(kRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 110)));
    render_left(synth, 12000);
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, first_depth)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
    render_left(synth, 4800);
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, second_depth)));
    return decay_rate_db_per_s(render_left(synth, 38400));
  };

  const float half = rate_after_cc64(90, 90);
  const float shallower = rate_after_cc64(90, 110);
  const float natural = rate_after_cc64(90, 127);
  INFO("NativeSynth decay rates: half=" << half << " shallower=" << shallower
                                        << " natural=" << natural);
  REQUIRE(shallower + 0.3f < half);
  REQUIRE(natural + 0.3f < shallower);
}

TEST_CASE("NativeSynth does not re-damp a piano voice already in release", "[midi][synth][piano]") {
  const auto render_after_release = [](uint8_t pedal_depth) {
    NativeSynthConfig cfg;
    cfg.patch = gm_fallback_patch(0, 0);
    NativeSynth synth(cfg);
    synth.prepare(kRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 110)));
    render_left(synth, 12000);
    synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
    // Both cases open the shared sympathetic bank equally. A difference here
    // therefore proves that a pedal message changed a voice already releasing.
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, pedal_depth)));
    return render_left(synth, 16384);
  };

  const std::vector<float> full_lift = render_after_release(127);
  const std::vector<float> half_lift = render_after_release(110);
  REQUIRE(rms(full_lift, 0, 8192) > 1.0e-5f);
  const bool identical = half_lift == full_lift;
  REQUIRE(identical);
}

TEST_CASE("NativeSynth applies sustain contact when a sostenuto capture is released",
          "[midi][synth][piano]") {
  const auto render_after_capture_release = [](bool restate_sustain) {
    NativeSynthConfig cfg;
    cfg.patch = gm_fallback_patch(0, 0);
    NativeSynth synth(cfg);
    synth.prepare(kRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 110)));
    render_left(synth, 12000);
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 66, 127)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, 90)));
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 66, 0)));
    if (restate_sustain) {
      // This is the behavior the capture release must match: the same pedal
      // position is applied after the voice becomes sustain-eligible.
      synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, 90)));
    }
    return render_left(synth, 38400);
  };

  const std::vector<float> capture_release = render_after_capture_release(false);
  const std::vector<float> repeated_cc = render_after_capture_release(true);
  REQUIRE(rms(capture_release, 0, 9600) > 1.0e-5f);
  const bool identical = capture_release == repeated_cc;
  REQUIRE(identical);
}
