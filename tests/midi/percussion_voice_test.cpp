/// @file percussion_voice_test.cpp
/// @brief Membrane-modal percussion core (midi/synth/percussion_voice):
///        circular-membrane Bessel mode set, strike-point excitation
///        weighting (centre thump vs rim pitch), and backward-compatible
///        deterministic rendering at strike_r == 0.

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/bessel.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"

namespace {

using sonare::midi::synth::bessel_j;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::SynthEngineMode;
using sonare::midi::synth::SynthFilterOutput;

constexpr double kRate = 48000.0;

sonare::midi::MidiEvent event(const sonare::midi::Ump& ump) {
  sonare::midi::MidiEvent e;
  e.ump = ump;
  return e;
}

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

NativeSynthPatch tom_patch() {
  NativeSynthPatch p{};
  p.mode = SynthEngineMode::kPercussion;
  p.one_shot = true;
  p.cutoff_hz = 20000.0f;
  p.amp_env.attack_ms = 0.5f;
  p.amp_env.decay_ms = 400.0f;
  p.amp_env.sustain = 0.0f;
  p.amp_env.release_ms = 120.0f;
  p.percussion.num_modes = 5;
  p.percussion.base_freq_hz = 150.0f;
  p.percussion.mode_decay_s = 0.4f;
  p.percussion.noise_gain = 0.0f;  // isolate the tone layer for these checks
  return p;
}

}  // namespace

TEST_CASE("bessel_j matches known values and zeros", "[midi][synth][bessel]") {
  // Origin: J_0(0) = 1, J_{m>=1}(0) = 0.
  REQUIRE(bessel_j(0, 0.0f) == Catch::Approx(1.0f));
  REQUIRE(bessel_j(1, 0.0f) == Catch::Approx(0.0f).margin(1e-6));
  REQUIRE(bessel_j(2, 0.0f) == Catch::Approx(0.0f).margin(1e-6));
  REQUIRE(bessel_j(3, 0.0f) == Catch::Approx(0.0f).margin(1e-6));

  // First zeros (the membrane mode alphas) drop the Bessel functions to ~0.
  REQUIRE(bessel_j(0, 2.4048f) == Catch::Approx(0.0f).margin(1e-3));
  REQUIRE(bessel_j(1, 3.8317f) == Catch::Approx(0.0f).margin(1e-3));
  REQUIRE(bessel_j(2, 5.1356f) == Catch::Approx(0.0f).margin(1e-3));
  REQUIRE(bessel_j(3, 6.3802f) == Catch::Approx(0.0f).margin(1e-3));

  // A reference interior value: J_0(1) = 0.7651976866.
  REQUIRE(bessel_j(0, 1.0f) == Catch::Approx(0.7651977f).margin(1e-5));
  // J_1(1) = 0.4400505857.
  REQUIRE(bessel_j(1, 1.0f) == Catch::Approx(0.4400506f).margin(1e-5));
}

TEST_CASE("strike_r == 0 reproduces the legacy percussion output bit-for-bit",
          "[midi][synth][percussion]") {
  NativeSynthPatch legacy = tom_patch();  // strike_r defaults to 0
  NativeSynthPatch explicit_centre = tom_patch();
  explicit_centre.percussion.strike_r = 0.0f;

  const std::vector<float> a = render_patch(legacy, 50, 100, 4096);
  const std::vector<float> b = render_patch(explicit_centre, 50, 100, 4096);
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    REQUIRE(a[i] == b[i]);
  }
}

namespace {

/// Single-frequency magnitude (Goertzel) of a buffer at @p freq_hz.
double goertzel(const std::vector<float>& buf, double freq_hz) {
  const double w = 2.0 * M_PI * freq_hz / kRate;
  const double coeff = 2.0 * std::cos(w);
  double s_prev = 0.0;
  double s_prev2 = 0.0;
  for (float x : buf) {
    const double s = static_cast<double>(x) + coeff * s_prev - s_prev2;
    s_prev2 = s_prev;
    s_prev = s;
  }
  return std::sqrt(s_prev * s_prev + s_prev2 * s_prev2 - coeff * s_prev * s_prev2);
}

}  // namespace

