/// @file flute_voice_test.cpp
/// @brief Air-jet flute waveguide (midi/synth/flute_voice): fundamental tuning
///        (the jet locking the first register), the octave-rich open-flue-pipe
///        spectrum (a flute radiates a prominent 2nd harmonic, unlike the odd-
///        only clarinet), prompt speech + steady sustain, note-off ring-down,
///        unconditional stability across the keyboard, dynamics and parameter
///        extremes, and deterministic rendering.

#include "midi/synth/flute_voice.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "core/fft.h"
#include "midi/midi_event.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"
#include "support/audio_fixtures.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::FlutePatchParams;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::SynthEngineMode;

using sonare::test::kFft;
using sonare::test::kRate;

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

std::vector<float> render_cc_change(const NativeSynthPatch& patch, uint8_t note, uint8_t velocity,
                                    int pre, int post, uint8_t cc, uint8_t value) {
  NativeSynthConfig cfg;
  cfg.patch = patch;
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, velocity)));
  std::vector<float> head = render_left(synth, pre);
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, cc, value)));
  std::vector<float> tail = render_left(synth, post);
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

float peak(const std::vector<float>& buf) {
  float p = 0.0f;
  for (float s : buf) p = std::max(p, std::fabs(s));
  return p;
}

using sonare::test::power_spectrum;

double fft_fundamental(const std::vector<float>& buf, size_t from, double f0_hint) {
  const std::vector<double> ps = power_spectrum(buf, from);
  const int lo = std::max(1, static_cast<int>(0.5 * f0_hint / kRate * kFft));
  const int hi =
      std::min(static_cast<int>(ps.size()) - 2, static_cast<int>(1.5 * f0_hint / kRate * kFft));
  int pk = lo;
  for (int b = lo; b <= hi; ++b)
    if (ps[static_cast<size_t>(b)] > ps[static_cast<size_t>(pk)]) pk = b;
  const double lm = std::log(ps[static_cast<size_t>(pk - 1)] + 1e-30);
  const double l0 = std::log(ps[static_cast<size_t>(pk)] + 1e-30);
  const double lp = std::log(ps[static_cast<size_t>(pk + 1)] + 1e-30);
  const double denom = lm - 2.0 * l0 + lp;
  const double delta = denom != 0.0 ? 0.5 * (lm - lp) / denom : 0.0;
  return (static_cast<double>(pk) + delta) * kRate / kFft;
}

double harmonic_power(const std::vector<double>& power, double f0, int k) {
  const int centre = static_cast<int>(std::lround(k * f0 / kRate * kFft));
  double acc = 0.0;
  for (int b = centre - 2; b <= centre + 2; ++b) {
    if (b > 0 && b < static_cast<int>(power.size())) acc += power[static_cast<size_t>(b)];
  }
  return acc;
}

double note_hz(uint8_t note) { return 440.0 * std::pow(2.0, (static_cast<int>(note) - 69) / 12.0); }

/// A filter-bypassed flute test patch (raw bore, no body resonance so the pitch
/// and harmonic measurements read the air column directly).
NativeSynthPatch flute_base_patch() {
  NativeSynthPatch p;
  p.mode = SynthEngineMode::kFlute;
  p.cutoff_hz = 20000.0f;
  p.amp_env.attack_ms = 5.0f;
  p.amp_env.sustain = 1.0f;
  p.amp_env.release_ms = 100.0f;
  p.flute.breath_pressure = 0.6f;
  p.flute.vel_to_breath = 0.5f;
  p.flute.jet_ratio = 0.5f;
  p.flute.jet_reflection = 0.5f;
  p.flute.end_reflection = 0.5f;
  p.flute.brightness = 0.5f;
  p.flute.damping = 0.35f;
  return p;
}

}  // namespace

TEST_CASE("flute rendering is deterministic", "[midi][synth][flute]") {
  const NativeSynthPatch patch = flute_base_patch();
  const std::vector<float> first = render_patch(patch, 72, 100, 16384);
  const std::vector<float> second = render_patch(patch, 72, 100, 16384);
  REQUIRE(peak(first) > 0.01f);
  REQUIRE(first == second);
}

