/// @file ks_voice_test.cpp
/// @brief Karplus-Strong string (midi/synth/ks_voice): fractional-delay
///        tuning accuracy, decay stretching down the keyboard, pick-position
///        comb notches, velocity -> brightness, note-off damping and
///        deterministic rendering through the GM guitar/harp fallbacks.

#include "midi/synth/ks_voice.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "core/fft.h"
#include "midi/midi_event.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"
#include "support/alloc_guard.h"

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
                                int num_samples, int note_off_at = -1) {
  NativeSynthConfig cfg;
  cfg.patch = patch;
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, velocity)));
  if (note_off_at < 0) return render_left(synth, num_samples);
  std::vector<float> head(static_cast<size_t>(note_off_at));
  std::vector<float> head_r(static_cast<size_t>(note_off_at));
  float* chans[2] = {head.data(), head_r.data()};
  synth.process(chans, 2, note_off_at);
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, note, 0)));
  std::vector<float> tail = render_left(synth, num_samples - note_off_at);
  head.insert(head.end(), tail.begin(), tail.end());
  return head;
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

/// Interpolated zero-crossing frequency estimate over buf[from, to). The
/// buffer is first isolated to its fundamental with eight one-pole lowpass
/// passes at @p f0_hint * 1.3 (harmonic wiggles would otherwise add spurious
/// crossings), then full cycles are counted between the first and last upward
/// crossing with linear sub-sample interpolation at both ends. The one-pole
/// phase shift is constant across the window, so it cancels in the period.
double estimate_frequency(const std::vector<float>& buf, size_t from, size_t to, double f0_hint) {
  std::vector<float> lp(buf.begin(), buf.end());
  const float alpha =
      1.0f - static_cast<float>(std::exp(-2.0 * 3.14159265358979 * f0_hint * 1.3 / kRate));
  for (int pass = 0; pass < 8; ++pass) {
    float state = 0.0f;
    for (float& s : lp) {
      state += alpha * (s - state);
      s = state;
    }
  }
  // Remove the window mean so a residual DC offset cannot hide crossings.
  double mean = 0.0;
  size_t count = 0;
  for (size_t i = from; i < to && i < lp.size(); ++i) {
    mean += lp[i];
    ++count;
  }
  if (count > 0) {
    mean /= static_cast<double>(count);
    for (size_t i = from; i < to && i < lp.size(); ++i) lp[i] -= static_cast<float>(mean);
  }
  double first = -1.0;
  double last = -1.0;
  int cycles = 0;
  for (size_t i = from + 1; i < to && i < lp.size(); ++i) {
    if (lp[i - 1] < 0.0f && lp[i] >= 0.0f) {
      const double frac = static_cast<double>(lp[i - 1]) / (static_cast<double>(lp[i - 1]) - lp[i]);
      const double t = static_cast<double>(i - 1) + frac;
      if (first < 0.0) {
        first = t;
      } else {
        last = t;
        ++cycles;
      }
    }
  }
  if (cycles < 1 || last <= first) return 0.0;
  return static_cast<double>(cycles) * kRate / (last - first);
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

/// Power of harmonic k (+-2 bins around k*f0).
double harmonic_power(const std::vector<double>& power, double f0, int k) {
  const int centre = static_cast<int>(std::lround(k * f0 / kRate * kFft));
  double acc = 0.0;
  for (int b = centre - 2; b <= centre + 2; ++b) {
    if (b > 0 && b < static_cast<int>(power.size())) acc += power[static_cast<size_t>(b)];
  }
  return acc;
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

/// A bright filter-bypassed KS test patch.
NativeSynthPatch ks_base_patch() {
  NativeSynthPatch p;
  p.mode = SynthEngineMode::kKarplusStrong;
  p.cutoff_hz = 20000.0f;
  p.amp_env.attack_ms = 1.0f;
  p.amp_env.sustain = 1.0f;
  return p;
}

}  // namespace

TEST_CASE("KS rendering is deterministic", "[midi][synth][ks]") {
  const NativeSynthPatch& patch = gm_fallback_patch(0, 25);  // steel guitar (KS family)
  REQUIRE(patch.mode == SynthEngineMode::kKarplusStrong);
  const std::vector<float> first = render_patch(patch, 52, 100, 4096);
  const std::vector<float> second = render_patch(patch, 52, 100, 4096);
  float peak = 0.0f;
  for (float s : first) peak = std::max(peak, std::fabs(s));
  REQUIRE(peak > 0.01f);
  REQUIRE(first == second);
}

TEST_CASE("KS fractional-delay tuning is accurate", "[midi][synth][ks]") {
  NativeSynthPatch patch = ks_base_patch();
  patch.ks.brightness = 0.7f;
  patch.ks.pick_position = 0.18f;
  // A4 (440 Hz) and A2 (110 Hz): the sounding fundamental must match the note
  // within 5 cents (|ratio - 1| < 0.29%).
  for (const auto& [note, expected] :
       {std::pair<uint8_t, double>{69, 440.0}, std::pair<uint8_t, double>{45, 110.0}}) {
    const std::vector<float> tone = render_patch(patch, note, 110, 48000);
    const double estimated = estimate_frequency(tone, 8000, 44000, expected);
    REQUIRE(estimated > 0.0);
    REQUIRE(std::fabs(estimated / expected - 1.0) < 0.0029);
  }
}

TEST_CASE("decay stretching: low strings ring longer", "[midi][synth][ks]") {
  NativeSynthPatch patch = ks_base_patch();
  patch.ks.decay_s = 2.0f;
  patch.ks.decay_stretch = 0.7f;
  auto decay_ratio = [](const std::vector<float>& buf) {
    const float early = rms(buf, 2000, 8000);
    const float late = rms(buf, 38400, 48000);  // 0.8-1.0 s
    return early > 0.0f ? late / early : 0.0f;
  };
  const std::vector<float> low_note = render_patch(patch, 40, 110, 48000);
  const std::vector<float> high_note = render_patch(patch, 76, 110, 48000);
  REQUIRE(decay_ratio(low_note) > 1.5f * decay_ratio(high_note));
}

TEST_CASE("pick-position comb notches the matching harmonic", "[midi][synth][ks]") {
  // Picking at the middle of the string (0.5) puts a node at every even
  // harmonic; picking near the bridge (0.1) keeps them strong.
  NativeSynthPatch middle = ks_base_patch();
  middle.ks.brightness = 0.85f;
  middle.ks.pick_position = 0.5f;
  NativeSynthPatch bridge = middle;
  bridge.ks.pick_position = 0.1f;

  const double f0 = 220.0;
  const std::vector<float> mid_tone = render_patch(middle, 57, 110, 24000);
  const std::vector<float> bridge_tone = render_patch(bridge, 57, 110, 24000);
  const std::vector<double> mid_power = power_spectrum(mid_tone, 2048);
  const std::vector<double> bridge_power = power_spectrum(bridge_tone, 2048);

  // Second-harmonic level relative to the fundamental, per pick position.
  const double mid_h2 = harmonic_power(mid_power, f0, 2) / harmonic_power(mid_power, f0, 1);
  const double bridge_h2 =
      harmonic_power(bridge_power, f0, 2) / harmonic_power(bridge_power, f0, 1);
  REQUIRE(bridge_h2 > 8.0 * mid_h2);
}

TEST_CASE("velocity opens the excitation lowpass (brightness)", "[midi][synth][ks]") {
  NativeSynthPatch patch = ks_base_patch();
  patch.ks.vel_to_brightness = 0.8f;
  const std::vector<float> loud = render_patch(patch, 52, 127, 16000);
  const std::vector<float> soft = render_patch(patch, 52, 30, 16000);
  const double loud_high = high_band_fraction(loud, 1024, 1200.0);
  const double soft_high = high_band_fraction(soft, 1024, 1200.0);
  REQUIRE(loud_high > 1.5 * soft_high);
}

TEST_CASE("note-off damps the string", "[midi][synth][ks]") {
  NativeSynthPatch patch = ks_base_patch();
  patch.ks.decay_s = 6.0f;
  patch.ks.release_damp_s = 0.05f;
  patch.amp_env.release_ms = 400.0f;
  // Same note, held vs released at 0.25 s; compare the 0.6-0.8 s window.
  const std::vector<float> held = render_patch(patch, 52, 110, 38400);
  const std::vector<float> damped = render_patch(patch, 52, 110, 38400, /*note_off_at=*/12000);
  const float held_late = rms(held, 28800, 38400);
  const float damped_late = rms(damped, 28800, 38400);
  REQUIRE(held_late > 0.0f);
  REQUIRE(damped_late < 0.1f * held_late);
}

TEST_CASE("KS audio path is allocation-free", "[midi][synth][ks]") {
  NativeSynthConfig cfg;
  cfg.patch = ks_base_patch();
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);

  std::vector<float> left(256, 0.0f);
  std::vector<float> right(256, 0.0f);
  float* chans[2] = {left.data(), right.data()};
  {
    sonare::test::AllocationGuard guard;
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 40, 100)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 52, 100)));
    synth.process(chans, 2, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 40, 0)));
    synth.process(chans, 2, 256);
    REQUIRE(guard.count() == 0);
  }
}

TEST_CASE("GM harp fallback is a stretched KS string", "[midi][synth][ks]") {
  const NativeSynthPatch& harp = gm_fallback_patch(0, 46);
  REQUIRE(harp.mode == SynthEngineMode::kKarplusStrong);
  const std::vector<float> tone = render_patch(harp, 60, 100, 24000);
  float peak = 0.0f;
  for (float s : tone) peak = std::max(peak, std::fabs(s));
  REQUIRE(peak > 0.01f);
  // Pitch sanity on the fallback patch too (C4 = 261.63 Hz).
  const double estimated = estimate_frequency(tone, 4000, 20000, 261.6256);
  REQUIRE(std::fabs(estimated / 261.6256 - 1.0) < 0.005);
}