TEST_CASE("a centre strike drops the m>=1 membrane modes to a pitchless thump",
          "[midi][synth][percussion]") {
  // At strike_r -> 0+ only the m == 0 modes (idx 0 and 3) survive; the
  // m >= 1 modes vanish (J_{m>0}(0) = 0), so the centre hit is hollow. The
  // total energy is not the discriminator (the surviving m == 0 modes keep
  // full gain while a rim hit attenuates every mode) -- the mode *content*
  // is. Probe the (1,1) mode at 1.59x the 150 Hz fundamental.
  NativeSynthPatch centre = tom_patch();
  centre.percussion.strike_r = 1.0e-4f;
  NativeSynthPatch rim = tom_patch();
  rim.percussion.strike_r = 0.7f;

  const std::vector<float> c = render_patch(centre, 50, 100, 8192);
  const std::vector<float> r = render_patch(rim, 50, 100, 8192);

  const double base = 150.0;
  const double m0_c = goertzel(c, base);         // (0,1) fundamental
  const double m1_c = goertzel(c, base * 1.59);  // (1,1)
  const double m0_r = goertzel(r, base);
  const double m1_r = goertzel(r, base * 1.59);

  // Centre: the m == 0 fundamental dominates; the m == 1 mode is suppressed.
  REQUIRE(m1_c < 0.05 * m0_c);
  // Rim: the m == 1 mode is excited to a substantial share of the fundamental.
  REQUIRE(m1_r > 0.2 * m0_r);
  // The (1,1) mode is far stronger (relative to its fundamental) at the rim.
  // (The centre floor is spectral leakage from the strong fundamental, not
  // the true mode amplitude, so a 5x separation is the robust bound.)
  REQUIRE((m1_r / m0_r) > 5.0 * (m1_c / m0_c));
}

TEST_CASE("shell_mix == 0 reproduces the legacy percussion output bit-for-bit",
          "[midi][synth][percussion]") {
  NativeSynthPatch dry = tom_patch();  // shell_mix defaults to 0
  NativeSynthPatch explicit_off = tom_patch();
  explicit_off.percussion.shell_mix = 0.0f;
  explicit_off.percussion.shell_num_modes = 2;  // configured but bypassed by mix 0

  const std::vector<float> a = render_patch(dry, 50, 100, 4096);
  const std::vector<float> b = render_patch(explicit_off, 50, 100, 4096);
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    REQUIRE(a[i] == b[i]);
  }
}

TEST_CASE("the shell resonance rings the body at its tuned mode", "[midi][synth][percussion]") {
  // Isolate the shell from the membrane: a noise-only strike (no tone modes)
  // gives the shell broadband content to ring on, as a real drum head feeds
  // its shell, with nothing else sounding at the shell frequency.
  NativeSynthPatch dry{};
  dry.mode = SynthEngineMode::kPercussion;
  dry.one_shot = true;
  dry.cutoff_hz = 20000.0f;
  dry.amp_env.attack_ms = 0.5f;
  dry.amp_env.decay_ms = 400.0f;
  dry.amp_env.sustain = 0.0f;
  dry.amp_env.release_ms = 120.0f;
  dry.percussion.num_modes = 0;  // no membrane tone
  dry.percussion.noise_gain = 0.8f;
  dry.percussion.noise_decay_ms = 40.0f;
  dry.percussion.noise_cutoff_hz = 6000.0f;
  NativeSynthPatch shelled = dry;
  // A lower-Q shell (t60 = 60 ms) has the bandwidth to ring off the broadband
  // burst -- a high-Q body barely couples to noise, as real shells are not.
  shelled.percussion.shell_mix = 0.8f;
  shelled.percussion.shell_num_modes = 1;
  shelled.percussion.shell_freq_hz = {620.0f, 0.0f, 0.0f, 0.0f};
  shelled.percussion.shell_t60_s = {0.06f, 0.0f, 0.0f, 0.0f};
  shelled.percussion.shell_weight = {2.0f, 0.0f, 0.0f, 0.0f};

  const std::vector<float> d = render_patch(dry, 50, 100, 8192);
  const std::vector<float> s = render_patch(shelled, 50, 100, 8192);

  // The shell lifts the band at its tuned 620 Hz over the dry voice, and that
  // band stands proud of an off-resonance reference (300 Hz).
  const double dry_620 = goertzel(d, 620.0);
  const double shell_620 = goertzel(s, 620.0);
  REQUIRE(shell_620 > 1.5 * dry_620);
  REQUIRE(goertzel(s, 620.0) > 1.5 * goertzel(s, 300.0));
  // The unit-peak normalization keeps the mix bounded (no blow-up).
  float peak = 0.0f;
  for (float v : s) peak = std::max(peak, std::fabs(v));
  REQUIRE(peak < 4.0f);
}