TEST_CASE("flute is unconditionally stable", "[midi][synth][flute]") {
  // Across the keyboard and dynamics the air-jet loop must stay bounded and
  // finite (self-oscillating but self-limiting through the jet cubic).
  for (uint8_t note : {48, 60, 72, 84, 96}) {
    for (uint8_t velocity : {40, 100, 127}) {
      const std::vector<float> tone = render_patch(flute_base_patch(), note, velocity, 48000);
      REQUIRE(peak(tone) < 4.0f);
      REQUIRE(std::isfinite(tone.back()));
    }
  }
}

TEST_CASE("flute is stable at parameter extremes", "[midi][synth][flute]") {
  // Brightest / least-damped / highest-reflection / hardest-blown corner is the
  // hardest to hold bounded; the pump-bounded loop must not run away.
  for (float bright : {0.0f, 1.0f}) {
    for (float refl : {0.2f, 1.0f}) {
      for (float breath : {0.1f, 1.0f}) {
        NativeSynthPatch patch = flute_base_patch();
        patch.flute.brightness = bright;
        patch.flute.damping = 0.0f;
        patch.flute.jet_reflection = refl;
        patch.flute.end_reflection = refl;
        patch.flute.breath_pressure = breath;
        const std::vector<float> tone = render_patch(patch, 72, 120, 48000);
        REQUIRE(peak(tone) < 4.0f);
        REQUIRE(std::isfinite(tone.back()));
      }
    }
  }
}

TEST_CASE("flute tuning is accurate", "[midi][synth][flute]") {
  // The jet locks the first register; the played fundamental lands within a
  // couple of percent across the flute range.
  for (uint8_t note : {55, 60, 67, 72, 79, 84, 91}) {
    const double f0 = note_hz(note);
    const std::vector<float> tone = render_patch(flute_base_patch(), note, 100, 24000);
    const double measured = fft_fundamental(tone, 12000, f0);
    const double cents = 1200.0 * std::log2(measured / f0);
    REQUIRE(std::fabs(cents) < 40.0);
  }
}

TEST_CASE("flute sustains a steady tone", "[midi][synth][flute]") {
  // An air-jet flute is a driven, sustained oscillator: the note holds at a
  // steady level rather than decaying like a plucked string.
  const std::vector<float> tone = render_patch(flute_base_patch(), 72, 100, 48000);
  const float early = rms(tone, 8000, 16000);
  const float late = rms(tone, 36000, 44000);
  REQUIRE(early > 0.01f);
  REQUIRE(late > 0.5f * early);
}

TEST_CASE("flute voices the octave-rich open-pipe spectrum", "[midi][synth][flute]") {
  // A flute is open at both ends (full harmonic series) and its asymmetric jet
  // drive voices a PROMINENT octave (2nd harmonic) — the open-flue-pipe colour,
  // unlike the odd-only clarinet. Assert the octave carries real energy and the
  // 3rd harmonic is present too (not a bare sine, not odd-only).
  const uint8_t note = 67;
  const double f0 = note_hz(note);
  const std::vector<float> tone = render_patch(flute_base_patch(), note, 100, 32000);
  const std::vector<double> ps = power_spectrum(tone, 16000);
  const double h1 = harmonic_power(ps, f0, 1);
  const double h2 = harmonic_power(ps, f0, 2);
  const double h3 = harmonic_power(ps, f0, 3);
  REQUIRE(h1 > 0.0);
  // The octave is a substantial partial (amplitude at least ~10% of the
  // fundamental => power at least ~1%).
  REQUIRE(h2 > 0.01 * h1);
  // A full (not odd-only) series: the 3rd harmonic is present as well.
  REQUIRE(h3 > 0.0002 * h1);
  // The fundamental still dominates (a flute is not a rich reed).
  REQUIRE(h1 > h2);
}

