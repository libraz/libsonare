/// @file brass_voice_test.cpp
/// @brief Sustained brass / lip-reed waveguide (midi/synth/brass_voice):
///        fundamental tuning, the lip resonance locking the buzz to the note,
///        the full harmonic series (a brass radiates all harmonics, unlike the
///        odd-only clarinet), prompt speech + steady sustain, note-off
///        ring-down, unconditional stability across the keyboard and dynamics
///        and both bore topologies, lip-tension pitch bend, and deterministic
///        rendering.

#include "midi/synth/brass_voice.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "core/fft.h"
#include "midi/midi_event.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/synth_presets.h"
#include "midi/ump.h"
#include "support/audio_fixtures.h"

namespace {

using sonare::midi::MidiEvent;
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

double spectral_centroid(const std::vector<float>& buf, size_t from) {
  const std::vector<double> ps = power_spectrum(buf, from);
  double num = 0.0;
  double den = 0.0;
  for (size_t b = 1; b < ps.size(); ++b) {
    num += static_cast<double>(b) * kRate / kFft * ps[b];
    den += ps[b];
  }
  return den > 0.0 ? num / den : 0.0;
}

/// A filter-bypassed brass test patch (raw bore, no body resonance so the pitch
/// and harmonic measurements read the air column, not a bell formant EQ).
NativeSynthPatch brass_base_patch() {
  NativeSynthPatch p;
  p.mode = SynthEngineMode::kBrass;
  p.cutoff_hz = 20000.0f;
  p.amp_env.attack_ms = 5.0f;
  p.amp_env.sustain = 1.0f;
  p.amp_env.release_ms = 100.0f;
  p.brass.breath_pressure = 0.8f;
  p.brass.vel_to_breath = 0.5f;
  p.brass.lip_tension = 0.5f;
  p.brass.lip_damping = 0.5f;
  p.brass.brightness = 0.5f;
  p.brass.damping = 0.3f;
  p.brass.conical = false;  // cylindrical (trumpet / trombone)
  return p;
}

}  // namespace

TEST_CASE("brass rendering is deterministic", "[midi][synth][brass]") {
  const NativeSynthPatch patch = brass_base_patch();
  const std::vector<float> first = render_patch(patch, 53, 100, 16384);
  const std::vector<float> second = render_patch(patch, 53, 100, 16384);
  REQUIRE(peak(first) > 0.01f);
  REQUIRE(first == second);
}

TEST_CASE("brass is unconditionally stable", "[midi][synth][brass]") {
  // Across the keyboard, dynamics and both bore topologies, the brass loop must
  // stay bounded and finite.
  for (bool conical : {false, true}) {
    for (uint8_t note : {29, 41, 53, 65, 77, 89}) {
      for (uint8_t velocity : {40, 100, 127}) {
        NativeSynthPatch patch = brass_base_patch();
        patch.brass.conical = conical;
        const std::vector<float> tone = render_patch(patch, note, velocity, 48000);
        REQUIRE(peak(tone) < 4.0f);
        REQUIRE(std::isfinite(tone.back()));
      }
    }
  }
}

TEST_CASE("brass is stable at extreme lip damping and breath", "[midi][synth][brass]") {
  for (float damp : {0.0f, 0.5f, 1.0f}) {
    for (float breath : {0.2f, 1.0f}) {
      NativeSynthPatch patch = brass_base_patch();
      patch.brass.lip_damping = damp;
      patch.brass.breath_pressure = breath;
      patch.brass.damping = 0.1f;  // least loop loss (hardest to hold bounded)
      const std::vector<float> tone = render_patch(patch, 53, 120, 48000);
      REQUIRE(peak(tone) < 4.0f);
      REQUIRE(std::isfinite(tone.back()));
    }
  }
}

TEST_CASE("brass tuning is accurate", "[midi][synth][brass]") {
  // The lip resonance locks to the note; the outward-striking sharpness is
  // corrected, so the played fundamental lands within a couple of percent.
  for (bool conical : {false, true}) {
    NativeSynthPatch patch = brass_base_patch();
    patch.brass.conical = conical;
    for (const auto& [note, expected] :
         {std::pair<uint8_t, double>{48, 130.8128}, std::pair<uint8_t, double>{55, 195.9977},
          std::pair<uint8_t, double>{60, 261.6256}}) {
      const std::vector<float> tone = render_patch(patch, note, 110, 48000);
      const double estimated = fft_fundamental(tone, 16000, expected);
      REQUIRE(std::fabs(estimated / expected - 1.0) < 0.02);
    }
  }
}