namespace {

NativeSynthPatch snare_patch() {
  NativeSynthPatch p = tom_patch();
  p.percussion.base_freq_hz = 185.0f;
  p.percussion.mode_decay_s = 0.12f;
  p.percussion.strike_r = 0.55f;
  return p;  // noise_gain stays 0 so the wire rattle is the only HF source
}

/// Energy above @p cutoff_hz via a crude one-pole high-pass difference.
double hf_energy(const std::vector<float>& buf) {
  double acc = 0.0;
  float prev = 0.0f;
  for (float v : buf) {
    const float hp = v - prev;  // first difference: emphasizes highs
    prev = v;
    acc += static_cast<double>(hp) * hp;
  }
  return acc;
}

}  // namespace

TEST_CASE("wire_buzz == 0 reproduces the legacy percussion output bit-for-bit",
          "[midi][synth][percussion]") {
  NativeSynthPatch dry = snare_patch();  // wire_buzz defaults to 0
  NativeSynthPatch explicit_off = snare_patch();
  explicit_off.percussion.wire_buzz = 0.0f;
  explicit_off.percussion.wire_threshold = 0.05f;  // configured but inert at buzz 0

  const std::vector<float> a = render_patch(dry, 38, 100, 4096);
  const std::vector<float> b = render_patch(explicit_off, 38, 100, 4096);
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    REQUIRE(a[i] == b[i]);
  }
}

TEST_CASE("the snare wire rattle is velocity-dependent and couples to the membrane",
          "[midi][synth][percussion]") {
  NativeSynthPatch buzzing = snare_patch();
  buzzing.percussion.wire_buzz = 1.0f;
  buzzing.percussion.wire_threshold = 0.06f;
  buzzing.percussion.wire_cutoff_hz = 4500.0f;

  // Harder hits rattle louder: the wire gate scales with strike velocity.
  const std::vector<float> soft = render_patch(buzzing, 38, 30, 8192);
  const std::vector<float> hard = render_patch(buzzing, 38, 120, 8192);
  REQUIRE(hf_energy(hard) > 4.0 * hf_energy(soft));

  // The rattle couples to the membrane: with no tone layer there is no head to
  // contact, so the wire adds nothing (bit-identical to wire off).
  NativeSynthPatch no_membrane = buzzing;
  no_membrane.percussion.num_modes = 0;
  NativeSynthPatch no_wire = no_membrane;
  no_wire.percussion.wire_buzz = 0.0f;
  const std::vector<float> a = render_patch(no_membrane, 38, 120, 4096);
  const std::vector<float> b = render_patch(no_wire, 38, 120, 4096);
  for (size_t i = 0; i < a.size(); ++i) {
    REQUIRE(a[i] == b[i]);
  }
}

