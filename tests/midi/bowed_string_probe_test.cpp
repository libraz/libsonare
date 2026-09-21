/// @file bowed_string_probe_test.cpp
/// @brief Self-verification of the bowed-string measurement helpers, plus the
///        control hashes the engine and body changes are judged against.
///
/// Each helper is read on a synthetic specimen whose answer is known by
/// construction, and every specimen's own buffer is hashed against a recorded
/// literal — a helper verified against a signal nobody pinned is not verified.
/// Alongside the specimen that carries the expected answer, each case also
/// reads a specimen that must give a DIFFERENT answer, so a helper returning a
/// constant fails rather than passes.
///
/// The control hashes at the foot are taken from the unmodified engine and are
/// the only thing that can say a later change is identity at its default field
/// values. They are float-arithmetic goldens like the synth manifests, so they
/// are read in the same build configuration they were recorded in (Debug).

#include "midi/bowed_string_probe.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "midi/synth/body_resonator.h"
#include "midi/synth/bowed_string_voice.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/pitch.h"
#include "midi/synth/string_loop.h"
#include "support/golden_hash.h"
#include "util/constants.h"

namespace {

using sonare::midi::synth::BodyResonator;
using sonare::midi::synth::BodyType;
using sonare::midi::synth::BowedStringPatchParams;
using sonare::midi::synth::BowedStringVoiceCore;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::note_to_hz;
using sonare::midi::synth::onepole_magnitude;
using sonare::midi::synth::solve_string_loop_filter;
using sonare::midi::synth::string_loop_gain_for;
using sonare::midi::synth::StringLoopFilter;
using sonare::test::fnv1a_quantized;
using sonare::test::bowed::bridge_force_ratio;
using sonare::test::bowed::bridge_force_ratio_db;
using sonare::test::bowed::bridge_force_variation_db;
using sonare::test::bowed::octave_band_tilt;
using sonare::test::bowed::slips_per_period;
using sonare::test::bowed::time_to_helmholtz;

constexpr double kSr = 48000.0;

// Comb fundamental and band edges for the tilt specimens. 46.875 Hz is exactly
// 32 bins of the 32768-point transform, and the edges sit at 4.1 * 2^k
// harmonics so no partial lands on a band boundary.
constexpr double kCombF0 = 46.875;
constexpr double kCombLoHz = 192.1875;
constexpr double kCombHiHz = 12300.0;
constexpr int kCombOctaves = 6;
constexpr int kCombSamples = 49152;

/// A harmonic comb of @p f0 whose octave-band RMS falls @p db_per_octave per
/// octave by construction: each band's partials share that band's power equally,
/// and nothing sounds outside [kCombLoHz, kCombHiHz).
std::vector<float> band_tilted_comb(double db_per_octave) {
  const int max_n = static_cast<int>(kCombHiHz / kCombF0) + 1;
  std::vector<double> amp(static_cast<std::size_t>(max_n) + 1, 0.0);
  double edge = kCombLoHz;
  for (int k = 0; k < kCombOctaves; ++k, edge *= 2.0) {
    int count = 0;
    for (int n = 1; n <= max_n; ++n) {
      const double hz = n * kCombF0;
      if (hz >= edge && hz < edge * 2.0) ++count;
    }
    if (count == 0) continue;
    const double band_power = std::pow(10.0, 0.1 * db_per_octave * k);
    const double a = std::sqrt(band_power / count);
    for (int n = 1; n <= max_n; ++n) {
      const double hz = n * kCombF0;
      if (hz >= edge && hz < edge * 2.0) amp[static_cast<std::size_t>(n)] = a;
    }
  }
  std::vector<float> out(kCombSamples, 0.0f);
  for (int n = 1; n <= max_n; ++n) {
    const double a = amp[static_cast<std::size_t>(n)];
    if (a <= 0.0) continue;
    // Deterministic per-partial phase; the spectrum does not depend on it, but
    // the recorded input hash does.
    const double phase = std::fmod(0.7654321 * n * n, 1.0) * sonare::constants::kTwoPiD;
    const double w = sonare::constants::kTwoPiD * n * kCombF0 / kSr;
    for (int i = 0; i < kCombSamples; ++i) {
      out[static_cast<std::size_t>(i)] += static_cast<float>(a * std::sin(w * i + phase));
    }
  }
  return out;
}

/// A ramp resetting @p slips times per period of @p f0: the reset is the slip.
/// The half-segment phase offset keeps every reset inside an analysis window
/// rather than on its edge.
std::vector<float> slip_ramp(double f0, int slips, int samples) {
  const int segment = static_cast<int>(std::lround(kSr / f0)) / slips;
  const int offset = segment / 2;
  std::vector<float> out(static_cast<std::size_t>(samples), 0.0f);
  for (int i = 0; i < samples; ++i) {
    const int phase = (i + offset) % segment;
    out[static_cast<std::size_t>(i)] = static_cast<float>(2.0 * phase / segment - 1.0);
  }
  return out;
}

std::vector<float> pure_tone(double f0, int samples) {
  std::vector<float> out(static_cast<std::size_t>(samples), 0.0f);
  const double w = sonare::constants::kTwoPiD * f0 / kSr;
  for (int i = 0; i < samples; ++i)
    out[static_cast<std::size_t>(i)] = static_cast<float>(std::sin(w * i));
  return out;
}

/// The shipped violin patch's bowed-string section.
const BowedStringPatchParams& violin_params() {
  return sonare::midi::synth::gm_fallback_patch(0, 40).bowed_string;
}

/// Bridge loop coefficients the engine derives from @p p at @p note / @p sr.
/// Mirrors the frequency-referenced law bowed_string_voice.cpp's start()
/// applies (anchor: violin note 55 @ 48 kHz; reference frequency 2000 Hz,
/// its kBowLossRefHz) — a change to either moves this control hash.
StringLoopFilter patch_bridge_filter(const BowedStringPatchParams& p, uint8_t note, double sr) {
  constexpr float kAnchorNote = 55.0f;
  constexpr float kAnchorSr = 48000.0f;
  constexpr float kRefHz = 2000.0f;
  const float a_ship = static_cast<float>(1.0 - 0.7 * (1.0 - static_cast<double>(p.brightness)));
  const float g_ship = static_cast<float>(0.99 - 0.09 * static_cast<double>(p.damping));
  const float anchor_period = kAnchorSr / note_to_hz(kAnchorNote);
  const float w0_a = sonare::constants::kTwoPi / anchor_period;
  const float wref_a = sonare::constants::kTwoPi * kRefHz / kAnchorSr;
  const float g0_a = g_ship * onepole_magnitude(a_ship, w0_a);
  const float gr_a = g_ship * onepole_magnitude(a_ship, wref_a);
  const float k = -6.907755279f * anchor_period / kAnchorSr;
  const float t60_0 = k / std::log(std::max(sonare::constants::kEpsilon, g0_a));
  const float t60_ref = k / std::log(std::max(sonare::constants::kEpsilon, gr_a));
  const float period = static_cast<float>(sr) / note_to_hz(note);
  const float w0 = sonare::constants::kTwoPi / period;
  const float wref = sonare::constants::kTwoPi * kRefHz / static_cast<float>(sr);
  return solve_string_loop_filter(w0, wref, string_loop_gain_for(period, sr, t60_0),
                                  string_loop_gain_for(period, sr, t60_ref));
}

constexpr uint8_t kControlNote = 60;
constexpr uint8_t kControlVelocity = 100;
constexpr uint64_t kControlSeed = 0x5011ADE5ull;

/// Renders the bowed-string core alone for one second on @p params.
std::vector<float> render_core(const BowedStringPatchParams& params, int samples) {
  BowedStringVoiceCore core;
  const int per_line = sonare::midi::synth::bowed_string_buffer_capacity(kSr);
  std::vector<float> slab(
      static_cast<std::size_t>(sonare::midi::synth::bowed_string_slab_capacity(kSr)), 0.0f);
  core.attach(slab.data(), per_line);
  core.start(params, kSr, kControlNote, kControlVelocity, kControlSeed);
  std::vector<float> out(static_cast<std::size_t>(samples), 0.0f);
  for (int i = 0; i < samples; ++i) out[static_cast<std::size_t>(i)] = core.render(1.0f);
  return out;
}

}  // namespace