TEST_CASE("brass lip resonance locks the buzz to the fundamental", "[midi][synth][brass]") {
  // The note must speak at its fundamental, not jump to the octave or a mistuned
  // inter-harmonic mode: the fundamental is the dominant partial.
  const double f0 = 174.6141;  // F3
  NativeSynthPatch patch = brass_base_patch();
  const std::vector<float> tone = render_patch(patch, 53, 110, 24000);
  const std::vector<double> ps = power_spectrum(tone, 8000);
  const double h1 = harmonic_power(ps, f0, 1);
  REQUIRE(h1 > 0.0);
  REQUIRE(h1 > harmonic_power(ps, f0, 2));  // fundamental dominates the octave
  REQUIRE(h1 > harmonic_power(ps, f0, 3));
  // No sub-octave (a register jump down would put energy at f0/2).
  REQUIRE(harmonic_power(ps, f0, 1) > 8.0 * harmonic_power(ps, 0.5 * f0, 1));
}

TEST_CASE("brass radiates the full harmonic series", "[midi][synth][brass]") {
  // A brass tube driven by the lip valve radiates the FULL harmonic series (the
  // octave and above carry real energy), unlike a clarinet's odd-only cylinder —
  // the lip nonlinearity injects the even harmonics into the bore.
  const double f0 = 130.8128;  // C3
  NativeSynthPatch patch = brass_base_patch();
  const std::vector<float> tone = render_patch(patch, 48, 110, 24000);
  const std::vector<double> ps = power_spectrum(tone, 8000);
  const double h1 = harmonic_power(ps, f0, 1);
  REQUIRE(h1 > 0.0);
  // The second harmonic (the octave) carries real energy — the signature of a
  // full-harmonic (brass) spectrum rather than an odd-only one.
  REQUIRE(harmonic_power(ps, f0, 2) > 0.02 * h1);
}

TEST_CASE("brass sustains a steady tone", "[midi][synth][brass]") {
  // A held note is self-sustained: the late window is comparable to the early
  // window (it does not decay while the breath is on).
  const NativeSynthPatch patch = brass_base_patch();
  const std::vector<float> tone = render_patch(patch, 53, 100, 48000);
  const float early = rms(tone, 6000, 12000);
  const float late = rms(tone, 40000, 46000);
  REQUIRE(early > 0.01f);
  REQUIRE(late > 0.5f * early);
}

TEST_CASE("brass rings down after note-off", "[midi][synth][brass]") {
  const NativeSynthPatch patch = brass_base_patch();
  const std::vector<float> tone = render_patch(patch, 53, 100, 48000, 20000);
  const float sounding = rms(tone, 12000, 18000);
  const float after = rms(tone, 40000, 46000);
  REQUIRE(sounding > 0.01f);
  REQUIRE(after < 0.5f * sounding);
}

TEST_CASE("brass lip tension bends the pitch", "[midi][synth][brass]") {
  // Tightening the embouchure (lip_tension) raises the lip resonance, so the
  // played note bends up.
  const double f0 = 174.6141;  // F3
  NativeSynthPatch loose = brass_base_patch();
  loose.brass.lip_tension = 0.0f;
  NativeSynthPatch tight = brass_base_patch();
  tight.brass.lip_tension = 1.0f;
  const double low = fft_fundamental(render_patch(loose, 53, 100, 48000), 16000, f0);
  const double high = fft_fundamental(render_patch(tight, 53, 100, 48000), 16000, f0);
  REQUIRE(high > low);
}

TEST_CASE("brass CC2 breath drives the mouth pressure", "[midi][synth][brass]") {
  // CC2 rides the lip's stable buzzing band: more breath pushes the tone toward
  // the buzzing edge (louder / brighter) without ever silencing the lips.
  const NativeSynthPatch patch = brass_base_patch();
  const std::vector<float> soft = render_cc_change(patch, 53, 100, 24000, 24000, 2, 10);
  const std::vector<float> hard = render_cc_change(patch, 53, 100, 24000, 24000, 2, 127);
  const float soft_rms = rms(soft, 36000, 48000);
  const float hard_rms = rms(hard, 36000, 48000);
  REQUIRE(soft_rms > 0.0f);
  REQUIRE(hard_rms > soft_rms);
}

TEST_CASE("brass CC74 brightness is wired to the sounding voice", "[midi][synth][brass]") {
  // The bell brightness is a subtle control on the (dark) linear bore, so rather
  // than assert a strong spectral shift, confirm the CC reaches the voice: a note
  // seeded dark vs bright renders a different, still-bounded tone.
  const NativeSynthPatch patch = brass_base_patch();
  const std::vector<float> dark = render_with_initial_cc(patch, 53, 100, 24000, 74, 0);
  const std::vector<float> bright = render_with_initial_cc(patch, 53, 100, 24000, 74, 127);
  REQUIRE(peak(dark) < 4.0f);
  REQUIRE(peak(bright) < 4.0f);
  REQUIRE(std::isfinite(bright.back()));
  REQUIRE(dark != bright);  // the control changes the sound
}

