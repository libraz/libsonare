/// @file piano_voice_test.cpp
/// @brief Extended waveguide piano (midi/synth/piano_voice): stiff-string
///        inharmonicity (stretched partials, growing up the keyboard),
///        two-stage coupled-string decay, felt-hammer velocity -> brightness,
///        damper note-off and deterministic rendering through the GM
///        acoustic-piano fallback.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "core/fft.h"
#include "midi/midi_event.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::gm_fallback_patch;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::SynthEngineMode;

constexpr double kRate = 48000.0;

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

std::vector<float> render_left(NativeSynth& synth, int num_samples) {
  std::vector<float> left(static_cast<size_t>(num_samples), 0.0f);
  std::vector<float> right(static_cast<size_t>(num_samples), 0.0f);
  float* chans[2] = {left.data(), right.data()};
  synth.process(chans, 2, num_samples);
  return left;
}

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
  std::vector<float> windowed(kFft, 0.0f);
  for (int i = 0; i < kFft && from + static_cast<size_t>(i) < buf.size(); ++i) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * i / (kFft - 1));
    windowed[static_cast<size_t>(i)] = buf[from + static_cast<size_t>(i)] * static_cast<float>(w);
  }
  sonare::FFT fft(kFft);
  std::vector<std::complex<float>> spectrum(static_cast<size_t>(fft.n_bins()));
  fft.forward(windowed.data(), spectrum.data());
  std::vector<double> power(spectrum.size());
  for (size_t i = 0; i < spectrum.size(); ++i) power[i] = std::norm(spectrum[i]);
  return power;
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

TEST_CASE("piano tuning follows a stretched (Railsback) octave curve", "[midi][synth][piano]") {
  using sonare::midi::synth::piano_stretch_cents;
  // A4 is the anchor; the curve is sharp in the treble, flat in the bass, and
  // grows toward both extremes (clamped to a tasteful range).
  REQUIRE(piano_stretch_cents(69) == 0.0f);                     // A4 anchor
  REQUIRE(piano_stretch_cents(96) > 1.0f);                      // C7 sharp
  REQUIRE(piano_stretch_cents(108) > piano_stretch_cents(96));  // grows up top
  REQUIRE(piano_stretch_cents(48) < 0.0f);                      // C3 flat
  REQUIRE(piano_stretch_cents(21) < piano_stretch_cents(48));   // flatter down low
  REQUIRE(std::fabs(piano_stretch_cents(108)) <= 22.0f);
  REQUIRE(std::fabs(piano_stretch_cents(21)) <= 22.0f);

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
  REQUIRE(cents > 1.5);   // clearly sharp of ET
  REQUIRE(cents < 12.0);  // but within the tasteful range
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
  REQUIRE(late < 0.6 * early);
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
