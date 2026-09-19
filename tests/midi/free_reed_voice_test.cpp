/// @file free_reed_voice_test.cpp
/// @brief Free-reed slot flow (midi/synth/free_reed_voice): the shipped saw
///        shaper stays reachable and bit-identical, the slot flow lifts the
///        second partial over the first where the saw cannot, its duty places
///        the null the measured references carry, it leaves no steady flow
///        under the note, and it stays bounded across the keyboard.

#include "midi/synth/free_reed_voice.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"
#include "support/audio_fixtures.h"
#include "support/midi_render.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::SynthEngineMode;

using sonare::test::event;
using sonare::test::harmonic_power;
using sonare::test::kFft;
using sonare::test::kRate;
using sonare::test::power_spectrum;

std::vector<float> render_patch(const NativeSynthPatch& patch, uint8_t note, uint8_t velocity,
                                int num_samples) {
  NativeSynthConfig cfg;
  cfg.patch = patch;
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, velocity)));
  std::vector<float> left(static_cast<size_t>(num_samples), 0.0f);
  std::vector<float> right(static_cast<size_t>(num_samples), 0.0f);
  float* chans[2] = {left.data(), right.data()};
  synth.process(chans, 2, num_samples);
  return left;
}

/// A free reed with no musette pair, so one note gives one clean ladder.
NativeSynthPatch free_reed_patch() {
  NativeSynthPatch patch{};
  patch.mode = SynthEngineMode::kFreeReed;
  patch.amp_env.attack_ms = 5.0f;
  patch.amp_env.sustain = 1.0f;
  patch.amp_env.release_ms = 120.0f;
  patch.cutoff_hz = 20000.0f;
  patch.gain = 0.8f;
  patch.free_reed.brightness = 0.85f;
  patch.free_reed.detune = 0.0f;
  patch.free_reed.breath_noise = 0.0f;
  return patch;
}

float peak(const std::vector<float>& buf) {
  float p = 0.0f;
  for (float s : buf) p = std::max(p, std::fabs(s));
  return p;
}

bool all_finite(const std::vector<float>& buf) {
  for (float s : buf) {
    if (!std::isfinite(s)) return false;
  }
  return true;
}

double note_hz(uint8_t note) {
  return 440.0 * std::pow(2.0, (static_cast<double>(note) - 69.0) / 12.0);
}

}  // namespace

TEST_CASE("the free reed's slot flow is off by default", "[midi][synth][free_reed]") {
  NativeSynthPatch plain = free_reed_patch();
  NativeSynthPatch gated = plain;
  // Every companion field at a value that would change the sound if the slot
  // flow were reached; the zero duty is what has to keep it out.
  gated.free_reed.slot_duty = 0.0f;
  gated.free_reed.slot_return = 1.4f;
  gated.free_reed.slot_gap = 0.47f;
  gated.free_reed.radiation = 0.0f;
  REQUIRE(render_patch(plain, 60, 100, 24000) == render_patch(gated, 60, 100, 24000));
}

TEST_CASE("the free reed's slot flow lifts the second partial over the first",
          "[midi][synth][free_reed]") {
  const uint8_t note = 59;
  const double f0 = note_hz(note);

  NativeSynthPatch saw = free_reed_patch();
  NativeSynthPatch slot = saw;
  slot.free_reed.slot_duty = 0.56f;
  slot.free_reed.slot_return = 0.8f;
  slot.free_reed.slot_gap = 0.41f;
  slot.free_reed.radiation = 1.0f;

  const std::vector<double> saw_ps = power_spectrum(render_patch(saw, note, 100, 24000), 8000);
  const std::vector<double> slot_ps = power_spectrum(render_patch(slot, note, 100, 24000), 8000);
  const double saw_ratio = harmonic_power(saw_ps, f0, 2) / harmonic_power(saw_ps, f0, 1);
  const double slot_ratio = harmonic_power(slot_ps, f0, 2) / harmonic_power(slot_ps, f0, 1);

  // The saw shaper's ladder falls from the fundamental whatever its knobs do;
  // the references' second partial sits some 8 dB above theirs, and only the
  // radiated slot flow gets there.
  REQUIRE(saw_ratio < 1.0);
  REQUIRE(slot_ratio > 2.0);
}

TEST_CASE("the free reed's slot duty places the null", "[midi][synth][free_reed]") {
  const uint8_t note = 47;
  const double f0 = note_hz(note);

  // A flow hump of duty d nulls near the harmonic 2/d, so a narrow slot puts
  // its null high and a wide one puts it low. The references' nulls are what
  // the duty was fitted to.
  NativeSynthPatch narrow = free_reed_patch();
  narrow.free_reed.slot_duty = 0.29f;
  narrow.free_reed.slot_return = 0.0f;
  narrow.free_reed.radiation = 1.0f;
  NativeSynthPatch wide = narrow;
  wide.free_reed.slot_duty = 0.56f;

  const std::vector<double> narrow_ps =
      power_spectrum(render_patch(narrow, note, 100, 24000), 8000);
  const std::vector<double> wide_ps = power_spectrum(render_patch(wide, note, 100, 24000), 8000);

  const auto deepest = [&](const std::vector<double>& ps) {
    int best = 2;
    double best_ratio = 1e30;
    for (int k = 2; k <= 10; ++k) {
      const double r = harmonic_power(ps, f0, k) /
                       (harmonic_power(ps, f0, k - 1) + harmonic_power(ps, f0, k + 1) + 1e-30);
      if (r < best_ratio) {
        best_ratio = r;
        best = k;
      }
    }
    return best;
  };
  REQUIRE(deepest(narrow_ps) > deepest(wide_ps));
}

TEST_CASE("the free reed's slot flow leaves no steady flow under the note",
          "[midi][synth][free_reed]") {
  for (uint8_t note : {41, 59, 77}) {
    const double f0 = note_hz(note);
    NativeSynthPatch patch = free_reed_patch();
    patch.free_reed.slot_duty = 0.56f;
    patch.free_reed.slot_return = 0.8f;
    patch.free_reed.slot_gap = 0.41f;
    // Radiating differentiates the flow and would remove the steady part on its
    // own; at zero it is the closed-form mean that has to.
    patch.free_reed.radiation = 0.0f;

    const std::vector<double> ps = power_spectrum(render_patch(patch, note, 100, 24000), 8000);
    const int top = static_cast<int>(std::lround(0.8 * f0 / kRate * kFft));
    double below = 0.0;
    for (int b = 1; b < top && b < static_cast<int>(ps.size()); ++b) {
      below += ps[static_cast<size_t>(b)];
    }
    REQUIRE(below < 0.02 * harmonic_power(ps, f0, 1));
  }
}

TEST_CASE("the free reed's slot flow stays bounded across the keyboard",
          "[midi][synth][free_reed]") {
  for (uint8_t note : {28, 46, 64, 82}) {
    for (float duty : {0.1f, 0.29f, 0.56f, 0.9f}) {
      for (float ret : {0.0f, 0.4f, 1.5f}) {
        for (float radiation : {0.0f, 0.6f, 1.0f}) {
          NativeSynthPatch patch = free_reed_patch();
          patch.free_reed.slot_duty = duty;
          patch.free_reed.slot_return = ret;
          patch.free_reed.radiation = radiation;
          const std::vector<float> tone = render_patch(patch, note, 120, 12000);
          REQUIRE(all_finite(tone));
          // The differentiator's normalisation is what holds the low notes in:
          // it divides by 2*sin(pi*f0/sr), which at 41 Hz is a gain of 186.
          REQUIRE(peak(tone) < 4.0f);
        }
      }
    }
  }
}
