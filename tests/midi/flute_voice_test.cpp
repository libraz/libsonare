/// @file flute_voice_test.cpp
/// @brief Air-jet flute waveguide (midi/synth/flute_voice): fundamental tuning
///        (the jet locking the first register), the octave-rich open-flue-pipe
///        spectrum (a flute radiates a prominent 2nd harmonic, unlike the odd-
///        only clarinet), prompt speech + steady sustain, note-off ring-down,
///        unconditional stability across the keyboard, dynamics and parameter
///        extremes, and deterministic rendering.

#include "midi/synth/flute_voice.h"

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <sstream>
#include <vector>

#include "core/fft.h"
#include "midi/midi_event.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/pitch.h"
#include "midi/ump.h"
#include "support/audio_fixtures.h"
#include "support/golden_hash.h"
#include "support/midi_render.h"
#include "util/constants.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::Velocity16;
using sonare::midi::synth::FlutePatchParams;
using sonare::midi::synth::FluteVoiceCore;
using sonare::midi::synth::gm_fallback_patch;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::SynthEngineMode;

using sonare::test::event;
using sonare::test::kFft;
using sonare::test::kRate;
using sonare::test::render_left;

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

/// Sends @p cc = @p value BEFORE the note-on (so the note is seeded at that
/// controller position), then renders @p num samples.
std::vector<float> render_with_initial_cc(const NativeSynthPatch& patch, uint8_t note,
                                          uint8_t velocity, int num, uint8_t cc, uint8_t value) {
  NativeSynthConfig cfg;
  cfg.patch = patch;
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, cc, value)));
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, velocity)));
  return render_left(synth, num);
}

/// Loudest 10 ms of the first 150 ms over the 0.2-0.4 s held level, in dB.
double onset_lift_db(const std::vector<float>& buf) {
  const size_t win = static_cast<size_t>(0.010 * kRate);
  double top = 0.0;
  for (size_t i = 0; i + win <= static_cast<size_t>(0.150 * kRate); i += win / 2) {
    top = std::max(top, static_cast<double>(rms(buf, i, i + win)));
  }
  const double held = rms(buf, static_cast<size_t>(0.2 * kRate), static_cast<size_t>(0.4 * kRate));
  return 20.0 * std::log10(top / held);
}

using sonare::test::fft_fundamental;
using sonare::test::harmonic_power;
using sonare::test::power_spectrum;

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

