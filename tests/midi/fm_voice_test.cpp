/// @file fm_voice_test.cpp
/// @brief FM operator stack (midi/synth/fm_voice): deterministic rendering,
///        spectral tolerance bands for the e-piano / bell / brass fallback
///        patches (harmonic 1:1 stacks vs inharmonic bell ratios), feedback
///        spectral enrichment, velocity -> modulation index brightness and
///        key-rate-scaled decay.

#include "midi/synth/fm_voice.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <set>
#include <vector>

#include "core/fft.h"
#include "midi/midi_event.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::FmAlgorithm;
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

float rms(const std::vector<float>& buf, size_t from, size_t to) {
  double acc = 0.0;
  size_t n = 0;
  for (size_t i = from; i < to && i < buf.size(); ++i) {
    acc += static_cast<double>(buf[i]) * buf[i];
    ++n;
  }
  return n > 0 ? static_cast<float>(std::sqrt(acc / static_cast<double>(n))) : 0.0f;
}

/// Hann-windowed power spectrum of buf[from, from+kFft).
constexpr int kFft = 8192;
std::vector<double> power_spectrum(const std::vector<float>& buf, size_t from) {
  std::vector<float> windowed(kFft);
  for (int i = 0; i < kFft; ++i) {
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

/// Fraction of spectral power OUTSIDE +-3 bins of the f0 harmonic grid
/// (skipping the lowest bins). ~0 for a harmonic tone, large for bells.
double inharmonicity(const std::vector<float>& buf, size_t from, double f0) {
  const std::vector<double> power = power_spectrum(buf, from);
  std::set<int> harmonic_bins;
  for (int k = 1; k * f0 < 0.5 * kRate; ++k) {
    const int centre = static_cast<int>(std::lround(k * f0 / kRate * kFft));
    for (int b = centre - 3; b <= centre + 3; ++b) harmonic_bins.insert(b);
  }
  double harmonic = 0.0;
  double other = 0.0;
  for (int b = 8; b < static_cast<int>(power.size()); ++b) {
    if (harmonic_bins.count(b) > 0) {
      harmonic += power[static_cast<size_t>(b)];
    } else {
      other += power[static_cast<size_t>(b)];
    }
  }
  const double total = harmonic + other;
  return total > 0.0 ? other / total : 1.0;
}

/// Fraction of spectral power above @p freq_hz.
double high_band_fraction(const std::vector<float>& buf, size_t from, double freq_hz) {
  const std::vector<double> power = power_spectrum(buf, from);
  const int split = static_cast<int>(std::lround(freq_hz / kRate * kFft));
  double low = 0.0;
  double high = 0.0;
  for (int b = 1; b < static_cast<int>(power.size()); ++b) {
    (b >= split ? high : low) += power[static_cast<size_t>(b)];
  }
  const double total = low + high;
  return total > 0.0 ? high / total : 0.0;
}

/// A bypass-filter FM test patch wrapper.
NativeSynthPatch fm_base_patch() {
  NativeSynthPatch p;
  p.mode = SynthEngineMode::kFm;
  p.cutoff_hz = 20000.0f;
  p.amp_env.attack_ms = 1.0f;
  p.amp_env.sustain = 1.0f;
  return p;
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

}  // namespace

TEST_CASE("FM rendering is deterministic", "[midi][synth][fm]") {
  const NativeSynthPatch& patch = gm_fallback_patch(0, 4);  // FM e-piano
  REQUIRE(patch.mode == SynthEngineMode::kFm);
  const std::vector<float> first = render_patch(patch, 60, 100, 4096);
  const std::vector<float> second = render_patch(patch, 60, 100, 4096);
  float peak = 0.0f;
  for (float s : first) peak = std::max(peak, std::fabs(s));
  REQUIRE(peak > 0.01f);
  REQUIRE(first == second);
}

TEST_CASE("1:1 operator stacks stay harmonic, bell ratios go inharmonic", "[midi][synth][fm]") {
  // Harmonic: 2-op 1:1 stack at moderate index.
  NativeSynthPatch harmonic = fm_base_patch();
  harmonic.fm.algorithm = FmAlgorithm::kStack2;
  harmonic.fm.ops[0].ratio = 1.0f;
  harmonic.fm.ops[0].level = 1.0f;
  harmonic.fm.ops[0].env = {0.0f, 1.0f, 0.0f, 60.0f, 1.0f, 120.0f};
  harmonic.fm.ops[1].ratio = 1.0f;
  harmonic.fm.ops[1].level = 1.5f;
  harmonic.fm.ops[1].env = {0.0f, 1.0f, 0.0f, 60.0f, 1.0f, 120.0f};
  const std::vector<float> tone = render_patch(harmonic, 69, 127, 24000);
  REQUIRE(inharmonicity(tone, 12000, 440.0) < 0.02);

  // Inharmonic: the FM bell family patch (3.5 ratio sidebands).
  const NativeSynthPatch& bell = gm_fallback_patch(0, 9);  // Glockenspiel
  REQUIRE(bell.mode == SynthEngineMode::kFm);
  const std::vector<float> ring = render_patch(bell, 69, 127, 24000);
  REQUIRE(inharmonicity(ring, 4096, 440.0) > 0.3);
}

TEST_CASE("the feedback operator enriches the brass spectrum", "[midi][synth][fm]") {
  const NativeSynthPatch& brass = gm_fallback_patch(0, 56);  // Trumpet family
  REQUIRE(brass.mode == SynthEngineMode::kFm);
  REQUIRE(brass.fm.ops[2].feedback > 0.0f);

  NativeSynthPatch no_feedback = brass;
  no_feedback.fm.ops[2].feedback = 0.0f;

  const std::vector<float> with_fb = render_patch(brass, 57, 127, 24000);
  const std::vector<float> without_fb = render_patch(no_feedback, 57, 127, 24000);
  const double fb_high = high_band_fraction(with_fb, 12000, 1500.0);
  const double plain_high = high_band_fraction(without_fb, 12000, 1500.0);
  REQUIRE(fb_high > 1.2 * plain_high);
}

TEST_CASE("velocity scales the modulation index (brightness)", "[midi][synth][fm]") {
  const NativeSynthPatch& ep = gm_fallback_patch(0, 4);  // FM e-piano
  const std::vector<float> loud = render_patch(ep, 60, 127, 12000);
  const std::vector<float> soft = render_patch(ep, 60, 30, 12000);
  // Brightness, not just level: compare spectral balance above ~1.5 kHz.
  const double loud_high = high_band_fraction(loud, 2048, 1500.0);
  const double soft_high = high_band_fraction(soft, 2048, 1500.0);
  REQUIRE(loud_high > 2.0 * soft_high);
}

TEST_CASE("key-rate scaling shortens decay up the keyboard", "[midi][synth][fm]") {
  const NativeSynthPatch& ep = gm_fallback_patch(0, 4);  // FM e-piano, krs > 0
  auto decay_ratio = [](const std::vector<float>& buf) {
    // Level after 0.5 s relative to the initial strike window.
    const float early = rms(buf, 480, 4800);
    const float late = rms(buf, 24000, 28800);
    return early > 0.0f ? late / early : 0.0f;
  };
  const std::vector<float> low_note = render_patch(ep, 36, 110, 28800);
  const std::vector<float> high_note = render_patch(ep, 96, 110, 28800);
  // The high note must have decayed appreciably further after 0.5 s.
  REQUIRE(decay_ratio(high_note) < 0.6f * decay_ratio(low_note));
}