TEST_CASE("flute responds to live breath and brightness CCs", "[midi][synth][flute]") {
  // CC2 (breath) and CC74 (reflection brightness) colour the sounding note; the
  // tone after the controller move must differ from the untouched tone.
  const NativeSynthPatch patch = flute_base_patch();
  const std::vector<float> ref = render_patch(patch, 72, 100, 24000);
  for (uint8_t cc : {uint8_t{2}, uint8_t{74}}) {
    const std::vector<float> moved = render_cc_change(patch, 72, 100, 8000, 16000, cc, 10);
    // The post-move region differs from the same region of the untouched note.
    double diff = 0.0;
    for (size_t i = 12000; i < 24000 && i < moved.size(); ++i) {
      diff += std::fabs(static_cast<double>(moved[i]) - ref[i]);
    }
    REQUIRE(diff > 1.0);
    REQUIRE(std::isfinite(moved.back()));
  }
}

TEST_CASE("flute rings down after note-off", "[midi][synth][flute]") {
  // Stopping the breath cuts the jet drive; the bore rings down to near silence.
  const std::vector<float> tone = render_patch(flute_base_patch(), 72, 100, 48000, 20000);
  const float sounding = rms(tone, 12000, 20000);
  const float after = rms(tone, 40000, 48000);
  REQUIRE(sounding > 0.01f);
  REQUIRE(after < 0.25f * sounding);
}

TEST_CASE("flute advanced-physics gates are off by default (bit-identical)",
          "[midi][synth][flute]") {
  // The Phase-4 gates (overblow / jet turbulence / edge hysteresis / vortex) all
  // default to 0 and are skipped entirely, so a base flute renders exactly the
  // same whether or not the gate fields exist (the base patch leaves them 0).
  const std::vector<float> base = render_patch(flute_base_patch(), 72, 100, 16384);
  NativeSynthPatch zeroed = flute_base_patch();
  zeroed.flute.overblow = 0.0f;
  zeroed.flute.jet_turbulence = 0.0f;
  zeroed.flute.edge_hysteresis = 0.0f;
  zeroed.flute.vortex = 0.0f;
  const std::vector<float> also = render_patch(zeroed, 72, 100, 16384);
  REQUIRE(base == also);
}

TEST_CASE("flute advanced-physics gates change the tone and stay bounded", "[midi][synth][flute]") {
  // Each gate on must alter the sounding tone (proving it is wired) and keep the
  // loop bounded / finite; all gates on together must also stay bounded.
  const std::vector<float> base = render_patch(flute_base_patch(), 72, 100, 24000);
  const float base_rms = rms(base, 12000, 24000);
  struct Gate {
    const char* name;
    float FlutePatchParams::*field;
  };
  const Gate gates[] = {
      {"overblow", &FlutePatchParams::overblow},
      {"jet_turbulence", &FlutePatchParams::jet_turbulence},
      {"edge_hysteresis", &FlutePatchParams::edge_hysteresis},
      {"vortex", &FlutePatchParams::vortex},
  };
  for (const Gate& g : gates) {
    NativeSynthPatch patch = flute_base_patch();
    patch.flute.*(g.field) = 1.0f;
    const std::vector<float> tone = render_patch(patch, 72, 100, 24000);
    REQUIRE(peak(tone) < 4.0f);
    REQUIRE(std::isfinite(tone.back()));
    REQUIRE(std::fabs(rms(tone, 12000, 24000) - base_rms) > 1e-6f);
  }
  // All gates on simultaneously across the keyboard: still bounded.
  for (uint8_t note : {48, 72, 96}) {
    NativeSynthPatch patch = flute_base_patch();
    patch.flute.overblow = 1.0f;
    patch.flute.jet_turbulence = 1.0f;
    patch.flute.edge_hysteresis = 1.0f;
    patch.flute.vortex = 1.0f;
    const std::vector<float> tone = render_patch(patch, note, 120, 48000);
    REQUIRE(peak(tone) < 4.0f);
    REQUIRE(std::isfinite(tone.back()));
  }
}
