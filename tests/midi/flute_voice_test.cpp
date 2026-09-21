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
#include "midi/synth/string_loop.h"
#include "midi/ump.h"
#include "support/audio_fixtures.h"
#include "support/midi_render.h"
#include "util/constants.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::FlutePatchParams;
using sonare::midi::synth::gm_fallback_patch;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::note_to_hz;
using sonare::midi::synth::onepole_magnitude;
using sonare::midi::synth::solve_string_loop_filter;
using sonare::midi::synth::string_loop_gain_for;
using sonare::midi::synth::StringLoopFilter;
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

// --- Bell loop-loss law reproduction ---
//
// flute_voice.cpp's own bell_loss_anchor_t60() / refresh_excitation_targets()
// are private (internal linkage), so this reproduces them from the patch
// fields and a (note, sr) pair, the same way plucked_string_loss_law_test.cpp
// reproduces plucked_string_voice.cpp's brightness_hf(). A re-fit that moves
// kBellRefHz must also update kFluteBellRefHz here.

using sonare::constants::kEpsilon;
using sonare::constants::kTwoPi;

constexpr float kFluteBellRefHz = 3000.0f;
// Note 76, not 60: interior to all eight shipped flute patches' voicematch
// gate grids (measured in seven of them; interpolated only in piccolo's,
// 74-90) rather than at the bottom edge of six and below the other two.
constexpr uint8_t kFluteBellAnchorNote = 76;
constexpr float kFluteBellAnchorSr = 48000.0f;

/// flute_voice.cpp's shipped pole formula, unchanged: brightness straight into
/// a feedback coefficient with no sr term, the defect this law replaces.
float flute_shipped_pole(float brightness) noexcept {
  return std::clamp(0.80f - 0.30f * std::clamp(brightness, 0.0f, 1.0f), 0.0f, 0.95f);
}

/// flute_voice.cpp's shipped flat loss formula, unchanged: already note/rate
/// invariant (a flat scalar carries no f0 dependence to fix).
float flute_shipped_damping_gain(float damping) noexcept {
  return std::clamp(1.0f - 0.18f * std::clamp(damping, 0.0f, 1.0f), 0.5f, 1.0f);
}

struct FluteBellT60Pair {
  float fundamental_s;
  float reference_s;
};

/// Reproduces flute_voice.cpp's bell_loss_anchor_t60(): the two decay targets
/// (t60, seconds) the shipped {a_ship, g_ship} pair implies at the anchor, both
/// read off the shipped pole's own response so its contribution at the
/// fundamental is not silently dropped -- the same shape
/// bowed_string_voice.cpp's bow_loss_anchor_t60() uses.
FluteBellT60Pair flute_bell_loss_anchor_t60(float a_ship, float g_ship) noexcept {
  const float anchor_period =
      kFluteBellAnchorSr / note_to_hz(static_cast<float>(kFluteBellAnchorNote));
  const float w0_a = kTwoPi / anchor_period;
  const float wref_a = kTwoPi * kFluteBellRefHz / kFluteBellAnchorSr;
  const float g0_a = g_ship * onepole_magnitude(a_ship, w0_a);
  const float gr_a = g_ship * onepole_magnitude(a_ship, wref_a);
  const float k = -6.907755279f * anchor_period / kFluteBellAnchorSr;
  return {k / std::log(std::max(kEpsilon, g0_a)), k / std::log(std::max(kEpsilon, gr_a))};
}

