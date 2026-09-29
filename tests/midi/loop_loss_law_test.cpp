/// @file loop_loss_law_test.cpp
/// @brief The shared waveguide loop-loss law (string_loop.h's
///        loss_pole_at_rate): reproduction of each of the four calling
///        engines' own shipped coefficient, the pole's correct sr-only (not
///        sr/f0) grouping, sample-rate invariance of the resulting Hz-domain
///        shape, and -- the centrepiece -- that none of the four call sites
///        lets the note back into the mapping.
///
/// The retired law read one note's own decay TIME as a target and re-solved
/// the pole at every other note from it, so an instrument's single physical
/// corner swung across the register (the flute's bell: 1681 Hz at note 60,
/// 4598 Hz at note 90, against a single measured 3291 Hz corner), moving 21 of
/// 28 gated voices away from their references. The new law is one function of
/// the sample rate alone; a note term reappearing at any of the four call
/// sites is what the note-invariance cases below are built to catch.
///
/// What this law does not reach in the flute -- its jet delay and its seeded
/// noise levels -- is wind_rate_law_test.cpp's.

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <vector>

#include "midi/bowed_string_probe.h"
#include "midi/midi_event.h"
#include "midi/synth/bowed_string_voice.h"
#include "midi/synth/flute_voice.h"
#include "midi/synth/gm_fallback_map.h"
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

using sonare::constants::kTwoPi;
using sonare::constants::kTwoPiD;
using sonare::midi::synth::bowed_string_buffer_capacity;
using sonare::midi::synth::bowed_string_slab_capacity;
using sonare::midi::synth::BowedStringPatchParams;
using sonare::midi::synth::BowedStringVoiceCore;
using sonare::midi::synth::flute_buffer_capacity;
using sonare::midi::synth::flute_slab_capacity;
using sonare::midi::synth::FlutePatchParams;
using sonare::midi::synth::FluteVoiceCore;
using sonare::midi::synth::gm_fallback_patch;
using sonare::midi::synth::kLossVoicedSr;
using sonare::midi::synth::ks_buffer_capacity;
using sonare::midi::synth::ks_slab_capacity;
using sonare::midi::synth::KsPatchParams;
using sonare::midi::synth::KsVoiceCore;
using sonare::midi::synth::loss_pole_at_rate;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::note_to_hz;
using sonare::midi::synth::onepole_magnitude;
using sonare::midi::synth::plucked_string_buffer_capacity;
using sonare::midi::synth::plucked_string_slab_capacity;
using sonare::midi::synth::PluckedStringPatchParams;
using sonare::midi::synth::PluckedStringVoiceCore;
using sonare::midi::synth::reed_buffer_capacity;
using sonare::midi::synth::reed_slab_capacity;
using sonare::midi::synth::ReedPatchParams;
using sonare::midi::synth::ReedVoiceCore;
using sonare::midi::synth::SynthEngineMode;
using sonare::test::event;
using sonare::test::bowed::BandTilt;
using sonare::test::bowed::octave_band_tilt;

// --- Each engine's own a_voiced formula, duplicated ---
//
// refresh_excitation_targets() (flute, reed) and start() (bowed_string,
// plucked_string) keep the mapping private, so it is reproduced here from the
// patch field rather than parsed out of the engine.

float flute_a_voiced(float brightness) noexcept {
  return std::clamp(0.80f - 0.30f * std::clamp(brightness, 0.0f, 1.0f), 0.0f, 0.95f);
}
float bowed_a_voiced(float brightness) noexcept {
  return (1.0f - std::clamp(brightness, 0.0f, 1.0f)) * 0.7f;
}
float plucked_a_voiced(float brightness) noexcept {
  return (1.0f - std::clamp(brightness, 0.0f, 1.0f)) * 0.7f;
}
// reed_voice.cpp's own kBellPoleSpan, duplicated: it is a SONARE_TUNABLE with
// internal linkage, so a re-fit that moves it must also update this literal.
constexpr float kReedBellPoleSpan = 0.7f;
float reed_a_voiced(float brightness) noexcept {
  return (1.0f - std::clamp(brightness, 0.0f, 1.0f)) * kReedBellPoleSpan;
}

/// One (engine, shipped brightness) pair, read straight off a real patch.
struct ShippedVoice {
  const char* name;
  float (*a_voiced)(float) noexcept;
  float brightness;
};