namespace {

NativeSynthPatch cymbal_patch() {
  NativeSynthPatch p{};
  p.mode = SynthEngineMode::kPercussion;
  p.one_shot = true;
  p.cutoff_hz = 20000.0f;
  p.amp_env.attack_ms = 0.5f;
  p.amp_env.decay_ms = 1400.0f;
  p.amp_env.sustain = 0.0f;
  p.amp_env.release_ms = 400.0f;
  p.percussion.num_modes = 4;
  p.percussion.mode_ratios = {1.0f, 1.34f, 1.72f, 2.15f, 0.0f, 0.0f};
  p.percussion.base_freq_hz = 3600.0f;
  p.percussion.mode_decay_s = 1.1f;
  p.percussion.tone_gain = 0.25f;
  p.percussion.noise_gain = 0.0f;  // no noise layer: the modes are bit-identical
                                   // with and without shimmer, so the diff is
                                   // the pure shimmer wash
  return p;
}

double window_energy(const std::vector<float>& buf, size_t from, size_t to) {
  double acc = 0.0;
  for (size_t i = from; i < to && i < buf.size(); ++i) acc += static_cast<double>(buf[i]) * buf[i];
  return acc;
}

}  // namespace

TEST_CASE("shimmer == 0 reproduces the legacy percussion output bit-for-bit",
          "[midi][synth][percussion]") {
  NativeSynthPatch dry = cymbal_patch();  // shimmer defaults to 0
  NativeSynthPatch explicit_off = cymbal_patch();
  explicit_off.percussion.shimmer = 0.0f;
  explicit_off.percussion.shimmer_attack_ms = 20.0f;  // configured but inert

  const std::vector<float> a = render_patch(dry, 49, 110, 4096);
  const std::vector<float> b = render_patch(explicit_off, 49, 110, 4096);
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    REQUIRE(a[i] == b[i]);
  }
}

TEST_CASE("the cymbal shimmer swells after the strike and rides the ring",
          "[midi][synth][percussion]") {
  NativeSynthPatch dry = cymbal_patch();
  NativeSynthPatch shimmering = cymbal_patch();
  shimmering.percussion.shimmer = 6.0f;
  shimmering.percussion.shimmer_attack_ms = 60.0f;
  shimmering.percussion.shimmer_cutoff_hz = 9000.0f;

  const std::vector<float> d = render_patch(dry, 49, 110, 8192);
  const std::vector<float> s = render_patch(shimmering, 49, 110, 8192);

  // The modes are identical with and without shimmer, so the difference is the
  // pure shimmer wash.
  std::vector<float> wash(s.size(), 0.0f);
  for (size_t i = 0; i < s.size(); ++i) wash[i] = s[i] - d[i];

  // Growth: the shimmer builds *after* the strike -- a mid window (60-90 ms)
  // carries more wash energy than the first 30 ms, unlike a struck-then-decay
  // source.
  const double early = window_energy(wash, 0, 1440);
  const double mid = window_energy(wash, 2880, 4320);
  REQUIRE(mid > early);

  // The wash is genuinely high-frequency (above the 3600 Hz mode set) and
  // adds audible energy the dry cymbal lacks.
  const std::vector<float> wash_mid(wash.begin() + 2880, wash.begin() + 4320);
  REQUIRE(goertzel(wash_mid, 11000.0) > 3.0 * goertzel(wash_mid, 2000.0));
  REQUIRE(window_energy(s, 2880, 4320) > 1.5 * window_energy(d, 2880, 4320));

  // One-way pump: the shimmer stays bounded (no nonlinear blow-up).
  float peak = 0.0f;
  for (float v : s) peak = std::max(peak, std::fabs(v));
  REQUIRE(peak < 4.0f);
}

// ---------------------------------------------------------------------------
// Radiated upper bound (noise_air_hz) over the burst / wire / shimmer streams.
// ---------------------------------------------------------------------------