/// The engine's own solve, reproduced from the patch's fields and a
/// (note, sr) pair.
StringLoopFilter flute_solve_for(float brightness, float damping, uint8_t note,
                                 double sr) noexcept {
  const float period = static_cast<float>(sr) / note_to_hz(note);
  const float omega0 = kTwoPi / period;
  const float omega_ref = kTwoPi * kFluteBellRefHz / static_cast<float>(sr);
  const float a_ship = flute_shipped_pole(brightness);
  const float g_ship = flute_shipped_damping_gain(damping);
  const FluteBellT60Pair t60 = flute_bell_loss_anchor_t60(a_ship, g_ship);
  return solve_string_loop_filter(omega0, omega_ref,
                                  string_loop_gain_for(period, sr, t60.fundamental_s),
                                  string_loop_gain_for(period, sr, t60.reference_s));
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

TEST_CASE("the flute bell loss law reproduces each shipped patch's own pole/gain at its anchor",
          "[midi][synth][flute][loss_law]") {
  // Both gains fed to the solver are read off the SHIPPED pole's own response
  // (bell_loss_anchor_t60()), not the bare damping-gain formula alone, so the
  // solve reproduces {a_ship, g_ship} together, not just the pole.
  struct Patch {
    const char* name;
    float brightness;
    float damping;
  };
  const Patch patches[] = {
      {"piccolo", 0.371516f, 0.25f},      {"concert_flute", 0.55f, 0.30f},
      {"recorder", 0.50f, 0.35f},         {"pan_flute", 0.42f, 0.40f},
      {"blown_bottle", 0.35f, 0.671722f}, {"shakuhachi", 0.386933f, 0.0864287f},
      {"tin_whistle", 0.70f, 0.28f},      {"ocarina", 0.302582f, 0.55f},
  };
  for (const Patch& p : patches) {
    const float a_ship = flute_shipped_pole(p.brightness);
    const float g_ship = flute_shipped_damping_gain(p.damping);
    const StringLoopFilter f =
        flute_solve_for(p.brightness, p.damping, kFluteBellAnchorNote, kFluteBellAnchorSr);
    INFO(p.name << " brightness " << p.brightness << " damping " << p.damping);
    CHECK(f.a == Catch::Approx(a_ship).epsilon(1e-5));
    CHECK(f.g == Catch::Approx(g_ship).epsilon(1e-5));
  }
}

TEST_CASE("the flute bell loss law's coefficient does not group by N",
          "[midi][synth][flute][loss_law]") {
  // Two (note, sr) pairs landing on the same period N would have been handed
  // the identical pole under the shipped `clamp(0.80 - 0.30*brightness, 0,
  // 0.95)` formula, since that formula never reads the period at all. Now they
  // must not agree, because omega_ref is a function of sr alone.
  const FlutePatchParams& concert_flute = gm_fallback_patch(0, 73).flute;
  CHECK(concert_flute.brightness == 0.55f);
  CHECK(concert_flute.damping == 0.30f);

  const float period_a = kFluteBellAnchorSr / note_to_hz(48.0f);         // note 48 @ 48 kHz
  const float period_b = 2.0f * kFluteBellAnchorSr / note_to_hz(60.0f);  // note 60 @ 96 kHz
  INFO("period_a " << period_a << " period_b " << period_b);
  REQUIRE(period_a == Catch::Approx(period_b).epsilon(1e-6));

  const StringLoopFilter fa =
      flute_solve_for(concert_flute.brightness, concert_flute.damping, 48, kFluteBellAnchorSr);
  const StringLoopFilter fb = flute_solve_for(concert_flute.brightness, concert_flute.damping, 60,
                                              2.0 * kFluteBellAnchorSr);
  INFO("a at N=" << period_a << ": 48kHz pole " << fa.a << ", 96kHz pole " << fb.a);
  CHECK(std::fabs(fa.a - fb.a) > 1e-3f);
}

TEST_CASE(
    "the flute bell loss law's Hz-domain shape is invariant across sample rates at a fixed "
    "note",
    "[midi][synth][flute][loss_law]") {
  // A one-pole+gain magnitude read at a fixed absolute Hz frequency, at a note
  // held fixed and the rate swept, is the operational meaning of "grouping by
  // f0" for a filter coefficient: the coefficients themselves are NOT expected
  // to be sr-invariant (a fixed cutoff in Hz corresponds to a different
  // feedback coefficient at every rate) -- only the shape they produce, read
  // in Hz, is.
  //
  // Read EXACTLY at kFluteBellRefHz first: both g_fundamental and g_reference
  // are targets the solve hits exactly UNLESS an internal safety clamp binds
  // (solve_string_loop_filter's own kMaxLoopGain / sub-fundamental-ring
  // compensation, not the outer kMaxPole) -- normally 0.0000 dB, confirming
  // the mechanism, but note 48 sits 28 semitones below the note-76 anchor,
  // dark enough that a clamp engages and leaves a small (6.8e-4 dB, measured)
  // residual even here. The bound keeps real headroom over that.
  //
  // Read at 1000 Hz next, an INTERMEDIATE frequency the solve does not pin: a
  // one-pole has only two degrees of freedom, so hitting the fundamental and
  // kFluteBellRefHz exactly leaves the curve's shape AWAY from those two
  // points dependent on which `a` was needed to hit them -- and concert
  // flute's shipped brightness (0.55) gives a substantially coloured
  // a_ship = 0.635, unlike a nearly-transparent pole, so this residual is
  // measurably larger than plucked_string_loss_law_test.cpp's equivalent
  // check (whose patch's a_ship is 0.128). Measured at the note-76 anchor:
  // 0.79 dB at note 48 (28 semitones from the anchor, versus 12 at the old
  // note-60 anchor) -- distance from the anchor, not the anchor's own
  // position, drives this residual, exactly as the anchor-position rule
  // predicts. The bound sits with real headroom over the measured value.
  const FlutePatchParams& concert_flute = gm_fallback_patch(0, 73).flute;
  const double rates[] = {24000.0, 48000.0, 96000.0};
  const uint8_t notes[] = {48, 60};
  std::ostringstream report;
  double spread_at_ref_hz = 0.0;
  double spread_at_1khz = 0.0;
  double note48_db = 0.0;
  double note60_db = 0.0;
  for (uint8_t note : notes) {
    double lo_ref = 0.0, hi_ref = 0.0;
    double lo_1k = 0.0, hi_1k = 0.0;
    for (size_t i = 0; i < 3; ++i) {
      const double sr = rates[i];
      const StringLoopFilter f =
          flute_solve_for(concert_flute.brightness, concert_flute.damping, note, sr);
      const auto db_at = [&](double hz) {
        const double w = kTwoPi * hz / sr;
        return 20.0 *
               std::log10(static_cast<double>(f.g) * onepole_magnitude(f.a, static_cast<float>(w)));
      };
      const double db_ref = db_at(kFluteBellRefHz);
      const double db_1k = db_at(1000.0);
      report << "note " << int{note} << " sr " << sr << " a " << f.a << " g " << f.g
             << " probe@refHz(dB) " << db_ref << " probe@1kHz(dB) " << db_1k << "\n";
      if (i == 0) {
        lo_ref = hi_ref = db_ref;
        lo_1k = hi_1k = db_1k;
      }
      lo_ref = std::min(lo_ref, db_ref);
      hi_ref = std::max(hi_ref, db_ref);
      lo_1k = std::min(lo_1k, db_1k);
      hi_1k = std::max(hi_1k, db_1k);
      if (note == 48) note48_db = db_1k;
      if (note == 60) note60_db = db_1k;
    }
    spread_at_ref_hz = std::max(spread_at_ref_hz, hi_ref - lo_ref);
    spread_at_1khz = std::max(spread_at_1khz, hi_1k - lo_1k);
  }
  INFO(report.str());
  // At the pinned reference frequency: exact but for a small internal-clamp
  // residual far from the anchor (measured 6.8e-4 dB at note 48).
  CHECK(spread_at_ref_hz < 2.0e-3);
  // At an unpinned intermediate frequency: small, with real headroom over the
  // measured 0.79 dB maximum (note 48, 28 semitones from the note-76 anchor).
  CHECK(spread_at_1khz < 1.2);
  // And genuinely sensitive: two different notes read differently (measured
  // 1.55 dB apart at 1 kHz), well past the 1 kHz sr-spread above, so the
  // invariance above is not a metric that cannot move at all.
  CHECK(std::fabs(note48_db - note60_db) > 1.0);
}

TEST_CASE(
    "the flute bell loss law's solved pole is bounded by the solver's own ceiling across the "
    "shipped register",
    "[midi][synth][flute][loss_law]") {
  // solve_string_loop_filter() clamps its own output at kMaxPole = 0.995
  // (string_loop.h). At the note-60 anchor this never bound (measured max
  // 0.9352, ocarina, note 48/96 kHz). At the note-76 anchor it DOES: note 48
  // is now 28 semitones from the anchor rather than 12, and blown_bottle
  // (the darkest patch away from the anchor) reaches EXACTLY 0.995 at note
  // 48/96 kHz -- a direct, measured consequence of moving the anchor away
  // from the low end of the register, not a bug in the solve. This test
  // checks the pole stays finite and never exceeds the ceiling, which is what
  // the clamp is for; it does not assert the clamp stays unreached.
  struct Patch {
    const char* name;
    float brightness;
    float damping;
  };
  const Patch patches[] = {
      {"piccolo", 0.371516f, 0.25f},      {"concert_flute", 0.55f, 0.30f},
      {"recorder", 0.50f, 0.35f},         {"pan_flute", 0.42f, 0.40f},
      {"blown_bottle", 0.35f, 0.671722f}, {"shakuhachi", 0.386933f, 0.0864287f},
      {"tin_whistle", 0.70f, 0.28f},      {"ocarina", 0.302582f, 0.55f},
  };
  const double rates[] = {24000.0, 48000.0, 96000.0};
  float max_a = 0.0f;
  const char* max_name = "";
  int max_note = 0;
  double max_sr = 0.0;
  for (const Patch& p : patches) {
    for (int note = 48; note <= 96; note += 3) {
      for (double sr : rates) {
        const StringLoopFilter f =
            flute_solve_for(p.brightness, p.damping, static_cast<uint8_t>(note), sr);
        REQUIRE(std::isfinite(f.a));
        REQUIRE(std::isfinite(f.g));
        if (f.a > max_a) {
          max_a = f.a;
          max_name = p.name;
          max_note = note;
          max_sr = sr;
        }
      }
    }
  }
  INFO("max solved pole across the shipped register: " << max_a << " at " << max_name << " note "
                                                       << max_note << " sr " << max_sr);
  // Never exceeds the clamp, and the clamp is now demonstrably reached rather
  // than merely approached -- both are the expected shape, so the bound is
  // set just above the ceiling itself rather than at the old (now stale)
  // note-60-anchor headroom.
  CHECK(max_a <= 0.9951f);
}

TEST_CASE(
    "the flute bell pole's external 0.95 clamp is provably slack for any reachable "
    "brightness",
    "[midi][synth][flute][loss_law]") {
  // The clamp in flute_shipped_pole() / bell_loss_anchor_t60()'s a_ship input
  // (clamp(0.80 - 0.30*brightness, 0, 0.95)) is a formula reproduction, not a
  // live safety net: brightness is itself clamped to [0,1] before reaching it
  // (refresh_excitation_targets()), so a_ship's own range is [0.50, 0.80] for
  // ANY reachable brightness -- comfortably inside [0, 0.95]. Swept rather
  // than argued: 101 points across [0,1] plus all eight shipped patches.
  float max_a_ship = 0.0f;
  float min_a_ship = 1.0f;
  for (int i = 0; i <= 100; ++i) {
    const float br = static_cast<float>(i) / 100.0f;
    const float a = flute_shipped_pole(br);
    max_a_ship = std::max(max_a_ship, a);
    min_a_ship = std::min(min_a_ship, a);
  }
  CHECK(min_a_ship == Catch::Approx(0.50f).epsilon(1e-6));
  CHECK(max_a_ship == Catch::Approx(0.80f).epsilon(1e-6));
  CHECK(max_a_ship < 0.95f);

  const float shipped_brightness[] = {0.371516f, 0.55f,     0.50f, 0.42f,
                                      0.35f,     0.386933f, 0.70f, 0.302582f};
  for (float br : shipped_brightness) {
    CHECK(flute_shipped_pole(br) < 0.95f);
  }
}

TEST_CASE("the rendered flute engine's sr-discriminator groups by f0, not by N",
          "[midi][synth][flute][loss_law]") {
  // Exercises the real code path (FluteVoiceCore::start()/render()), unlike
  // the coefficient-level tests above: a Goertzel reading of the 10th harmonic
  // relative to the fundamental (h10/h1, dB), the same shape the wind survey's
  // own harness used, over the 0.5-1.5 s sustained window (a driven oscillator
  // has no decay to window around).
  //
  // Pre-fix (measured against `git show HEAD~1:...` before this change, at
  // brightness 0.20, damping 0.0 -- an illustrative dark patch for measurement
  // power, not a shipped one): the within-note spread across {24, 48, 96} kHz
  // is 13.30 dB at note 36, 15.26 dB at note 48, 39.82 dB at note 60. Post-fix
  // at the note-76 anchor with kBellRefHz = 3000, measured here: 4.50, 0.07
  // and 11.59 dB -- note 60's own spread grew relative to the note-60-anchor
  // measurement (was 0.96 dB) because it is now 16 semitones from the anchor
  // instead of 0, exactly the anchor-distance effect the anchor-position rule
  // predicts. The bounds sit with real headroom over these post-fix numbers,
  // not the pre-fix ones.
  const auto goertzel_mag = [](const std::vector<float>& x, size_t from, size_t count, double freq,
                               double sr) noexcept -> double {
    const double w = 2.0 * sonare::constants::kTwoPiD * freq / sr;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (size_t i = 0; i < count; ++i) {
      const double sample = (from + i < x.size()) ? static_cast<double>(x[from + i]) : 0.0;
      const double s0 = sample + coeff * s1 - s2;
      s2 = s1;
      s1 = s0;
    }
    const double real = s1 - s2 * std::cos(w);
    const double imag = s2 * std::sin(w);
    return std::sqrt(real * real + imag * imag) / (static_cast<double>(count) / 2.0);
  };

  NativeSynthPatch patch;
  patch.mode = SynthEngineMode::kFlute;
  patch.cutoff_hz = 20000.0f;
  patch.amp_env.attack_ms = 5.0f;
  patch.amp_env.sustain = 1.0f;
  patch.amp_env.release_ms = 100.0f;
  patch.flute.breath_pressure = 0.7f;
  patch.flute.vel_to_breath = 0.0f;
  patch.flute.jet_ratio = 0.5f;
  patch.flute.jet_reflection = 0.5f;
  patch.flute.end_reflection = 0.5f;
  patch.flute.brightness = 0.20f;
  patch.flute.damping = 0.0f;
  patch.flute.attack_ms = 5.0f;
  patch.flute.release_ms = 50.0f;
  patch.flute.breath_noise = 0.0f;
  patch.flute.chiff = 0.0f;
  patch.flute.vibrato_depth = 0.0f;

  const uint8_t notes[] = {36, 48, 60};
  const double bound_db[] = {6.0, 2.0, 14.0};
  const double rates[] = {24000.0, 48000.0, 96000.0};
  std::ostringstream report;
  double reading_at_48k[3] = {0.0, 0.0, 0.0};

  for (size_t ni = 0; ni < 3; ++ni) {
    const uint8_t note = notes[ni];
    const double f0 = note_hz(note);
    double lo = 0.0, hi = 0.0;
    for (size_t ri = 0; ri < 3; ++ri) {
      const double sr = rates[ri];
      NativeSynthConfig cfg;
      cfg.patch = patch;
      NativeSynth synth(cfg);
      synth.prepare(sr, 256);
      synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, 100)));
      const int total = static_cast<int>(1.5 * sr);
      const std::vector<float> out = render_left(synth, total);
      const int from = static_cast<int>(0.5 * sr);
      const size_t count = static_cast<size_t>(total - from);
      const double h1 = goertzel_mag(out, static_cast<size_t>(from), count, f0, sr);
      const double h10 = goertzel_mag(out, static_cast<size_t>(from), count, 10.0 * f0, sr);
      const double tilt_db = 20.0 * std::log10(h10 / std::max(1e-12, h1));
      report << "note " << int{note} << " sr " << sr << " tilt " << tilt_db << " dB\n";
      if (ri == 0) {
        lo = tilt_db;
        hi = tilt_db;
      }
      lo = std::min(lo, tilt_db);
      hi = std::max(hi, tilt_db);
      if (ri == 1) reading_at_48k[ni] = tilt_db;
    }
    INFO(report.str());
    CHECK(hi - lo < bound_db[ni]);
  }
  // Sensitivity: different notes read differently, so a small spread above is
  // not "the metric cannot move".
  CHECK(std::fabs(reading_at_48k[0] - reading_at_48k[2]) > 1.0);
}