/// The shipped bank, one representative brightness per engine plus enough
/// siblings to span each engine's own range -- not an anchor note, since the
/// new law takes none, but a spread of real patches.
std::vector<ShippedVoice> shipped_voices() {
  return {
      {"flute.piccolo", flute_a_voiced, gm_fallback_patch(0, 72).flute.brightness},
      {"flute.concert_flute", flute_a_voiced, gm_fallback_patch(0, 73).flute.brightness},
      {"flute.shakuhachi", flute_a_voiced, gm_fallback_patch(0, 77).flute.brightness},
      {"flute.ocarina", flute_a_voiced, gm_fallback_patch(0, 79).flute.brightness},
      {"bowed_string.viola", bowed_a_voiced, gm_fallback_patch(0, 41).bowed_string.brightness},
      {"bowed_string.cello", bowed_a_voiced, gm_fallback_patch(0, 42).bowed_string.brightness},
      {"plucked_string.sitar", plucked_a_voiced,
       gm_fallback_patch(0, 104).plucked_string.brightness},
      {"plucked_string.koto", plucked_a_voiced,
       gm_fallback_patch(0, 107).plucked_string.brightness},
      {"reed.oboe", reed_a_voiced, gm_fallback_patch(0, 68).reed.brightness},
      {"reed.bassoon", reed_a_voiced, gm_fallback_patch(0, 70).reed.brightness},
  };
}

// --- Rendered-engine helpers: dark (brightness = 0, the strongest pole for
// every engine) patches chosen for measurement power, not shipped values,
// exactly as plucked_string_loss_law_test.cpp's retired dark patch was. ---

std::vector<float> render_flute_dark(uint8_t note, double sr, int samples) {
  FlutePatchParams p;
  p.jet_ratio = 0.5f;
  p.jet_reflection = 0.5f;
  p.end_reflection = 0.5f;
  p.brightness = 0.0f;
  p.damping = 0.0f;
  p.breath_pressure = 0.7f;
  p.vel_to_breath = 0.0f;
  p.breath_noise = 0.0f;
  p.chiff = 0.0f;
  p.vibrato_depth = 0.0f;
  FluteVoiceCore core;
  const int per_span = flute_buffer_capacity(sr);
  std::vector<float> slab(static_cast<size_t>(flute_slab_capacity(sr)), 0.0f);
  core.attach(slab.data(), per_span);
  core.start(p, sr, note, sonare::midi::Velocity16::from7(100), 0x5011ADE5ull);
  std::vector<float> out(static_cast<size_t>(samples), 0.0f);
  for (int i = 0; i < samples; ++i) out[static_cast<size_t>(i)] = core.render(1.0f);
  return out;
}

std::vector<float> render_bowed_dark(uint8_t note, double sr, int samples) {
  BowedStringPatchParams p;
  p.bow_position = 0.13f;
  p.bow_force = 0.5f;
  p.bow_speed = 0.7f;
  p.vel_to_speed = 0.0f;
  p.brightness = 0.0f;
  p.damping = 0.4f;
  p.rosin = 0.0f;
  BowedStringVoiceCore core;
  const int per_line = bowed_string_buffer_capacity(sr);
  std::vector<float> slab(static_cast<size_t>(bowed_string_slab_capacity(sr)), 0.0f);
  core.attach(slab.data(), per_line);
  core.start(p, sr, note, sonare::midi::Velocity16::from7(100), 0x5011ADE5ull);
  std::vector<float> out(static_cast<size_t>(samples), 0.0f);
  for (int i = 0; i < samples; ++i) out[static_cast<size_t>(i)] = core.render(1.0f);
  return out;
}

std::vector<float> render_plucked_dark(uint8_t note, double sr, int samples) {
  PluckedStringPatchParams p;
  p.buzz = 0.0f;
  p.brightness = 0.0f;
  p.decay_s = 4.0f;
  p.decay_stretch = 0.5f;
  p.pick_position = 0.0f;
  PluckedStringVoiceCore core;
  const int per_line = plucked_string_buffer_capacity(sr);
  std::vector<float> slab(static_cast<size_t>(plucked_string_slab_capacity(sr)), 0.0f);
  core.attach(slab.data(), per_line);
  core.start(p, sr, note, sonare::midi::Velocity16::from7(100), 0x5011ADE5ull);
  std::vector<float> out(static_cast<size_t>(samples), 0.0f);
  for (int i = 0; i < samples; ++i) out[static_cast<size_t>(i)] = core.render(1.0f);
  return out;
}