TEST_CASE("bowed probe reads an octave-band tilt", "[midi][synth][bowed][probe]") {
  // A sawtooth's PARTIALS fall 6 dB/octave, but its octave BANDS fall 3 — each
  // band holds twice as many partials. The specimen is built band-wise so the
  // tilt under test is the one the helper is defined on.
  const std::vector<float> tilted = band_tilted_comb(-6.0);
  const std::vector<float> flat = band_tilted_comb(0.0);
  CHECK(fnv1a_quantized(tilted) == 0x33072efa9d3c21c3ull);
  CHECK(fnv1a_quantized(flat) == 0x58948660dec83423ull);

  const sonare::test::bowed::BandTilt measured =
      octave_band_tilt(tilted, kCombLoHz, kCombHiHz, kSr);
  REQUIRE(measured.bands == kCombOctaves);
  CHECK(measured.db_per_octave > -6.3);
  CHECK(measured.db_per_octave < -5.7);

  // Sensitivity: the same helper on a flat specimen must not answer -6.
  const sonare::test::bowed::BandTilt level = octave_band_tilt(flat, kCombLoHz, kCombHiHz, kSr);
  REQUIRE(level.bands == kCombOctaves);
  CHECK(level.db_per_octave > -0.3);
  CHECK(level.db_per_octave < 0.3);
}