namespace {

/// A cymbal that actually runs all the bounded streams: a high-passed burst
/// under a shimmer wash, plus enough wire buzz to put the third stream in the
/// sum.
NativeSynthPatch washing_cymbal_patch() {
  NativeSynthPatch p = cymbal_patch();
  p.percussion.noise_gain = 1.0f;
  p.percussion.noise_decay_ms = 900.0f;
  p.percussion.noise_cutoff_hz = 5500.0f;
  p.percussion.noise_output = SynthFilterOutput::kHighpass;
  p.percussion.shimmer = 6.0f;
  p.percussion.shimmer_cutoff_hz = 9000.0f;
  p.percussion.wire_buzz = 0.5f;
  p.percussion.wire_cutoff_hz = 4000.0f;
  return p;
}

}  // namespace

TEST_CASE("noise_air_hz == 0 reproduces the legacy percussion output bit-for-bit",
          "[midi][synth][percussion]") {
  NativeSynthPatch unbounded = washing_cymbal_patch();  // noise_air_hz defaults to 0
  NativeSynthPatch explicit_off = washing_cymbal_patch();
  explicit_off.percussion.noise_air_hz = 0.0f;

  const std::vector<float> a = render_patch(unbounded, 49, 110, 8192);
  const std::vector<float> b = render_patch(explicit_off, 49, 110, 8192);
  REQUIRE(a.size() == b.size());
  double energy = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    REQUIRE(a[i] == b[i]);
    energy += static_cast<double>(a[i]) * a[i];
  }
  // Two silences also match sample for sample; the streams have to be sounding
  // for the match to be evidence of anything.
  REQUIRE(energy > 0.0);
}

TEST_CASE("the radiated bound removes the wash's tail without moving its corner",
          "[midi][synth][percussion]") {
  NativeSynthPatch open = washing_cymbal_patch();
  NativeSynthPatch bounded = washing_cymbal_patch();
  bounded.percussion.noise_air_hz = 9000.0f;  // well above the 5500 Hz corner

  const std::vector<float> o = render_patch(open, 49, 110, 8192);
  const std::vector<float> b = render_patch(bounded, 49, 110, 8192);

  // A high-pass has no top, so the unbounded wash is still white an octave
  // above anything the patch names. That is the tail the bound is for.
  REQUIRE(goertzel(b, 16000.0) < 0.35 * goertzel(o, 16000.0));

  // Placed above the corner, the bound is a ceiling and not a re-voicing: what
  // the corner passes just below it survives.
  REQUIRE(goertzel(b, 6000.0) > 0.6 * goertzel(o, 6000.0));

  // ...and it is a bound, not a switch: raising it puts the tail back.
  NativeSynthPatch higher = washing_cymbal_patch();
  higher.percussion.noise_air_hz = 16000.0f;
  const std::vector<float> h = render_patch(higher, 49, 110, 8192);
  REQUIRE(goertzel(h, 16000.0) > goertzel(b, 16000.0));
}

TEST_CASE("the radiated bound leaves the particle layer alone", "[midi][synth][percussion]") {
  // PhISEM is a separate excitation model with its own resonance stage, so the
  // bound deliberately does not reach it. A shaker with no burst of its own
  // must render bit-identically whatever the bound says.
  NativeSynthPatch shaker;
  shaker.mode = SynthEngineMode::kPercussion;
  shaker.one_shot = true;
  shaker.percussion.num_modes = 0;
  shaker.percussion.noise_gain = 0.0f;
  shaker.percussion.phisem_beans = 24.0f;
  shaker.percussion.phisem_res_hz = 6000.0f;

  NativeSynthPatch bounded = shaker;
  bounded.percussion.noise_air_hz = 2000.0f;

  const std::vector<float> a = render_patch(shaker, 82, 110, 8192);
  const std::vector<float> b = render_patch(bounded, 82, 110, 8192);
  REQUIRE(a.size() == b.size());
  double energy = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    REQUIRE(a[i] == b[i]);
    energy += static_cast<double>(a[i]) * a[i];
  }
  REQUIRE(energy > 0.0);  // the layer really is sounding, so the match means something
}