std::vector<float> render_reed_dark(uint8_t note, double sr, int samples) {
  ReedPatchParams p;
  p.breath_pressure = 0.7f;
  p.vel_to_breath = 0.0f;
  p.conical = false;
  p.brightness = 0.0f;
  p.damping = 0.0f;
  p.breath_noise = 0.0f;
  p.chiff = 0.0f;
  ReedVoiceCore core;
  const int capacity = reed_buffer_capacity(sr);
  std::vector<float> slab(static_cast<size_t>(reed_slab_capacity(sr)), 0.0f);
  core.attach(slab.data(), capacity);
  core.start(p, sr, note, sonare::midi::Velocity16::from7(100), 0x5011ADE5ull);
  std::vector<float> out(static_cast<size_t>(samples), 0.0f);
  for (int i = 0; i < samples; ++i) out[static_cast<size_t>(i)] = core.render(1.0f);
  return out;
}

std::vector<float> render_ks(const KsPatchParams& params, uint8_t note, double sr, int samples) {
  KsVoiceCore core;
  const int per_line = ks_buffer_capacity(sr);
  std::vector<float> slab(static_cast<size_t>(ks_slab_capacity(sr)), 0.0f);
  core.attach(slab.data(), per_line);
  core.start(params, sr, note, sonare::midi::Velocity16::from7(100), 0x5011ADE5ull);
  std::vector<float> out(static_cast<size_t>(samples), 0.0f);
  for (int i = 0; i < samples; ++i) out[static_cast<size_t>(i)] = core.render(1.0f);
  return out;
}

/// A sample count that is an exact power of two at every rate in the grid, so
/// probe_fft_size's cap at 32768 never truncates one rate's window more than
/// another's: the analysis window covers the SAME real-time duration at every
/// rate, which isolating the loop filter's own effect from a window-length
/// artifact requires.
int window_samples(double sr) noexcept { return static_cast<int>(sr * 8192.0 / 48000.0); }

}  // namespace

TEST_CASE("the loss law reproduces each engine's shipped voiced-rate coefficient, at every note",
          "[midi][synth][loss_law]") {
  // std::pow(x, 1.0) returns x exactly (checked across 1001 coefficients over
  // [0, 0.95]: zero mismatches), so at sr == kLossVoicedSr the mapping is a
  // true identity rather than a close approximation -- bit equality is the
  // assertion, and a future rewrite that reintroduces any solve in place of
  // the identity has nowhere to hide a rounding excuse. The retired law only
  // reproduced its own shipped pole at ONE anchor note, so this sweeps a
  // spread of notes through the call shape each engine actually uses
  // (brightness, note, sr) even though the note is not read.
  const uint8_t notes[] = {21, 36, 48, 60, 72, 84, 96, 108};
  for (const ShippedVoice& v : shipped_voices()) {
    const float a_ship = v.a_voiced(v.brightness);
    for (uint8_t note : notes) {
      const float pole = loss_pole_at_rate(a_ship, kLossVoicedSr);
      INFO(v.name << " brightness " << v.brightness << " note " << int{note});
      CHECK(pole == a_ship);
    }
  }
}

TEST_CASE("the loss law's coefficient does not group by N", "[midi][synth][loss_law]") {
  // Two (note, sr) pairs that land on the same period N would have been
  // handed the identical pole under every one of the four retired formulas,
  // none of which read the period at all. Now they must not agree, because
  // loss_pole_at_rate is a function of sr alone.
  const float a_ship = flute_a_voiced(gm_fallback_patch(0, 73).flute.brightness);
  const double sr_a = kLossVoicedSr;        // note 48 @ 48 kHz
  const double sr_b = 2.0 * kLossVoicedSr;  // note 60 @ 96 kHz
  const float period_a = static_cast<float>(sr_a) / note_to_hz(48.0f);
  const float period_b = static_cast<float>(sr_b) / note_to_hz(60.0f);
  INFO("period_a " << period_a << " period_b " << period_b);
  REQUIRE(period_a == Catch::Approx(period_b).epsilon(1e-6));

  const float pole_a = loss_pole_at_rate(a_ship, sr_a);
  const float pole_b = loss_pole_at_rate(a_ship, sr_b);
  INFO("pole at N=" << period_a << ": 48kHz " << pole_a << ", 96kHz " << pole_b);
  CHECK(std::fabs(pole_a - pole_b) > 1e-3f);
}

