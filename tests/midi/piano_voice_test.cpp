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

TEST_CASE("piano rendering is deterministic", "[midi][synth][piano]") {
  const NativeSynthPatch& piano = gm_fallback_patch(0, 0);
  const std::vector<float> first = render_patch(piano, 60, 100, 8192);
  const std::vector<float> second = render_patch(piano, 60, 100, 8192);
  float peak = 0.0f;
  for (float s : first) peak = std::max(peak, std::fabs(s));
  REQUIRE(peak > 0.01f);
  REQUIRE(first == second);
}