TEST_CASE("brass CC74 preserves the conical darkening on the live path", "[midi][synth][brass]") {
  // The conical bell bias must apply to live CC74 updates, not only the note-on
  // seed: under an identical live brightness slam, a conical bore stays darker
  // than a cylindrical one. Before the live path carried the bias the two
  // converged to the same pole and this contrast collapsed.
  NativeSynthPatch cyl = brass_base_patch();  // cylindrical (conical = false)
  NativeSynthPatch con = brass_base_patch();
  con.brass.conical = true;

  const std::vector<float> cyl_live = render_cc_change(cyl, 53, 100, 8000, 32000, 74, 110);
  const std::vector<float> con_live = render_cc_change(con, 53, 100, 8000, 32000, 74, 110);
  REQUIRE(peak(con_live) < 4.0f);
  REQUIRE(std::isfinite(con_live.back()));
  // Measure well past the CC change at sample 8000, in the sustained tail.
  REQUIRE(spectral_centroid(cyl_live, 16000) > spectral_centroid(con_live, 16000));
}

TEST_CASE("brass presets speak and stay bounded", "[midi][synth][brass]") {
  // Every catalog brass preset must resolve, sound, and stay bounded across the
  // brass range.
  for (const char* name : {"trumpet", "trombone", "tuba", "french-horn", "muted-trumpet", "cornet",
                           "flugelhorn", "euphonium"}) {
    const sonare::midi::synth::SynthPreset* preset = sonare::midi::synth::find_synth_preset(name);
    REQUIRE(preset != nullptr);
    REQUIRE(preset->config.patch.mode == SynthEngineMode::kBrass);
    NativeSynth synth(preset->config);
    synth.prepare(kRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 53, 100)));
    const std::vector<float> tone = render_left(synth, 40000);
    INFO(name);
    REQUIRE(peak(tone) < 4.0f);
    REQUIRE(std::isfinite(tone.back()));
    REQUIRE(rms(tone, 24000, 40000) > 0.002f);  // it actually sounds
  }
}

TEST_CASE("brass is stable under a rapid breath sweep", "[midi][synth][brass]") {
  NativeSynthConfig cfg;
  cfg.patch = brass_base_patch();
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 53, 100)));
  std::vector<float> out;
  for (int block = 0; block < 48; ++block) {
    const uint8_t cc = static_cast<uint8_t>((block % 2 == 0) ? 5 : 127);  // slam CC2 up/down
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 2, cc)));
    const std::vector<float> chunk = render_left(synth, 1000);
    out.insert(out.end(), chunk.begin(), chunk.end());
  }
  REQUIRE(peak(out) < 4.0f);
  REQUIRE(std::isfinite(out.back()));
}

TEST_CASE("advanced brass gates are off by default (bit-identical)", "[midi][synth][brass]") {
  // The Phase-4 gates default to 0, so a default patch renders exactly as it did
  // before they existed (the off-path is skipped entirely).
  NativeSynthPatch base = brass_base_patch();
  NativeSynthPatch same = brass_base_patch();
  same.brass.brassiness = 0.0f;
  same.brass.mute = 0.0f;
  same.brass.half_valve = 0.0f;
  same.brass.dynamic_lip = 0.0f;
  REQUIRE(render_patch(base, 53, 100, 24000) == render_patch(same, 53, 100, 24000));
}

TEST_CASE("cuivre brightens the brass tone", "[midi][synth][brass]") {
  // Turning up brassiness blooms the upper harmonics (the shock), lifting the
  // spectral centroid, while staying bounded.
  NativeSynthPatch dark = brass_base_patch();
  NativeSynthPatch bright = brass_base_patch();
  bright.brass.brassiness = 1.0f;
  const std::vector<float> off = render_patch(dark, 53, 110, 40000);
  const std::vector<float> on = render_patch(bright, 53, 110, 40000);
  REQUIRE(peak(on) < 4.0f);
  REQUIRE(std::isfinite(on.back()));
  REQUIRE(spectral_centroid(on, 24000) > spectral_centroid(off, 24000));
}

TEST_CASE("cuivre dynamics gate is off by default (bit-identical)", "[midi][synth][brass]") {
  // With brassiness on, leaving cuivre_dynamics at its default (0) selects the
  // static shock shaper, so the render is bit-identical to an explicit 0.
  NativeSynthPatch base = brass_base_patch();
  base.brass.brassiness = 0.8f;
  NativeSynthPatch same = base;
  same.brass.cuivre_dynamics = 0.0f;
  REQUIRE(render_patch(base, 53, 100, 24000) == render_patch(same, 53, 100, 24000));
}