TEST_CASE("the loss law's coefficient does not depend on the note", "[midi][synth][loss_law]") {
  // The centrepiece. loss_pole_at_rate takes a rate, never a note -- so this
  // is necessarily true by construction, and an engine that reintroduced a
  // note term in its OWN call site (the exact shape of the regression this
  // law replaces) would still leave this case green. That is the reason the
  // two cases below repeat the same question against the rendered engines
  // instead of against this helper.
  const double rates[] = {44100.0, 96000.0};
  const uint8_t notes[] = {21, 36, 48, 60, 72, 84, 96, 108};
  for (const ShippedVoice& v : shipped_voices()) {
    const float a_ship = v.a_voiced(v.brightness);
    for (double sr : rates) {
      const float reference = loss_pole_at_rate(a_ship, sr);
      for (uint8_t note : notes) {
        INFO(v.name << " sr " << sr << " note " << int{note});
        CHECK(loss_pole_at_rate(a_ship, sr) == reference);
      }
    }
  }
}

TEST_CASE("the loss law's Hz-domain shape is invariant across sample rates at a fixed note",
          "[midi][synth][loss_law]") {
  // A one-pole magnitude read at a fixed absolute Hz frequency, with the rate
  // swept, is the operational meaning of "the corner sits in Hz, not in
  // samples": the coefficient itself is expected to change with sr (a fixed
  // cutoff in Hz corresponds to a different feedback coefficient at every
  // rate) -- only the shape it produces, read in Hz, should not.
  const float a_ship = bowed_a_voiced(gm_fallback_patch(0, 42).bowed_string.brightness);
  const double corner_hz = -std::log(static_cast<double>(a_ship)) * kLossVoicedSr / kTwoPiD;
  const double rates[] = {44100.0, 48000.0, 96000.0};
  const double probes_hz[] = {200.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 10000.0, 16000.0};
  std::ostringstream report;
  double max_dev_under_corner = 0.0;
  double max_dev_at_16k = 0.0;
  double reading_1k_48k = 0.0;
  double reading_10k_48k = 0.0;
  for (double probe_hz : probes_hz) {
    double lo = 0.0, hi = 0.0;
    for (size_t i = 0; i < 3; ++i) {
      const double sr = rates[i];
      const float pole = loss_pole_at_rate(a_ship, sr);
      const float w = static_cast<float>(kTwoPiD * probe_hz / sr);
      const double db = 20.0 * std::log10(static_cast<double>(onepole_magnitude(pole, w)));
      report << "probe " << probe_hz << " Hz sr " << sr << " pole " << pole << " db " << db << "\n";
      if (i == 0) {
        lo = hi = db;
      }
      lo = std::min(lo, db);
      hi = std::max(hi, db);
      if (sr == 48000.0 && probe_hz == 1000.0) reading_1k_48k = db;
      if (sr == 48000.0 && probe_hz == 10000.0) reading_10k_48k = db;
    }
    const double spread = hi - lo;
    if (probe_hz <= corner_hz) max_dev_under_corner = std::max(max_dev_under_corner, spread);
    if (probe_hz == 16000.0) max_dev_at_16k = spread;
  }
  INFO(report.str() << "corner_hz " << corner_hz);
  // Bound with real headroom over the measured values, not just under them.
  CHECK(max_dev_under_corner < 0.15);
  CHECK(max_dev_at_16k < 2.0);
  // Sensitivity: the shape genuinely rolls off, so the invariance above is not
  // a metric that cannot move at all.
  CHECK(reading_1k_48k - reading_10k_48k > 3.0);
}