TEST_CASE("bowed probe counts slips per period", "[midi][synth][bowed][probe]") {
  const std::vector<float> two = slip_ramp(100.0, 2, 48000);
  const std::vector<float> one = slip_ramp(100.0, 1, 48000);
  CHECK(fnv1a_quantized(two) == 0x09b410db4303f473ull);
  CHECK(fnv1a_quantized(one) == 0xb0d51bc1db15412bull);

  const sonare::test::bowed::SlipRate double_slip = slips_per_period(two, 100.0, kSr);
  REQUIRE(double_slip.periods > 0.0);
  CHECK(double_slip.per_period > 1.9);
  CHECK(double_slip.per_period < 2.1);

  // Sensitivity: one reset per period must read 1, not 2.
  const sonare::test::bowed::SlipRate helmholtz = slips_per_period(one, 100.0, kSr);
  REQUIRE(helmholtz.periods > 0.0);
  CHECK(helmholtz.per_period > 0.9);
  CHECK(helmholtz.per_period < 1.1);

  // A sinusoid has no discontinuity at all, so it slips zero times per period —
  // which is what keeps the onset predicate two-sided.
  const std::vector<float> tone = pure_tone(100.0, 48000);
  CHECK(fnv1a_quantized(tone) == 0x10a65925bd6fca83ull);
  const sonare::test::bowed::SlipRate smooth = slips_per_period(tone, 100.0, kSr);
  REQUIRE(smooth.periods > 0.0);
  CHECK(smooth.slips == 0);
}

TEST_CASE("bowed probe reports that a pure tone never establishes Helmholtz motion",
          "[midi][synth][bowed][probe]") {
  const std::vector<float> tone_250 = pure_tone(250.0, 48000);
  CHECK(fnv1a_quantized(tone_250) == 0x6b6c829f641218c3ull);
  const sonare::test::bowed::HelmholtzOnset never = time_to_helmholtz(tone_250, 250.0, kSr);
  REQUIRE(never.windows > 0);
  CHECK_FALSE(never.established);

  // Sensitivity: the same scan over the same span of a one-slip waveform must
  // establish, or "never" is a constant rather than a measurement.
  const std::vector<float> saw = slip_ramp(250.0, 1, 48000);
  CHECK(fnv1a_quantized(saw) == 0xf3b5bfc55b48252full);
  const sonare::test::bowed::HelmholtzOnset onset = time_to_helmholtz(saw, 250.0, kSr);
  REQUIRE(onset.windows > 0);
  CHECK(onset.established);
  CHECK(onset.seconds < 0.01);

  // A waveform that slips twice per period never establishes either.
  const std::vector<float> two_250 = slip_ramp(250.0, 2, 48000);
  CHECK(fnv1a_quantized(two_250) == 0xae753b22643e1cfbull);
  const sonare::test::bowed::HelmholtzOnset multiple = time_to_helmholtz(two_250, 250.0, kSr);
  REQUIRE(multiple.windows > 0);
  CHECK_FALSE(multiple.established);
}