namespace {

/// A bare PhISEM shaker: no membrane, no noise burst, one bright band.
NativeSynthPatch shaker_patch() {
  NativeSynthPatch p{};
  p.mode = SynthEngineMode::kPercussion;
  p.one_shot = true;
  p.cutoff_hz = 20000.0f;
  p.percussion.num_modes = 0;
  p.percussion.noise_gain = 0.0f;
  p.percussion.phisem_beans = 24.0f;
  p.percussion.phisem_energy_ms = 120.0f;
  p.percussion.phisem_res_hz = 3000.0f;
  p.percussion.phisem_res_q = 2.0f;
  return p;
}

}  // namespace

TEST_CASE("phisem_body_gain == 0 reproduces the legacy particle output bit-for-bit",
          "[midi][synth][percussion]") {
  const NativeSynthPatch legacy = shaker_patch();
  NativeSynthPatch configured = legacy;
  configured.percussion.phisem_body_hz = 285.0f;  // set but silenced by gain 0
  configured.percussion.phisem_body_q = 5.5f;
  configured.percussion.phisem_body_gain = 0.0f;

  const std::vector<float> a = render_patch(legacy, 73, 110, 8192);
  const std::vector<float> b = render_patch(configured, 73, 110, 8192);
  REQUIRE(a.size() == b.size());
  double energy = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    REQUIRE(a[i] == b[i]);
    energy += static_cast<double>(a[i]) * a[i];
  }
  REQUIRE(energy > 0.0);  // the layer really is sounding, so the match means something
}

TEST_CASE("the particle body rings well below the band the collisions radiate",
          "[midi][synth][percussion]") {
  const NativeSynthPatch dry = shaker_patch();
  NativeSynthPatch gourd = dry;
  gourd.percussion.phisem_body_hz = 285.0f;
  gourd.percussion.phisem_body_q = 5.5f;
  gourd.percussion.phisem_body_gain = 0.4f;

  const std::vector<float> a = render_patch(dry, 73, 110, 8192);
  const std::vector<float> b = render_patch(gourd, 73, 110, 8192);

  // The body lifts its own centre by a wide margin and leaves the bright band
  // where it was: the two paths are parallel, so neither passes through the
  // other.
  REQUIRE(goertzel(b, 285.0) > goertzel(a, 285.0) * 4.0);
  REQUIRE(goertzel(b, 3000.0) == Catch::Approx(goertzel(a, 3000.0)).epsilon(0.02));

  // Two pole pairs, not one. An octave under the centre a single pair of this Q
  // is down 18.4 dB and the cascade twice that, so a threshold between them
  // fails if the second pair ever stops being applied. Read against the same
  // octave of the dry render, which carries the bright band's own skirt.
  const double centre = goertzel(b, 285.0) - goertzel(a, 285.0);
  const double octave_down = goertzel(b, 142.5) - goertzel(a, 142.5);
  REQUIRE(octave_down > 0.0);
  REQUIRE(20.0 * std::log10(centre / octave_down) > 26.0);
}

// ---------------------------------------------------------------------------
// GM/GS drum map: per-note archetypes + exclusive (mute-group) choke.
// ---------------------------------------------------------------------------