TEST_CASE("the rendered flute engine's loss corner does not move with note",
          "[midi][synth][flute][loss_law]") {
  // Exercises the real code path (FluteVoiceCore::start()/render()), unlike
  // the coefficient-level tests above: a note term reintroduced at the
  // engine's own call site would move the loop's Hz corner with the note,
  // which reads here as the octave-band tilt of a fixed absolute band
  // changing across notes at one fixed rate.
  // Notes 36-64: the flute's own register break above 64 (an octave jump in
  // the driven jet's operating point, unrelated to the loss law) makes octave-
  // band tilt an unstable measurement past it, so the note spread below stays
  // inside the one register where only the loss filter should be moving.
  const double sr = 48000.0;
  const uint8_t notes[] = {36, 40, 44, 48, 52, 56, 60, 64};
  const int samples = static_cast<int>(sr);  // 1 s: ample steady-state for a driven oscillator
  std::ostringstream report;
  double lo = 0.0, hi = 0.0;
  for (size_t i = 0; i < 8; ++i) {
    const std::vector<float> tone = render_flute_dark(notes[i], sr, samples);
    const BandTilt tilt = octave_band_tilt(tone, 200.0, 6400.0, sr);
    REQUIRE(tilt.bands >= 2);
    report << "note " << int{notes[i]} << " tilt " << tilt.db_per_octave << " dB/oct ("
           << tilt.bands << " bands)\n";
    if (i == 0) {
      lo = hi = tilt.db_per_octave;
    }
    lo = std::min(lo, tilt.db_per_octave);
    hi = std::max(hi, tilt.db_per_octave);
  }
  INFO(report.str());
  // Measured 1.96 dB with the fixed law; a note term reintroduced at this
  // engine's own call site (verified in a scratch copy, discarded) measures
  // 5.09 dB over the same notes -- the bound sits with real headroom over the
  // former and well under the latter.
  CHECK(hi - lo < 3.0);
  // Sensitivity: the metric can see the loop filter at all -- a bright patch
  // (a transparent pole) reads a shallower tilt than the dark one above, at
  // the same note and rate.
  FlutePatchParams bright;
  bright.jet_ratio = 0.5f;
  bright.jet_reflection = 0.5f;
  bright.end_reflection = 0.5f;
  bright.brightness = 1.0f;
  bright.damping = 0.0f;
  bright.breath_pressure = 0.7f;
  bright.vel_to_breath = 0.0f;
  bright.breath_noise = 0.0f;
  bright.chiff = 0.0f;
  bright.vibrato_depth = 0.0f;
  FluteVoiceCore core;
  const int per_span = flute_buffer_capacity(sr);
  std::vector<float> slab(static_cast<size_t>(flute_slab_capacity(sr)), 0.0f);
  core.attach(slab.data(), per_span);
  core.start(bright, sr, 60, sonare::midi::Velocity16::from7(100), 0x5011ADE5ull);
  std::vector<float> bright_tone(static_cast<size_t>(samples), 0.0f);
  for (int i = 0; i < samples; ++i) bright_tone[static_cast<size_t>(i)] = core.render(1.0f);
  const BandTilt dark60 = octave_band_tilt(render_flute_dark(60, sr, samples), 200.0, 6400.0, sr);
  const BandTilt bright60 = octave_band_tilt(bright_tone, 200.0, 6400.0, sr);
  REQUIRE(dark60.bands >= 2);
  REQUIRE(bright60.bands >= 2);
  INFO("dark tilt " << dark60.db_per_octave << " bright tilt " << bright60.db_per_octave);
  CHECK(dark60.db_per_octave < bright60.db_per_octave - 1.0);
}

TEST_CASE("the rendered bowed-string engine's loss corner does not move with note",
          "[midi][synth][bowed][loss_law]") {
  // Same measurement as the flute case above, over the bowed-string register.
  // Notes 28-64: the bowed string's own slip regime breaks down above 64 (a
  // register jump unrelated to the loss law -- one voice even loses enough
  // bands to drop the measurement), so the spread below stays inside the one
  // regime where only the loss filter should be moving.
  const double sr = 48000.0;
  const uint8_t notes[] = {28, 32, 36, 40, 44, 48, 52, 56, 60, 64};
  const int samples = static_cast<int>(sr);
  std::ostringstream report;
  double lo = 0.0, hi = 0.0;
  for (size_t i = 0; i < 10; ++i) {
    const std::vector<float> tone = render_bowed_dark(notes[i], sr, samples);
    const BandTilt tilt = octave_band_tilt(tone, 200.0, 6400.0, sr);
    REQUIRE(tilt.bands >= 2);
    report << "note " << int{notes[i]} << " tilt " << tilt.db_per_octave << " dB/oct ("
           << tilt.bands << " bands)\n";
    if (i == 0) {
      lo = hi = tilt.db_per_octave;
    }
    lo = std::min(lo, tilt.db_per_octave);
    hi = std::max(hi, tilt.db_per_octave);
  }
  INFO(report.str());
  // Measured 1.91 dB with the fixed law; the same reintroduced-note-term
  // scratch copy measures 4.40 dB over the same notes.
  CHECK(hi - lo < 3.0);
  // Sensitivity: a bright (transparent-pole) patch reads a shallower tilt
  // than the dark one, at the same note and rate.
  BowedStringPatchParams bright;
  bright.bow_position = 0.13f;
  bright.bow_force = 0.5f;
  bright.bow_speed = 0.7f;
  bright.vel_to_speed = 0.0f;
  bright.brightness = 1.0f;
  bright.damping = 0.4f;
  bright.rosin = 0.0f;
  BowedStringVoiceCore core;
  const int per_line = bowed_string_buffer_capacity(sr);
  std::vector<float> slab(static_cast<size_t>(bowed_string_slab_capacity(sr)), 0.0f);
  core.attach(slab.data(), per_line);
  core.start(bright, sr, 60, sonare::midi::Velocity16::from7(100), 0x5011ADE5ull);
  std::vector<float> bright_tone(static_cast<size_t>(samples), 0.0f);
  for (int i = 0; i < samples; ++i) bright_tone[static_cast<size_t>(i)] = core.render(1.0f);
  const BandTilt dark60 = octave_band_tilt(render_bowed_dark(60, sr, samples), 200.0, 6400.0, sr);
  const BandTilt bright60 = octave_band_tilt(bright_tone, 200.0, 6400.0, sr);
  REQUIRE(dark60.bands >= 2);
  REQUIRE(bright60.bands >= 2);
  INFO("dark tilt " << dark60.db_per_octave << " bright tilt " << bright60.db_per_octave);
  CHECK(dark60.db_per_octave < bright60.db_per_octave - 1.0);
}