TEST_CASE("cuivre dynamics brightens ff over pp", "[midi][synth][brass]") {
  // With the dynamics gate on, a hard (ff) note blooms the shock more than a soft
  // (pp) note, so the ff/pp spectral-centroid contrast is wider than the static
  // shaper's (which brightens both dynamics equally). Centroid is gain-invariant,
  // so this measures spectral shape, not the amp VCA level.
  NativeSynthPatch stat = brass_base_patch();
  stat.brass.brassiness = 0.8f;
  stat.brass.vel_to_breath = 0.7f;
  NativeSynthPatch dyn = stat;
  dyn.brass.cuivre_dynamics = 1.0f;

  const int n = 40000;
  const double c_stat_pp = spectral_centroid(render_patch(stat, 53, 30, n), 24000);
  const double c_stat_ff = spectral_centroid(render_patch(stat, 53, 120, n), 24000);
  const double c_dyn_pp = spectral_centroid(render_patch(dyn, 53, 30, n), 24000);
  const double c_dyn_ff = spectral_centroid(render_patch(dyn, 53, 120, n), 24000);

  REQUIRE(c_dyn_ff > c_dyn_pp);                          // ff is brighter than pp
  REQUIRE(c_dyn_ff / c_dyn_pp > c_stat_ff / c_stat_pp);  // the gate widens the contrast
}

TEST_CASE("mute makes the brass tone nasal", "[midi][synth][brass]") {
  // A mute reshapes the bell radiation into a bright, nasal honk (a strong upper
  // formant), lifting the centroid well above the open tone.
  NativeSynthPatch open = brass_base_patch();
  NativeSynthPatch muted = brass_base_patch();
  muted.brass.mute = 1.0f;
  const std::vector<float> off = render_patch(open, 65, 110, 40000);
  const std::vector<float> on = render_patch(muted, 65, 110, 40000);
  REQUIRE(peak(on) < 4.0f);
  REQUIRE(std::isfinite(on.back()));
  REQUIRE(spectral_centroid(on, 24000) > 1.2 * spectral_centroid(off, 24000));
}

TEST_CASE("brass bell body brightens the tone", "[midi][synth][brass]") {
  // The kBrassBell radiation formant lifts the ~1.2 kHz region over the round
  // linear bore, raising the spectral centroid while staying bounded.
  NativeSynthPatch plain = brass_base_patch();
  NativeSynthPatch belled = brass_base_patch();
  belled.body = sonare::midi::synth::BodyType::kBrassBell;
  belled.body_mix = 0.5f;
  const std::vector<float> off = render_patch(plain, 65, 110, 40000);
  const std::vector<float> on = render_patch(belled, 65, 110, 40000);
  REQUIRE(peak(on) < 4.0f);
  REQUIRE(std::isfinite(on.back()));
  REQUIRE(spectral_centroid(on, 24000) > spectral_centroid(off, 24000));
}

TEST_CASE("half-valve and dynamic lip alter the tone but stay bounded", "[midi][synth][brass]") {
  const NativeSynthPatch base = brass_base_patch();
  const std::vector<float> plain = render_patch(base, 53, 100, 24000);
  for (int which = 0; which < 2; ++which) {
    NativeSynthPatch patch = brass_base_patch();
    const char* label = which == 0 ? "half_valve" : "dynamic_lip";
    if (which == 0)
      patch.brass.half_valve = 1.0f;
    else
      patch.brass.dynamic_lip = 1.0f;
    const std::vector<float> tone = render_patch(patch, 53, 100, 24000);
    INFO(label);
    REQUIRE(peak(tone) < 4.0f);
    REQUIRE(std::isfinite(tone.back()));
    REQUIRE(tone != plain);  // the gate changes the sound
  }
}

TEST_CASE("all advanced brass gates compose stably", "[midi][synth][brass]") {
  // Every Phase-4 gate at once, across the keyboard and both topologies, must
  // stay bounded and finite.
  for (bool conical : {false, true}) {
    for (uint8_t note : {29, 41, 53, 65, 77, 89}) {
      NativeSynthPatch patch = brass_base_patch();
      patch.brass.conical = conical;
      patch.brass.brassiness = 0.9f;
      patch.brass.mute = 0.8f;
      patch.brass.half_valve = 0.7f;
      patch.brass.dynamic_lip = 0.8f;
      const std::vector<float> tone = render_patch(patch, note, 120, 48000);
      REQUIRE(peak(tone) < 4.0f);
      REQUIRE(std::isfinite(tone.back()));
    }
  }
}
