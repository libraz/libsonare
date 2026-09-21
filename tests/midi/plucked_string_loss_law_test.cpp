/// @file plucked_string_loss_law_test.cpp
/// @brief The plucked-string loop-loss law: the shipped brightness pole
///        reproduced exactly at a fixed anchor, its Hz-domain shape held
///        invariant across sample rates, and the engine's own rendered
///        sr-discriminator with ks_voice (already frequency-referenced) as
///        the control.
///
/// The shipped pole `(1 - brightness) * 0.7` carries no sr term, so the loss a
/// harmonic received was a function of n/N (n = harmonic number, N = sr/f0)
/// rather than of the harmonic's own frequency in Hz -- the same defect
/// design-loop-loss-law-2026-09-21.md #4 measured in the other four waveguide
/// engines. The fix re-derives the pole from decay_s's own t60 at the
/// fundamental (unchanged) and a second t60 at a fixed reference frequency,
/// both solved through solve_string_loop_filter.

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <vector>

#include "midi/bowed_string_probe.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/ks_voice.h"
#include "midi/synth/pitch.h"
#include "midi/synth/plucked_string_voice.h"
#include "midi/synth/string_loop.h"
#include "util/constants.h"

namespace {

using sonare::constants::kTwoPi;
using sonare::midi::synth::gm_fallback_patch;
using sonare::midi::synth::ks_buffer_capacity;
using sonare::midi::synth::ks_slab_capacity;
using sonare::midi::synth::KsPatchParams;
using sonare::midi::synth::KsVoiceCore;
using sonare::midi::synth::note_to_hz;
using sonare::midi::synth::onepole_magnitude;
using sonare::midi::synth::plucked_string_buffer_capacity;
using sonare::midi::synth::plucked_string_slab_capacity;
using sonare::midi::synth::PluckedStringPatchParams;
using sonare::midi::synth::PluckedStringVoiceCore;
using sonare::midi::synth::solve_string_loop_filter;
using sonare::midi::synth::string_loop_gain_for;
using sonare::midi::synth::StringLoopFilter;
using sonare::test::bowed::BandTilt;
using sonare::test::bowed::octave_band_tilt;

/// plucked_string_voice.cpp's kPluckedBrightnessRefHz, duplicated: it is a
/// SONARE_TUNABLE with internal linkage, so a re-fit that moves it must also
/// update this literal.
constexpr float kRefHz = 2500.0f;

/// The anchor the engine's law is built at (design-loop-loss-law-2026-09-21.md
/// #4.3): sitar note 60, 48 kHz.
constexpr uint8_t kAnchorNote = 60;
constexpr float kAnchorSr = 48000.0f;

/// The shipped pole, unchanged -- brightness straight into a feedback
/// coefficient with no sr term at all, the defect this law replaces.
float shipped_pole(float brightness) noexcept {
  return (1.0f - std::clamp(brightness, 0.0f, 1.0f)) * 0.7f;
}

/// plucked_string_voice.cpp's own decay-stretch formula, duplicated: t60 at
/// the fundamental, stretched per octave below A4. decay_s/decay_stretch are
/// not touched by this fix; reproducing them here is what lets the anchor
/// check and the sensitivity check use a realistic, per-note g0.
float stretched_t60(float decay_s, float decay_stretch, uint8_t note) noexcept {
  const float stretch = std::clamp(decay_stretch, 0.0f, 1.0f);
  const float octaves_below_a4 = (69.0f - static_cast<float>(note & 0x7Fu)) / 12.0f;
  return std::max(0.05f, decay_s) * std::exp2(stretch * octaves_below_a4);
}

/// Reproduces plucked_string_voice.cpp's brightness_hf(): the t60-ratio
/// fraction that reproduces the shipped pole's own darkening at kRefHz,
/// relative to the fundamental's own t60, read at the anchor. Depends on
/// decay_s/decay_stretch as well as brightness -- the anchor property this
/// buys is therefore patch-specific, not decay_s-independent.
float brightness_hf(float brightness, float decay_s, float decay_stretch) noexcept {
  const float a_ship = shipped_pole(brightness);
  const float anchor_period = kAnchorSr / note_to_hz(static_cast<float>(kAnchorNote));
  const float w0 = kTwoPi / anchor_period;
  const float w_ref = kTwoPi * kRefHz / kAnchorSr;
  const float tilt_a = onepole_magnitude(a_ship, w_ref) / onepole_magnitude(a_ship, w0);
  const float t60_a = stretched_t60(decay_s, decay_stretch, kAnchorNote);
  const float g0_a = string_loop_gain_for(anchor_period, kAnchorSr, t60_a);
  return 1.0f / (1.0f + std::log(tilt_a) / std::log(g0_a));
}

/// The engine's own solve, reproduced from the patch's fields and a
/// (note, sr) pair.
StringLoopFilter solve_for(float brightness, float decay_s, float decay_stretch, uint8_t note,
                           double sr) noexcept {
  const float period = static_cast<float>(sr) / note_to_hz(note);
  const float t60 = stretched_t60(decay_s, decay_stretch, note);
  const float hf = brightness_hf(brightness, decay_s, decay_stretch);
  const float omega0 = kTwoPi / period;
  const float omega_ref = kTwoPi * kRefHz / static_cast<float>(sr);
  const float g0 = string_loop_gain_for(period, sr, t60);
  const float g_ref = string_loop_gain_for(period, sr, t60 * hf);
  return solve_string_loop_filter(omega0, omega_ref, g0, g_ref);
}

/// |H(w)| in dB at a fixed Hz probe, given the solved filter and the rate the
/// probe frequency is expressed against.
double probe_db(const StringLoopFilter& f, double probe_hz, double sr) noexcept {
  const float w = static_cast<float>(kTwoPi * probe_hz / sr);
  return 20.0 * std::log10(static_cast<double>(f.g * onepole_magnitude(f.a, w)));
}

std::vector<float> render_plucked(const PluckedStringPatchParams& params, uint8_t note, double sr,
                                  int samples) {
  PluckedStringVoiceCore core;
  const int per_line = plucked_string_buffer_capacity(sr);
  std::vector<float> slab(static_cast<size_t>(plucked_string_slab_capacity(sr)), 0.0f);
  core.attach(slab.data(), per_line);
  core.start(params, sr, note, 100, 0x5011ADE5ull);
  std::vector<float> out(static_cast<size_t>(samples), 0.0f);
  for (int i = 0; i < samples; ++i) out[static_cast<size_t>(i)] = core.render(1.0f);
  return out;
}

std::vector<float> render_ks(const KsPatchParams& params, uint8_t note, double sr, int samples) {
  KsVoiceCore core;
  const int per_line = ks_buffer_capacity(sr);
  std::vector<float> slab(static_cast<size_t>(ks_slab_capacity(sr)), 0.0f);
  core.attach(slab.data(), per_line);
  core.start(params, sr, note, 100, 0x5011ADE5ull);
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

TEST_CASE("the loss law reproduces each shipped patch's own pole at its anchor",
          "[midi][synth][plucked_string][loss_law]") {
  // hf is derived from brightness AND decay_s/decay_stretch together (the
  // t60-ratio form the sibling engines use, unlike a fixed per-traversal gain
  // ratio), so the anchor property is patch-specific rather than
  // decay_s-independent: it holds for a patch's OWN shipped fields, not for an
  // arbitrary decay crossed with that patch's brightness.
  const PluckedStringPatchParams& sitar = gm_fallback_patch(0, 104).plucked_string;
  const PluckedStringPatchParams& shamisen = gm_fallback_patch(0, 106).plucked_string;
  const PluckedStringPatchParams& koto = gm_fallback_patch(0, 107).plucked_string;
  CHECK(sitar.brightness == 0.817194f);
  CHECK(sitar.decay_s == 5.06967f);
  CHECK(shamisen.brightness == 0.8f);
  CHECK(shamisen.decay_s == 2.0f);
  CHECK(koto.brightness == 0.8f);
  CHECK(koto.decay_s == 3.0f);

  for (const PluckedStringPatchParams* p : {&sitar, &shamisen, &koto}) {
    const float a_ship = shipped_pole(p->brightness);
    const StringLoopFilter f =
        solve_for(p->brightness, p->decay_s, p->decay_stretch, kAnchorNote, kAnchorSr);
    INFO("brightness " << p->brightness << " decay_s " << p->decay_s);
    CHECK(f.a == Catch::Approx(a_ship).epsilon(1e-5));
  }
}

TEST_CASE("the loss law's coefficient does not group by N",
          "[midi][synth][plucked_string][loss_law]") {
  // Two (note, sr) pairs that land on the same period N would have been
  // handed the identical pole under the shipped `(1-brightness)*0.7` formula,
  // since that formula never reads the period at all. Now they must not
  // agree, because omega_ref is a function of sr alone.
  const PluckedStringPatchParams& sitar = gm_fallback_patch(0, 104).plucked_string;
  const float period_a = kAnchorSr / note_to_hz(48.0f);         // note 48 @ 48 kHz
  const float period_b = 2.0f * kAnchorSr / note_to_hz(60.0f);  // note 60 @ 96 kHz
  INFO("period_a " << period_a << " period_b " << period_b);
  REQUIRE(period_a == Catch::Approx(period_b).epsilon(1e-6));

  const StringLoopFilter fa =
      solve_for(sitar.brightness, sitar.decay_s, sitar.decay_stretch, 48, kAnchorSr);
  const StringLoopFilter fb =
      solve_for(sitar.brightness, sitar.decay_s, sitar.decay_stretch, 60, 2.0 * kAnchorSr);
  INFO("a at N=" << period_a << ": 48kHz pole " << fa.a << ", 96kHz pole " << fb.a);
  CHECK(std::fabs(fa.a - fb.a) > 1e-3f);
}

TEST_CASE("the loss law's Hz-domain shape is invariant across sample rates at a fixed note",
          "[midi][synth][plucked_string][loss_law]") {
  // A one-pole magnitude read at a fixed absolute Hz frequency, at a note
  // held fixed and the rate swept, is the operational meaning of "grouping by
  // f0" for a filter coefficient: a per-SAMPLE pole necessarily changes value
  // with sr to hold a fixed Hz-domain shape (a fixed cutoff in Hz corresponds
  // to a different feedback coefficient at every rate), so the pole ITSELF is
  // not expected to be sr-invariant -- only the shape it produces, read in Hz,
  // is.
  const PluckedStringPatchParams& sitar = gm_fallback_patch(0, 104).plucked_string;
  const double rates[] = {24000.0, 48000.0, 96000.0};
  const uint8_t notes[] = {48, 60};
  std::ostringstream report;
  double spread_within_note = 0.0;
  double note48_db = 0.0;
  double note60_db = 0.0;
  for (uint8_t note : notes) {
    double lo = 0.0;
    double hi = 0.0;
    for (size_t i = 0; i < 3; ++i) {
      const double sr = rates[i];
      const StringLoopFilter f =
          solve_for(sitar.brightness, sitar.decay_s, sitar.decay_stretch, note, sr);
      const double db = probe_db(f, 1000.0, sr);
      report << "note " << int{note} << " sr " << sr << " a " << f.a << " probe@1kHz(dB) " << db
             << "\n";
      if (i == 0) {
        lo = db;
        hi = db;
      }
      lo = std::min(lo, db);
      hi = std::max(hi, db);
      if (note == 48) note48_db = db;
      if (note == 60) note60_db = db;
    }
    spread_within_note = std::max(spread_within_note, hi - lo);
  }
  INFO(report.str());
  // Invariant across sr, to a small fraction of a dB.
  CHECK(spread_within_note < 0.01);
  // And genuinely sensitive: two different notes read differently, an order
  // of magnitude past the sr-spread above, so the invariance above is not a
  // metric that cannot move at all.
  CHECK(std::fabs(note48_db - note60_db) > 0.02);
}

TEST_CASE("the rendered plucked-string engine's sr-discriminator, with ks_voice as the control",
          "[midi][synth][plucked_string][loss_law]") {
  // Exercises the real code path (PluckedStringVoiceCore::start()), unlike
  // the coefficient-level tests above. brightness = 0 (a fully dark pole) is
  // an illustrative value chosen for measurement power, not the shipped sitar
  // patch: the sitar's own brightness (0.817194) gives an almost-transparent
  // pole, so its output-level effect is latent, the same way brass's own
  // shipped range moves the output by only 0.14 dB
  // (design-loop-loss-law-2026-09-21.md #9.5) -- an output reading there is
  // read off the coefficient instead, which the tests above already do.
  PluckedStringPatchParams dark;
  dark.buzz = 0.0f;
  dark.brightness = 0.0f;
  dark.decay_s = 4.0f;
  dark.decay_stretch = 0.5f;
  dark.pick_position = 0.0f;

  // ks_voice: already frequency-referenced (ks_voice.cpp's HF-quote solve),
  // so its reading must ALSO group by f0 -- proving the discriminator itself,
  // not merely this engine, can tell the two groupings apart. brightness is a
  // dead knob on ks_voice whenever the HF-quote solve overwrites the pole (as
  // it does for every note below MIDI 101), so decay_s is what would move it;
  // here nothing needs moving, only sr-invariance needs confirming.
  const KsPatchParams& steel_guitar = gm_fallback_patch(0, 25).ks;

  const double rates[] = {24000.0, 48000.0, 96000.0};
  const uint8_t notes[] = {48, 60};
  std::ostringstream report;
  double plucked_spread[2] = {0.0, 0.0};
  double ks_spread[2] = {0.0, 0.0};
  double plucked_reading[2] = {0.0, 0.0};

  for (size_t ni = 0; ni < 2; ++ni) {
    const uint8_t note = notes[ni];
    double p_lo = 0.0, p_hi = 0.0, k_lo = 0.0, k_hi = 0.0;
    for (size_t ri = 0; ri < 3; ++ri) {
      const double sr = rates[ri];
      const int samples = window_samples(sr);

      const std::vector<float> plucked_out = render_plucked(dark, note, sr, samples);
      const BandTilt plucked_tilt = octave_band_tilt(plucked_out, 200.0, 6400.0, sr);
      const std::vector<float> ks_out = render_ks(steel_guitar, note, sr, samples);
      const BandTilt ks_tilt = octave_band_tilt(ks_out, 200.0, 6400.0, sr);

      report << "note " << int{note} << " sr " << sr << "  plucked " << plucked_tilt.db_per_octave
             << " dB/oct (" << plucked_tilt.bands << " bands)  ks " << ks_tilt.db_per_octave
             << " dB/oct (" << ks_tilt.bands << " bands)\n";

      REQUIRE(plucked_tilt.bands >= 2);
      REQUIRE(ks_tilt.bands >= 2);
      if (ri == 0) {
        p_lo = p_hi = plucked_tilt.db_per_octave;
        k_lo = k_hi = ks_tilt.db_per_octave;
      }
      p_lo = std::min(p_lo, plucked_tilt.db_per_octave);
      p_hi = std::max(p_hi, plucked_tilt.db_per_octave);
      k_lo = std::min(k_lo, ks_tilt.db_per_octave);
      k_hi = std::max(k_hi, ks_tilt.db_per_octave);
      if (ri == 1) plucked_reading[ni] = plucked_tilt.db_per_octave;  // the 48 kHz reading
    }
    plucked_spread[ni] = p_hi - p_lo;
    ks_spread[ni] = k_hi - k_lo;
  }
  INFO(report.str());
  // Pre-fix spread for this dark patch, measured against the pre-fix object
  // directly: 4.30 dB at note 48, 8.74 dB at note 60. Post-fix, measured here:
  // 0.21 dB and 2.80 dB. The already-fixed ks_voice control reads 2.03 dB and
  // 2.80 dB at the same notes/rates -- so ~2.8 dB is this discriminator's own
  // floor at note 60 (the Lagrange-3 interpolator's own dispersion is a
  // separate, un-addressed residual at this register), not something this fix
  // could drive lower. The bounds below are set with real headroom over the
  // measured post-fix numbers, not just under the pre-fix ones, since a bound
  // a few percent over a measured float value is a flake rather than a gate.
  constexpr double kSpreadBoundHz48 = 3.5;  // measured 0.21 (plucked), 2.03 (ks)
  constexpr double kSpreadBoundHz60 = 4.5;  // measured 2.80 (plucked), 2.80 (ks)
  const double bound[2] = {kSpreadBoundHz48, kSpreadBoundHz60};
  for (size_t ni = 0; ni < 2; ++ni) {
    CHECK(plucked_spread[ni] < bound[ni]);
    CHECK(ks_spread[ni] < bound[ni]);
  }
  // Sensitivity: the two notes' readings differ, so "small spread" above is
  // not "the metric cannot move".
  CHECK(std::fabs(plucked_reading[0] - plucked_reading[1]) > 1.0);
}