TEST_CASE("the rendered engines' sr-discriminator, with ks_voice as the control",
          "[midi][synth][loss_law]") {
  // Exercises the real code path of all four migrated engines, unlike the
  // coefficient-level tests above. ks_voice is already frequency-referenced
  // (solve_string_loop_filter's own HF-quote solve), so its reading must ALSO
  // group by f0 -- proving the discriminator itself, not merely one engine,
  // can tell the two groupings apart.
  // Each engine's own pair of notes and per-note bounds: ks_voice's own sr
  // noise floor (measured 2.03-2.80 dB across every note used below) sets the
  // bound as much as the migrated engine's does, so the pair and its bounds
  // are per engine rather than shared.
  //
  // What the engines actually read, so a later widening is visibly a decision:
  // flute 0.295 / 0.286, bowed 0.248 / 0.008, plucked 0.294, an order of
  // magnitude inside bounds the control sets. A note term reintroduced at an
  // engine's own call site measured 5.09 (flute) and 4.40 (bowed), so the
  // bounds straddle the defect rather than merely clearing the noise.
  struct EngineCase {
    const char* name;
    std::vector<float> (*render)(uint8_t, double, int);
    uint8_t note_a;
    uint8_t note_b;
    double bound_a;
    double bound_b;
  };
  const EngineCase engines[] = {
      {"flute", render_flute_dark, 48, 72, 3.0, 3.5},
      {"bowed_string", render_bowed_dark, 48, 64, 3.0, 2.0},
      {"plucked_string", render_plucked_dark, 48, 60, 3.5, 4.5},
      {"reed", render_reed_dark, 48, 60, 3.0, 4.0},
  };
  const KsPatchParams& steel_guitar = gm_fallback_patch(0, 25).ks;
  const double rates[] = {24000.0, 48000.0, 96000.0};

  for (const EngineCase& engine : engines) {
    const uint8_t notes[] = {engine.note_a, engine.note_b};
    std::ostringstream report;
    double engine_spread[2] = {0.0, 0.0};
    double ks_spread[2] = {0.0, 0.0};
    double engine_reading[2] = {0.0, 0.0};
    for (size_t ni = 0; ni < 2; ++ni) {
      const uint8_t note = notes[ni];
      double e_lo = 0.0, e_hi = 0.0, k_lo = 0.0, k_hi = 0.0;
      for (size_t ri = 0; ri < 3; ++ri) {
        const double sr = rates[ri];
        const int samples = window_samples(sr);

        const std::vector<float> engine_out = engine.render(note, sr, samples);
        const BandTilt engine_tilt = octave_band_tilt(engine_out, 200.0, 6400.0, sr);
        const std::vector<float> ks_out = render_ks(steel_guitar, note, sr, samples);
        const BandTilt ks_tilt = octave_band_tilt(ks_out, 200.0, 6400.0, sr);

        report << engine.name << " note " << int{note} << " sr " << sr << "  engine "
               << engine_tilt.db_per_octave << " dB/oct (" << engine_tilt.bands << " bands)  ks "
               << ks_tilt.db_per_octave << " dB/oct (" << ks_tilt.bands << " bands)\n";

        REQUIRE(engine_tilt.bands >= 2);
        REQUIRE(ks_tilt.bands >= 2);
        if (ri == 0) {
          e_lo = e_hi = engine_tilt.db_per_octave;
          k_lo = k_hi = ks_tilt.db_per_octave;
        }
        e_lo = std::min(e_lo, engine_tilt.db_per_octave);
        e_hi = std::max(e_hi, engine_tilt.db_per_octave);
        k_lo = std::min(k_lo, ks_tilt.db_per_octave);
        k_hi = std::max(k_hi, ks_tilt.db_per_octave);
        if (ri == 1) engine_reading[ni] = engine_tilt.db_per_octave;  // the 48 kHz reading
      }
      engine_spread[ni] = e_hi - e_lo;
      ks_spread[ni] = k_hi - k_lo;
    }
    INFO(engine.name << "\n" << report.str());
    const double bound[2] = {engine.bound_a, engine.bound_b};
    for (size_t ni = 0; ni < 2; ++ni) {
      CHECK(engine_spread[ni] < bound[ni]);
      CHECK(ks_spread[ni] < bound[ni]);
    }
    // Sensitivity: the two notes' readings differ, so "small spread" above is
    // not "the metric cannot move".
    CHECK(std::fabs(engine_reading[0] - engine_reading[1]) > 1.0);
  }
}