TEST_CASE("flute kill silences subsequent rendering and permits restart", "[midi][synth][flute]") {
  FlutePatchParams params;
  params.breath_noise = 0.0f;
  params.chiff = 0.0f;
  const double sample_rate = 48000.0;
  const int capacity = sonare::midi::synth::flute_buffer_capacity(sample_rate);
  std::vector<float> slab(
      static_cast<size_t>(sonare::midi::synth::flute_slab_capacity(sample_rate)));
  FluteVoiceCore core;
  core.attach(slab.data(), capacity);
  core.start(params, sample_rate, 72, Velocity16::from7(110), 0x4b494c4cULL);

  std::vector<float> sounding;
  sounding.reserve(12000);
  for (int i = 0; i < 12000; ++i) sounding.push_back(core.render(1.0f));
  REQUIRE(peak(sounding) > 0.01f);

  core.kill();
  bool silent = true;
  for (int i = 0; i < 4 * capacity; ++i) silent = (core.render(1.0f) == 0.0f) && silent;
  REQUIRE(silent);

  core.start(params, sample_rate, 72, Velocity16::from7(110), 0x4b494c4dULL);
  std::vector<float> restarted;
  restarted.reserve(12000);
  for (int i = 0; i < 12000; ++i) restarted.push_back(core.render(1.0f));
  REQUIRE(peak(restarted) > 0.01f);
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
    // Against the difference of the two renders, not the difference of their
    // levels: a gate that reshapes the tone without moving its energy is still
    // wired, and one whose level reading happens to land on the base's is not
    // evidence that it is not. The four measure 0.9 % to 112 % of the base's own
    // level here, so the bound sits an order under the weakest of them.
    std::vector<float> delta(tone.size());
    for (size_t i = 0; i < tone.size(); ++i) delta[i] = tone[i] - base[i];
    REQUIRE(rms(delta, 12000, 24000) > 1.0e-5f);
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

TEST_CASE("flute onset overshoot is off by default (bit-identical)", "[midi][synth][flute]") {
  // At 0 dB the settle and the velocity slope have nothing to shape, so neither
  // reaches the render.
  NativeSynthPatch base = gm_fallback_patch(0, 73);  // Flute
  base.flute.onset_breath_overshoot_db = 0.0f;
  NativeSynthPatch same = base;
  same.flute.onset_breath_settle_ms = 120.0f;
  same.flute.onset_breath_overshoot_vel_db = 4.0f;
  REQUIRE(render_patch(base, 72, 100, 24000) == render_patch(same, 72, 100, 24000));
}

TEST_CASE("a flute onset overshoot lifts the onset, more at ff when it slopes with velocity",
          "[midi][synth][flute]") {
  NativeSynthPatch off = gm_fallback_patch(0, 73);  // Flute
  off.flute.onset_breath_overshoot_db = 0.0f;
  // The patch's slow VCA attack would hide the onset the contour shapes.
  off.amp_env.attack_ms = 5.0f;
  NativeSynthPatch on = off;
  // The jet takes ~30 ms to speak, so the overshoot has to outlast it to be heard.
  on.flute.onset_breath_overshoot_db = 6.0f;
  on.flute.onset_breath_settle_ms = 150.0f;
  on.flute.onset_breath_overshoot_vel_db = 3.0f;
  const int n = static_cast<int>(0.45 * kRate);
  const double soft_gain =
      onset_lift_db(render_patch(on, 72, 32, n)) - onset_lift_db(render_patch(off, 72, 32, n));
  const double hard_gain =
      onset_lift_db(render_patch(on, 72, 127, n)) - onset_lift_db(render_patch(off, 72, 127, n));
  INFO("onset lift added at velocity 32: " << soft_gain << " dB, at 127: " << hard_gain << " dB");
  REQUIRE(hard_gain > 0.5);
  REQUIRE(hard_gain > soft_gain);
}

TEST_CASE("a streamed breath holds the flute onset overshoot", "[midi][synth][flute]") {
  // CC2 seeds the force axis before the note, so the player's breath is the
  // onset and the synthesized tonguing must not play over it.
  NativeSynthPatch off = gm_fallback_patch(0, 73);  // Flute
  off.flute.onset_breath_overshoot_db = 0.0f;
  NativeSynthPatch on = off;
  on.flute.onset_breath_overshoot_db = 6.0f;
  REQUIRE(render_with_initial_cc(on, 72, 100, 24000, 2, 90) ==
          render_with_initial_cc(off, 72, 100, 24000, 2, 90));
  REQUIRE(render_patch(on, 72, 100, 24000) != render_patch(off, 72, 100, 24000));
}

TEST_CASE("flute control hashes", "[.][midi][synth][flute][golden]") {
  // The eight GM flute programs and a breath / brightness gesture, rendered with
  // jet_drive off: the jet-drive field off must leave the engine as it was.
  INFO(sonare::test::kGoldenDigestProvenance);
  struct Row {
    uint8_t program;
    uint8_t note;
    uint8_t velocity;
    uint64_t hash;
  };
  static const Row kRows[] = {
      {72, 84, 100, 0xdc3da02b4d3e1d9bull}, {73, 72, 100, 0x3a5a3e055be6710eull},
      {74, 72, 64, 0x010480eb1f7c427dull},  {75, 67, 127, 0xe29548e5b0492a89ull},
      {76, 72, 100, 0x14bf03092c01eeb2ull}, {77, 60, 90, 0x071157f2771744e0ull},
      {78, 79, 110, 0x7be496dbd0f77b43ull}, {79, 72, 80, 0xe17e889d1d3bf5d4ull},
  };
  for (const Row& r : kRows) {
    const NativeSynthPatch& patch = gm_fallback_patch(0, r.program);
    REQUIRE(patch.mode == SynthEngineMode::kFlute);
    REQUIRE(!patch.flute.jet_drive);
    INFO("program " << int(r.program));
    CHECK(sonare::test::fnv1a_quantized(render_patch(patch, r.note, r.velocity, 24000)) == r.hash);
  }
  const NativeSynthPatch& flute = gm_fallback_patch(0, 73);
  CHECK(sonare::test::fnv1a_quantized(render_cc_change(flute, 72, 100, 8000, 16000, 2, 20)) ==
        0xf8cacd76cc06b9a5ull);
  CHECK(sonare::test::fnv1a_quantized(render_cc_change(flute, 72, 100, 8000, 16000, 74, 110)) ==
        0x1adfcf07f7e620a0ull);
  CHECK(sonare::test::fnv1a_quantized(render_patch(flute_base_patch(), 60, 100, 24000)) ==
        0x5f90049eacc9ed68ull);
}

namespace {

/// Held level in dBFS over 0.4-0.8 s of a 0.8 s render.
double held_db(const std::vector<float>& tone) {
  const float level = rms(tone, static_cast<size_t>(0.4 * kRate), static_cast<size_t>(0.8 * kRate));
  return 20.0 * std::log10(std::max(1.0e-12, static_cast<double>(level)));
}

/// Harmonic @p k over the fundamental (dB) of a held tone sounding @p f0.
double harmonic_db(const std::vector<float>& tone, double f0, int k) {
  const std::vector<double> ps = power_spectrum(tone, static_cast<size_t>(0.5 * kRate));
  return 10.0 * std::log10(std::max(1.0e-30, harmonic_power(ps, f0, k)) /
                           std::max(1.0e-30, harmonic_power(ps, f0, 1)));
}

/// Sounds the played fundamental: above -60 dBFS, within @p max_cents, and neither
/// the octave nor the twelfth outweighs it (a register jump or a squeal reads as false).
bool speaks_at(const std::vector<float>& tone, uint8_t note, double max_cents = 50.0) {
  if (held_db(tone) < -60.0) return false;
  const double f = fft_fundamental(tone, static_cast<size_t>(0.5 * kRate), note_hz(note));
  const double cents = 1200.0 * std::log2(f / note_hz(note));
  return std::fabs(cents) < max_cents && harmonic_db(tone, f, 2) < 0.0 &&
         harmonic_db(tone, f, 3) < 0.0;
}

/// Jet-drive flute whose breath is the patch's own, so the jet delay sits at
/// jet_ratio of the period.
NativeSynthPatch jet_drive_patch() {
  NativeSynthPatch p = flute_base_patch();
  p.flute.jet_drive = true;
  p.flute.jet_offset = 0.15f;
  p.flute.vel_to_breath = 0.0f;
  p.flute.breath_noise = 0.0f;
  p.flute.chiff = 0.0f;
  return p;
}

}  // namespace

TEST_CASE("flute jet-drive held level follows the breath", "[midi][synth][flute]") {
  // Above the jet-drive threshold at its shipped gain.
  constexpr float kSoftBreath = 0.4f;
  // The cubic jet settles where its loop gain reaches one, so its held level does
  // not move with the breath; the jet-drive source scales with the jet speed.
  const int n = static_cast<int>(0.8 * kRate);
  double spread[2] = {0.0, 0.0};
  for (int path = 0; path < 2; ++path) {
    double db[2] = {0.0, 0.0};
    for (int i = 0; i < 2; ++i) {
      NativeSynthPatch patch = path == 0 ? flute_base_patch() : jet_drive_patch();
      patch.flute.vel_to_breath = 0.0f;
      patch.flute.breath_pressure = i == 0 ? kSoftBreath : 0.9f;
      const std::vector<float> tone = render_patch(patch, 72, 100, n);
      INFO("path " << path << " breath " << patch.flute.breath_pressure);
      CHECK(speaks_at(tone, 72));
      db[i] = held_db(tone);
    }
    spread[path] = db[1] - db[0];
    INFO("path " << path << ": breath " << kSoftBreath << " " << db[0] << " dBFS, 0.9 " << db[1]
                 << " dBFS");
    CHECK(std::isfinite(spread[path]));
  }
  INFO("cubic spread " << spread[0] << " dB, jet-drive spread " << spread[1] << " dB");
  CHECK(std::fabs(spread[0]) < 1.0);
  // Uj ~ sqrt(P) alone scales the source by 3.5 dB between the two breaths.
  CHECK(spread[1] > 3.0);
}

TEST_CASE("flute jet-drive delay shortens as the breath rises", "[midi][synth][flute]") {
  FlutePatchParams params;
  params.breath_noise = 0.0f;
  params.chiff = 0.0f;
  params.breath_pressure = 0.5f;
  params.vel_to_breath = 1.0f;
  const double sample_rate = 48000.0;
  const int capacity = sonare::midi::synth::flute_buffer_capacity(sample_rate);
  std::vector<float> slab(
      static_cast<size_t>(sonare::midi::synth::flute_slab_capacity(sample_rate)));
  const auto held_delay = [&](bool jet_drive, uint8_t velocity) {
    params.jet_drive = jet_drive;
    FluteVoiceCore core;
    core.attach(slab.data(), capacity);
    core.start(params, sample_rate, 72, Velocity16::from7(velocity), 0x4a455444ULL);
    for (int i = 0; i < 12000; ++i) core.render(1.0f);
    return core.jet_delay_samples();
  };
  // The cubic jet's delay is the jet ratio of the period whatever the breath.
  CHECK(held_delay(false, 40) == held_delay(false, 120));
  const float soft = held_delay(true, 40);
  const float hard = held_delay(true, 120);
  INFO("jet delay at velocity 40: " << soft << " samples, at 120: " << hard);
  // Uj ~ sqrt(P), so the delay goes as the inverse root of the breath.
  CHECK(hard / soft == Catch::Approx(std::sqrt(40.0 / 120.0)).epsilon(0.01));
}

TEST_CASE("flute jet offset voices the even harmonics", "[midi][synth][flute]") {
  // A centred jet splits the flow symmetrically and its source is odd; an offset
  // jet's is not.
  const uint8_t note = 67;
  const int n = static_cast<int>(0.8 * kRate);
  NativeSynthPatch centred = jet_drive_patch();
  centred.flute.jet_offset = 0.0f;
  NativeSynthPatch offset = jet_drive_patch();
  offset.flute.jet_offset = 0.3f;
  const std::vector<float> c = render_patch(centred, note, 100, n);
  const std::vector<float> o = render_patch(offset, note, 100, n);
  const double fc = fft_fundamental(c, static_cast<size_t>(0.5 * kRate), note_hz(note));
  const double fo = fft_fundamental(o, static_cast<size_t>(0.5 * kRate), note_hz(note));
  CHECK(speaks_at(c, note));
  CHECK(speaks_at(o, note));
  const double h2c = harmonic_db(c, fc, 2);
  const double h2o = harmonic_db(o, fo, 2);
  INFO("h2/h1 centred " << h2c << " dB, offset 0.3 " << h2o << " dB");
  CHECK(h2o > h2c + 6.0);
}

TEST_CASE("concert flute on the jet-drive path speaks at every note and velocity",
          "[.][slow][midi][synth][flute]") {
  NativeSynthPatch patch = gm_fallback_patch(0, 73);  // Flute
  patch.flute.jet_drive = true;
  patch.flute.jet_offset = 0.15f;
  const int n = static_cast<int>(0.8 * kRate);
  for (uint8_t note = 60; note <= 96; note += 4) {
    for (uint8_t velocity : {uint8_t{32}, uint8_t{80}, uint8_t{127}}) {
      const std::vector<float> tone = render_patch(patch, note, velocity, n);
      const double f = fft_fundamental(tone, static_cast<size_t>(0.5 * kRate), note_hz(note));
      INFO("note " << int(note) << " velocity " << int(velocity) << ": "
                   << 1200.0 * std::log2(f / note_hz(note)) << " cents, " << held_db(tone)
                   << " dBFS");
      // The jet delay's breath law pulls v32 about 75 cents flat and v127 about 60 sharp.
      CHECK(speaks_at(tone, note, 100.0));
    }
  }
}

TEST_CASE("flute tone-hole lattice is off by default (bit-identical)", "[midi][synth][flute]") {
  // At 0 Hz the lattice sections are never built and the loop compensation gains an exact zero.
  const std::vector<float> base = render_patch(flute_base_patch(), 84, 100, 16384);
  NativeSynthPatch zeroed = flute_base_patch();
  zeroed.flute.lattice_cutoff_hz = 0.0f;
  REQUIRE(render_patch(zeroed, 84, 100, 16384) == base);
}

TEST_CASE("flute tone-hole lattice cutoff withdraws the bore from the partials above it",
          "[midi][synth][flute]") {
  // One corner for every note. C6's twelfth (3140 Hz) sits above an 1800 Hz lattice and loses the
  // bore's regeneration on both jet paths; its octave (2093 Hz) falls only on the cubic path, since
  // the jet-drive source remakes the even harmonics from its offset every cycle and the bore's
  // passive gain at 2f0 is all the lattice can take away. C4's ladder up to h6 (1570 Hz) sits
  // below the corner and keeps its octave. The section's phase delay at f0 comes off the bore, so
  // neither note moves in pitch.
  constexpr float kCutoffHz = 1800.0f;
  const int n = static_cast<int>(0.8 * kRate);
  struct Ladder {
    double cents;
    double h2;
    double h3;
  };
  const auto measure = [&](const NativeSynthPatch& patch, uint8_t note) {
    const std::vector<float> tone = render_patch(patch, note, 100, n);
    REQUIRE(speaks_at(tone, note));
    const double f = fft_fundamental(tone, static_cast<size_t>(0.5 * kRate), note_hz(note));
    return Ladder{1200.0 * std::log2(f / note_hz(note)), harmonic_db(tone, f, 2),
                  harmonic_db(tone, f, 3)};
  };
  for (int path = 0; path < 2; ++path) {
    const NativeSynthPatch off = path == 0 ? flute_base_patch() : jet_drive_patch();
    NativeSynthPatch on = off;
    on.flute.lattice_cutoff_hz = kCutoffHz;
    const Ladder top_off = measure(off, 84);
    const Ladder top_on = measure(on, 84);
    INFO("path " << path << " C6 h2/h1 " << top_off.h2 << " -> " << top_on.h2 << " dB, h3/h1 "
                 << top_off.h3 << " -> " << top_on.h3 << " dB, f0 " << top_off.cents << " -> "
                 << top_on.cents << " c");
    CHECK(top_on.h3 < top_off.h3 - 2.0);
    if (path == 0) CHECK(top_on.h2 < top_off.h2 - 3.0);
    CHECK(std::fabs(top_on.cents - top_off.cents) < 10.0);
    const Ladder low_off = measure(off, 60);
    const Ladder low_on = measure(on, 60);
    INFO("path " << path << " C4 h2/h1 " << low_off.h2 << " -> " << low_on.h2 << " dB, f0 "
                 << low_off.cents << " -> " << low_on.cents << " c");
    if (path == 0) CHECK(std::fabs(low_on.h2 - low_off.h2) < 2.0);
    CHECK(std::fabs(low_on.cents - low_off.cents) < 10.0);
  }
}