TEST_CASE("bowed probe measures the bridge force the output stands in for",
          "[midi][synth][bowed][probe]") {
  // The reachable coefficient set: damping in [0,1] gives a loss gain in
  // [0.90, 0.99], brightness in [0,1] a filter coefficient in [0.3, 1.0].
  double worst = 0.0;
  for (int di = 0; di <= 20; ++di) {
    for (int bi = 0; bi <= 20; ++bi) {
      const double damping = di / 20.0;
      const double brightness = bi / 20.0;
      const double g = 0.99 - 0.09 * damping;
      const double a = 1.0 - 0.7 * (1.0 - brightness);
      worst = std::max(worst, bridge_force_variation_db(g, a, kSr));
    }
  }
  INFO("worst reachable bridge-force variation (dB): " << worst);
  CHECK(worst <= 4.7);

  // Positive control: a flat comb through a known (g, a). The measured ratio is
  // read against the analytic value over the band the comb actually reaches.
  const std::vector<float> comb = band_tilted_comb(0.0);
  CHECK(fnv1a_quantized(comb) == 0x58948660dec83423ull);
  const double g = 0.99;
  const double a = 0.3;
  const sonare::test::bowed::BridgeForce control = bridge_force_ratio(comb, g, a, -60.0, kSr);
  REQUIRE(control.bins > 0);
  const double expected = bridge_force_ratio_db(g, a, control.lo_hz, kSr) -
                          bridge_force_ratio_db(g, a, control.hi_hz, kSr);
  INFO("control band " << control.lo_hz << ".." << control.hi_hz << " Hz over " << control.bins
                       << " bins: measured " << control.variation_db << " dB, analytic " << expected
                       << " dB");
  CHECK(std::fabs(control.variation_db - expected) < 0.15);

  // The engine, on the shipped violin patch with the two radiation gates off so
  // render() is output_scale_ * bridge_out_ and the incoming wave is readable.
  BowedStringPatchParams params = violin_params();
  params.polarization = 0.0f;
  params.sympathetic = 0.0f;
  const std::vector<float> v_plus = render_core(params, 48000);
  CHECK(fnv1a_quantized(v_plus) == 0xde1134e7e5d7d17cull);  // was 0x7c692eb527199fe0
  const StringLoopFilter solved = patch_bridge_filter(params, kControlNote, kSr);
  const double violin_g = static_cast<double>(solved.g);
  const double violin_a = 1.0 - static_cast<double>(solved.a);
  const sonare::test::bowed::BridgeForce measured =
      bridge_force_ratio(v_plus, violin_g, violin_a, -60.0, kSr);
  REQUIRE(measured.bins > 0);
  const double full_band = bridge_force_variation_db(violin_g, violin_a, kSr);
  INFO("violin band " << measured.lo_hz << ".." << measured.hi_hz << " Hz over " << measured.bins
                      << " bins: measured " << measured.variation_db << " dB, full band "
                      << full_band << " dB");
  CHECK(measured.variation_db <= 4.7);
  CHECK(full_band <= 4.7);
  // A sub-band cannot vary more than the whole band the same filter spans.
  CHECK(measured.variation_db <= full_band + 0.1);
}