namespace {

/// A `drum-kit` patch: note-on resolves each struck key through the GM drum map
/// instead of playing one fixed piece.
NativeSynthPatch drum_kit_patch() {
  NativeSynthPatch p{};
  p.mode = SynthEngineMode::kPercussion;
  p.one_shot = true;
  p.percussion.gm_kit = true;
  p.cutoff_hz = 20000.0f;
  return p;
}

/// Renders a gm_kit engine, striking (sample_offset, note) pairs (given in
/// non-decreasing offset order), and returns the mono (left) output. A non-zero
/// @p program selects a GS drum kit via a program change before the strikes.
std::vector<float> render_kit(const std::vector<std::pair<int, uint8_t>>& strikes, int num_samples,
                              uint8_t program = 0) {
  NativeSynthConfig cfg;
  cfg.patch = drum_kit_patch();
  NativeSynth synth(cfg);
  synth.prepare(kRate, 512);
  if (program != 0) {
    synth.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, program)));
  }
  std::vector<float> left(static_cast<size_t>(num_samples), 0.0f);
  std::vector<float> right(static_cast<size_t>(num_samples), 0.0f);
  int pos = 0;
  auto advance = [&](int to) {
    while (pos < to) {
      const int seg = std::min(512, to - pos);
      float* chans[2] = {left.data() + pos, right.data() + pos};
      synth.process(chans, 2, seg);
      pos += seg;
    }
  };
  for (const auto& s : strikes) {
    advance(s.first);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, s.second, 100)));
  }
  advance(num_samples);
  return left;
}

double window_rms(const std::vector<float>& x, int from, int to) {
  double acc = 0.0;
  for (int i = from; i < to; ++i) acc += static_cast<double>(x[i]) * x[i];
  return std::sqrt(acc / std::max(1, to - from));
}

}  // namespace

TEST_CASE("every GS drum key resolves to an audible instrument", "[midi][synth][percussion]") {
  // The per-note map must leave no GM/GS drum key (27..87) silent.
  for (uint8_t note = 27; note <= 87; ++note) {
    const std::vector<float> out = render_kit({{0, note}}, 12000);
    INFO("drum key " << static_cast<int>(note));
    REQUIRE(window_rms(out, 0, 12000) > 1.0e-4);
  }
}

TEST_CASE("GS drum keys differ in sustain by archetype", "[midi][synth][percussion]") {
  // Distinct archetypes ring for distinct lengths: open triangle sustains for
  // over a second while its mute twin and the claves die within ~150 ms. This
  // proves the keys resolve to different voicings, not one shared bucket.
  const int n = 32000;  // ~0.67 s
  auto tail = [&](uint8_t note) {
    return window_rms(render_kit({{0, note}}, n), 20000, 28000);  // ~0.42..0.58 s
  };
  const double open_triangle = tail(81);
  const double mute_triangle = tail(80);
  const double claves = tail(75);
  REQUIRE(open_triangle > 1.0e-4);
  REQUIRE(mute_triangle < open_triangle * 0.1);
  REQUIRE(claves < open_triangle * 0.1);
}

TEST_CASE("the cowbell voices its two clangy partials", "[midi][synth][percussion]") {
  // TR-808-informed cowbell: two tones near 587 Hz and 845 Hz (ratio ~1.44)
  // dominate over the surrounding spectrum.
  const std::vector<float> bell = render_kit({{0, 56}}, 8000);
  const double low = goertzel(bell, 587.0);
  const double high = goertzel(bell, 845.0);
  const double off = goertzel(bell, 300.0);
  REQUIRE(low > 3.0 * off);
  REQUIRE(high > 1.5 * off);
}

TEST_CASE("a closed hi-hat chokes the ringing open hi-hat", "[midi][synth][percussion]") {
  // Exclusive mute group 1: a closed/pedal strike cuts the open hat's tail.
  const int n = 24000;  // 0.5 s
  const std::vector<float> open_only = render_kit({{0, 46}}, n);
  // The closed strike lands early (~63 ms) so its own short burst has decayed
  // well before the measurement window, isolating the choked open-hat tail.
  const std::vector<float> choked = render_kit({{0, 46}, {3000, 42}}, n);
  const double base_tail = window_rms(open_only, 12000, 18000);  // ~0.25..0.375 s
  const double choked_tail = window_rms(choked, 12000, 18000);
  REQUIRE(base_tail > 1.0e-3);             // the open hat really is still ringing
  REQUIRE(choked_tail < base_tail * 0.2);  // the closed strike choked it
}