// --- Struck and blown loops that apply the same law ---

namespace {

/// Spread (max - min) of @p reading across the 24 / 48 / 96 kHz grid.
template <typename Reading>
double spread_across_rates(Reading reading, std::ostringstream& report) {
  double lo = 0.0, hi = 0.0;
  bool first = true;
  for (double sr : {24000.0, 48000.0, 96000.0}) {
    const double v = reading(sr);
    report << "sr " << sr << " reading " << v << "\n";
    lo = first ? v : std::min(lo, v);
    hi = first ? v : std::max(hi, v);
    first = false;
  }
  return hi - lo;
}

double tilt_of(const std::vector<float>& tone, double sr) {
  const BandTilt tilt = octave_band_tilt(tone, 200.0, 6400.0, sr);
  REQUIRE(tilt.bands >= 2);
  return tilt.db_per_octave;
}

std::vector<float> render_piano(const sonare::midi::synth::PianoPatchParams& params, uint8_t note,
                                double sr, int samples) {
  sonare::midi::synth::PianoVoiceCore core;
  std::vector<float> slab(static_cast<size_t>(sonare::midi::synth::piano_slab_capacity(sr)), 0.0f);
  core.attach(slab.data(), sonare::midi::synth::piano_string_capacity(sr));
  core.start(params, sr, note, sonare::midi::Velocity16::from7(100), 0x5011ADE5ull);
  std::vector<float> out(static_cast<size_t>(samples));
  for (float& v : out) v = core.render(1.0f);
  return out;
}

/// Mean power (dB) at 0.3 / 0.5 / 0.7 of each harmonic gap of @p f0 inside
/// [lo_hz, hi_hz], Hann-windowed over the whole of @p x.
double gap_floor_db(const std::vector<float>& x, double f0, double sr, double lo_hz, double hi_hz) {
  const size_t n = x.size();
  double acc = 0.0;
  int count = 0;
  for (int k = 1; (k + 0.3) * f0 <= hi_hz; ++k) {
    for (const double frac : {0.3, 0.5, 0.7}) {
      const double f = (k + frac) * f0;
      if (f < lo_hz || f > hi_hz) continue;
      const double w = kTwoPiD * f / sr;
      const double coeff = 2.0 * std::cos(w);
      double s1 = 0.0, s2 = 0.0;
      for (size_t i = 0; i < n; ++i) {
        const double hann = 0.5 - 0.5 * std::cos(kTwoPiD * static_cast<double>(i) / n);
        const double s0 = hann * x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
      }
      const double re = s1 - s2 * std::cos(w);
      const double im = s2 * std::sin(w);
      const double mag = std::sqrt(re * re + im * im) / (static_cast<double>(n) / 4.0);
      acc += mag * mag;
      ++count;
    }
  }
  REQUIRE(count >= 18);
  return 10.0 * std::log10(std::max(1.0e-24, acc / count));
}

}  // namespace

TEST_CASE("the KS brightness pole keeps its corner across rates where no HF target is solved",
          "[midi][synth][ks][loss_law]") {
  // hf_decay_s == 0 leaves the brightness knob's own pole as the only loop
  // loss. Measured spread: 4.34 dB/oct with the pole unmapped, 0.31 mapped.
  KsPatchParams dark;
  dark.brightness = 0.0f;
  dark.hf_decay_s = 0.0f;
  dark.decay_s = 3.0f;
  std::ostringstream report;
  const double spread = spread_across_rates(
      [&](double sr) {
        const int samples = window_samples(sr);
        return tilt_of(render_ks(dark, 48, sr, samples), sr);
      },
      report);
  INFO(report.str());
  CHECK(spread < 1.5);
}