TEST_CASE("bowed probe reaches the engine's own output", "[midi][synth][bowed][probe]") {
  // The synthetic specimens say the helpers read a waveform correctly; this says
  // they read THIS engine, which is the thing the onset and window sweeps depend
  // on. The numbers are reported rather than asserted — what is asserted is that
  // a stick-slip model slips at all and that the scan ran.
  const std::vector<float> render = render_core(violin_params(), 48000);
  CHECK(fnv1a_quantized(render) == 0x656683fee32ecbb7ull);  // was 0xa6d63b25826c6403
  const double f0 = static_cast<double>(sonare::midi::synth::note_to_hz(kControlNote));

  const sonare::test::bowed::SlipRate rate = slips_per_period(render, f0, kSr);
  INFO("violin note 60: " << rate.slips << " slips over " << rate.periods
                          << " periods = " << rate.per_period << " per period");
  REQUIRE(rate.periods > 0.0);
  CHECK(rate.slips > 0);

  const sonare::test::bowed::HelmholtzOnset onset = time_to_helmholtz(render, f0, kSr);
  INFO("violin note 60 onset: established " << onset.established << " at " << onset.seconds
                                            << " s over " << onset.windows << " windows");
  REQUIRE(onset.windows > 0);

  const sonare::test::bowed::BandTilt tilt = octave_band_tilt(render, 250.0, 16000.0, kSr);
  INFO("violin note 60 tilt: " << tilt.db_per_octave << " dB/oct over " << tilt.bands << " bands");
  REQUIRE(tilt.bands >= 2);
}

TEST_CASE("bowed string control hashes", "[midi][synth][bowed][probe]") {
  // (a) The bowed-string core alone on the shipped violin patch. The patch
  // values are asserted beside the hash so a moved patch value is told apart
  // from a moved engine when this goes red.
  const BowedStringPatchParams& v = violin_params();
  CHECK(v.bow_position == 0.169028f);  // was 0.17016
  CHECK(v.bow_force == 0.0643318f);
  CHECK(v.bow_speed == 0.630748f);
  CHECK(v.vel_to_speed == 0.312461f);  // was 0.6, the clamp default the field
                                       // inherited before the loop-loss re-fit
  CHECK(v.brightness == 0.228986f);    // was 0.47
  CHECK(v.damping == 0.0822536f);
  CHECK(v.attack_ms == 41.9837f);  // was 47.142
  CHECK(v.release_ms == 147.15f);  // was 165.23
  CHECK(v.rosin == 0.0875388f);    // was 0.1
  CHECK(v.elasto_plastic);
  CHECK(v.stribeck == 0.7f);
  CHECK(v.sympathetic == 0.495379f);  // was 0.08
  CHECK(v.polarization == 0.15f);

  const std::vector<float> core = render_core(v, 48000);
  CHECK(fnv1a_quantized(core) == 0x656683fee32ecbb7ull);  // was 0xa6d63b25826c6403

  // (b) The body resonator on both of its entry points, driven by the same
  // deterministic excitation. The percussion-shell configuration is written out
  // here rather than read from a kit so this control fires for a change to
  // process()/start_specs and not for a drum note being re-voiced.
  std::vector<float> excitation(12000, 0.0f);
  uint32_t state = 0x1234567u;
  for (float& sample : excitation) {
    state = state * 1664525u + 1013904223u;
    sample = static_cast<float>(static_cast<int32_t>(state >> 8) % 2001 - 1000) / 1000.0f;
  }
  CHECK(fnv1a_quantized(excitation) == 0xbe934ca06e9e66fdull);

  BodyResonator violin_body;
  violin_body.start(BodyType::kViolin, kSr, 261.63f, 0.28f);
  REQUIRE(violin_body.active());
  std::vector<float> violin_out(excitation.size(), 0.0f);
  for (std::size_t i = 0; i < excitation.size(); ++i)
    violin_out[i] = violin_body.process(excitation[i]);
  CHECK(fnv1a_quantized(violin_out) == 0x301db9e2991bf7bfull);

  const BodyResonator::Spec shell[3] = {
      {220.0f, 0.080f, 1.00f}, {330.0f, 0.050f, 0.60f}, {480.0f, 0.035f, 0.35f}};
  BodyResonator shell_body;
  shell_body.start_specs(shell, 3, kSr, 0.5f);
  REQUIRE(shell_body.active());
  std::vector<float> shell_out(excitation.size(), 0.0f);
  for (std::size_t i = 0; i < excitation.size(); ++i)
    shell_out[i] = shell_body.process(excitation[i]);
  CHECK(fnv1a_quantized(shell_out) == 0x72eced39118ca79aull);
}