TEST_CASE("phisem_beans == 0 reproduces the legacy percussion output bit-for-bit",
          "[midi][synth][percussion]") {
  NativeSynthPatch dry = tom_patch();  // phisem_beans defaults to 0
  NativeSynthPatch explicit_off = tom_patch();
  explicit_off.percussion.phisem_beans = 0.0f;
  explicit_off.percussion.phisem_res_hz = 3000.0f;  // configured but bypassed by beans 0
  explicit_off.percussion.phisem_scrape_hz = 100.0f;

  const std::vector<float> a = render_patch(dry, 50, 100, 4096);
  const std::vector<float> b = render_patch(explicit_off, 50, 100, 4096);
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    REQUIRE(a[i] == b[i]);
  }
}

TEST_CASE("the PhISEM shaker is audible, bounded and decays", "[midi][synth][percussion]") {
  // Maracas: one shake gesture -> a burst of bead collisions that dies out.
  const int n = 24000;  // 0.5 s
  const std::vector<float> mar = render_kit({{0, 70}}, n);
  const double early = window_rms(mar, 0, 4000);
  const double late = window_rms(mar, 16000, 24000);
  REQUIRE(early > 1.0e-3);      // the shake sounds
  REQUIRE(late < 0.3 * early);  // the system energy dies out
  float peak = 0.0f;
  for (float v : mar) peak = std::max(peak, std::fabs(v));
  REQUIRE(peak < 4.0f);  // the statistical sum stays bounded
}

TEST_CASE("a long guiro scrape sustains past a short one", "[midi][synth][percussion]") {
  // The scrape ridge train runs as long as the shake energy lasts: the long
  // guiro (500 ms) is still scraping in a window where the short one (120 ms)
  // has died.
  const int n = 32000;  // ~0.67 s
  const std::vector<float> shortg = render_kit({{0, 73}}, n);
  const std::vector<float> longg = render_kit({{0, 74}}, n);
  const double short_tail = window_rms(shortg, 12000, 20000);  // ~0.25..0.42 s
  const double long_tail = window_rms(longg, 12000, 20000);
  REQUIRE(long_tail > 2.0 * short_tail);
}

TEST_CASE("the TR-808 kit gives the kick a long decaying-sine tail", "[midi][synth][percussion]") {
  // Program 25 = TR-808 kit: the kick becomes a low, long decaying sine.
  const int n = 24000;  // 0.5 s
  const std::vector<float> std_kick = render_kit({{0, 36}}, n, 0);
  const std::vector<float> kick808 = render_kit({{0, 36}}, n, 25);
  const double std_tail = window_rms(std_kick, 10000, 18000);
  const double tail808 = window_rms(kick808, 10000, 18000);
  REQUIRE(tail808 > 3.0 * std_tail);  // the 808 kick still booms where Standard has died
}

TEST_CASE("the Power kit hits the kick harder than Standard", "[midi][synth][percussion]") {
  // Program 16 = Power (Rock) kit: bigger, louder shells.
  const int n = 12000;
  const std::vector<float> std_kick = render_kit({{0, 36}}, n, 0);
  const std::vector<float> power = render_kit({{0, 36}}, n, 16);
  float power_peak = 0.0f;
  float std_peak = 0.0f;
  for (float v : power) power_peak = std::max(power_peak, std::fabs(v));
  for (float v : std_kick) std_peak = std::max(std_peak, std::fabs(v));
  REQUIRE(power_peak > std_peak);
}

TEST_CASE("an unknown drum program leaves the Standard kit bit-identical",
          "[midi][synth][percussion]") {
  // Program 3 is not a GS kit slot -> Standard; must match the no-program render
  // sample-for-sample (kit 0 skips the transform entirely).
  const int n = 8000;
  const std::vector<float> no_prog = render_kit({{0, 38}}, n, 0);
  const std::vector<float> prog3 = render_kit({{0, 38}}, n, 3);
  REQUIRE(no_prog.size() == prog3.size());
  for (size_t i = 0; i < no_prog.size(); ++i) {
    REQUIRE(no_prog[i] == prog3[i]);
  }
}