TEST_CASE("the piano string-loop pole keeps its corner across rates", "[midi][synth][loss_law]") {
  // The dark end of the shipped grand at A0, where the pole is strongest, read
  // after the strike noise has died so only the loop filter shapes the tilt.
  // Measured spread: 8.64 dB/oct with the pole unmapped, 0.28 mapped.
  sonare::midi::synth::PianoPatchParams dark = gm_fallback_patch(0, 0).piano;
  dark.brightness = 0.0f;
  std::ostringstream report;
  const double spread = spread_across_rates(
      [&](double sr) {
        const int samples = window_samples(sr);
        const int skip = static_cast<int>(0.5 * sr);
        const std::vector<float> tone = render_piano(dark, 28, sr, skip + samples);
        return tilt_of(std::vector<float>(tone.begin() + skip, tone.end()), sr);
      },
      report);
  INFO(report.str());
  CHECK(spread < 1.5);
}

TEST_CASE("the piano strike noise keeps its power per Hz across rates", "[midi][synth][loss_law]") {
  // The first 50 ms of C4, read between the harmonics where the strike and
  // scrub noise sit. Measured spread: 8.5 dB unscaled, 2.3 dB scaled.
  const sonare::midi::synth::PianoPatchParams& grand = gm_fallback_patch(0, 0).piano;
  const double f0 = note_to_hz(uint8_t{60});
  std::ostringstream report;
  const double spread = spread_across_rates(
      [&](double sr) {
        const int samples = static_cast<int>(0.05 * sr);
        return gap_floor_db(render_piano(grand, 60, sr, samples), f0, sr, 1000.0, 6000.0);
      },
      report);
  INFO(report.str());
  CHECK(spread < 4.5);
}

TEST_CASE("the pipe-organ reflection floor keeps its corner across rates",
          "[midi][synth][loss_law]") {
  // A dark stopped rank at C3, whose reflection corner sits on the floor.
  // Breath and chiff off so the settled tone is deterministic. Measured
  // spread: 1.20 dB/oct with the floor held as a coefficient, 0.55 in Hz.
  sonare::midi::synth::PipeOrganPatchParams stopped;
  stopped.stopped = true;
  stopped.brightness = 0.0f;
  stopped.breath = 0.0f;
  stopped.chiff = 0.0f;
  std::ostringstream report;
  const double spread = spread_across_rates(
      [&](double sr) {
        const int samples = window_samples(sr);
        sonare::midi::synth::PipeOrganVoiceCore core;
        std::vector<float> slab(
            static_cast<size_t>(sonare::midi::synth::pipe_organ_slab_capacity(sr)), 0.0f);
        core.attach(slab.data(), sonare::midi::synth::pipe_organ_buffer_capacity(sr));
        core.start(stopped, sr, 48, sonare::midi::Velocity16::from7(100), 0x5011ADE5ull);
        for (int i = 0; i < samples / 2; ++i) core.render(1.0f);  // let it settle
        std::vector<float> tone(static_cast<size_t>(samples));
        for (float& v : tone) v = core.render(1.0f);
        return tilt_of(tone, sr);
      },
      report);
  INFO(report.str());
  CHECK(spread < 0.9);
}

TEST_CASE("loss_alpha_at_rate is the loss law in alpha form, exact at the voiced rate",
          "[midi][synth][loss_law]") {
  using sonare::midi::synth::loss_alpha_at_rate;
  for (const float alpha : {0.01f, 0.05f, 0.3f, 0.9f}) {
    INFO("alpha " << alpha);
    // Bit for bit at the voiced rate, which 1 - loss_pole_at_rate(1 - alpha) is not.
    REQUIRE(loss_alpha_at_rate(alpha, kLossVoicedSr) == alpha);
    // The corner in Hz, -ln(1 - alpha) * sr / (2 pi), holds at every other rate.
    const double corner_voiced = -std::log(1.0 - alpha) * kLossVoicedSr;
    for (const double sr : {22050.0, 44100.0, 96000.0, 192000.0}) {
      const double mapped = loss_alpha_at_rate(alpha, sr);
      CHECK(-std::log(1.0 - mapped) * sr == Catch::Approx(corner_voiced).epsilon(1.0e-5));
      CHECK(1.0f - loss_alpha_at_rate(alpha, sr) ==
            Catch::Approx(loss_pole_at_rate(1.0f - alpha, sr)).epsilon(1.0e-5));
    }
  }
  REQUIRE(loss_alpha_at_rate(1.0f, 96000.0) == 1.0f);  // a transparent filter stays transparent
}
