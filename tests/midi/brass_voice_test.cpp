/// @file brass_voice_test.cpp
/// @brief Sustained brass / lip-reed waveguide (midi/synth/brass_voice):
///        fundamental tuning, the lip resonance locking the buzz to the note,
///        the full harmonic series (a brass radiates all harmonics, unlike the
///        odd-only clarinet), prompt speech + steady sustain, note-off
///        ring-down, unconditional stability across the keyboard and dynamics
///        and both bore topologies, lip-tension pitch bend, and deterministic
///        rendering.

#include "midi/synth/brass_voice.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <utility>
#include <vector>

#include "core/fft.h"
#include "midi/midi_event.h"
#include "midi/synth/excitation_axes.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/synth_presets.h"
#include "midi/ump.h"
#include "support/audio_fixtures.h"
#include "support/midi_render.h"
#include "util/constants.h"

namespace {

using sonare::midi::MidiEvent;
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

using sonare::test::fft_fundamental;
using sonare::test::harmonic_power;
using sonare::test::power_spectrum;
using sonare::test::spectral_centroid;

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

TEST_CASE("catalog brass radiates through a bell, like the fallback voices",
          "[midi][synth][brass]") {
  // The catalogue and the fallback table are two address spaces over one set of
  // voices, so a catalogue entry left on the bore pressure while its fallback
  // twin radiates is a defect in the entry rather than a voicing choice. Read as
  // equality against the named twin rather than as "some value is set": the way
  // this went wrong was a re-fit moving the fallback and nothing pulling the
  // catalogue after it, which a presence check cannot see. The last three have
  // no program of their own and take the class their bell belongs to.
  struct Row {
    const char* preset;
    uint8_t program;
  };
  for (const Row row : {Row{"trumpet", 56},
                        {"trombone", 57},
                        {"tuba", 58},
                        {"muted-trumpet", 59},
                        {"french-horn", 60},
                        {"cornet", 56},
                        {"flugelhorn", 56},
                        {"euphonium", 58}}) {
    const sonare::midi::synth::SynthPreset* preset =
        sonare::midi::synth::find_synth_preset(row.preset);
    REQUIRE(preset != nullptr);
    const NativeSynthPatch& got = preset->config.patch;
    const NativeSynthPatch want = gm_fallback_patch(0, row.program);
    INFO(row.preset << " against GM program " << static_cast<int>(row.program));
    REQUIRE(want.brass.bell_cutoff_hz > 0.0f);  // the twin this row is read against
    REQUIRE(got.brass.bell_cutoff_hz == want.brass.bell_cutoff_hz);
    REQUIRE(got.cutoff_hz == want.cutoff_hz);  // fitted with the corner, not after it
    REQUIRE(got.brass.lip_aperture == want.brass.lip_aperture);
    REQUIRE(got.brass.brassiness == want.brass.brassiness);
    REQUIRE(got.brass.cuivre_dynamics == want.brass.cuivre_dynamics);
    REQUIRE(got.brass.bore_nonlinearity == want.brass.bore_nonlinearity);
    REQUIRE(got.brass.dynamic_lip == want.brass.dynamic_lip);
  }
}

TEST_CASE("the fallback brass voices radiate from the coupled bell alone", "[midi][synth][brass]") {
  // Two bell models share the engine and they are alternatives rather than a
  // pair: the standalone radiation highpass is consulted only where the coupled
  // corner is zero. Every voice here sets the corner, so the highpass is
  // unreachable for all of them — asserted by feeding it a value and reading the
  // render rather than by reading the branch, since the branch is what a later
  // edit would change. A voice that loses its corner fails here instead of
  // silently falling back to a filter nobody is maintaining.
  for (uint8_t program = 56; program <= 61; ++program) {
    const NativeSynthPatch shipped = gm_fallback_patch(0, program);
    INFO("program " << static_cast<int>(program));
    REQUIRE(shipped.mode == SynthEngineMode::kBrass);
    REQUIRE(shipped.brass.bell_cutoff_hz > 0.0f);
    NativeSynthPatch probe = shipped;
    probe.brass.bell_radiation_hz = 4000.0f;
    REQUIRE(render_patch(shipped, 53, 100, 24000) == render_patch(probe, 53, 100, 24000));
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
  same.brass.bell_radiation_hz = 0.0f;
  same.brass.brassiness = 0.0f;
  same.brass.mute = 0.0f;
  same.brass.half_valve = 0.0f;
  same.brass.dynamic_lip = 0.0f;
  REQUIRE(render_patch(base, 53, 100, 24000) == render_patch(same, 53, 100, 24000));
}

TEST_CASE("bell radiation lifts the partials over the fundamental", "[midi][synth][brass]") {
  // What a bell radiates is what it did not reflect, so the emitted field is the
  // complement of the loop lowpass. The bore pressure the core emits without it
  // is a near-sine whose fundamental stands over every partial; a reference brass
  // puts its formant on those partials instead.
  const double f0 = 174.6141;  // F3
  NativeSynthPatch bore = brass_base_patch();
  NativeSynthPatch radiated = brass_base_patch();
  radiated.brass.bell_radiation_hz = 900.0f;
  const std::vector<double> off = power_spectrum(render_patch(bore, 53, 100, 40000), 24000);
  const std::vector<float> on_buf = render_patch(radiated, 53, 100, 40000);
  const std::vector<double> on = power_spectrum(on_buf, 24000);
  REQUIRE(harmonic_power(on, f0, 4) / harmonic_power(on, f0, 1) >
          4.0 * harmonic_power(off, f0, 4) / harmonic_power(off, f0, 1));
  REQUIRE(peak(on_buf) < 4.0f);
}

TEST_CASE("bell radiation holds a formant the note moves under", "[midi][synth][brass]") {
  // The bore pressure's centroid is a multiple of the note, so an octave up moves
  // it an octave. The flare cutoff does not move with the note, so the radiated
  // centroid moves less — which is the register behaviour the references show and
  // the reason no brightness knob could reach the deficit.
  NativeSynthPatch bore = brass_base_patch();
  NativeSynthPatch radiated = brass_base_patch();
  radiated.brass.bell_radiation_hz = 900.0f;
  const double bore_octave = spectral_centroid(render_patch(bore, 65, 100, 40000), 24000) /
                             spectral_centroid(render_patch(bore, 53, 100, 40000), 24000);
  const double radiated_octave = spectral_centroid(render_patch(radiated, 65, 100, 40000), 24000) /
                                 spectral_centroid(render_patch(radiated, 53, 100, 40000), 24000);
  REQUIRE(bore_octave > 1.7);
  REQUIRE(radiated_octave < bore_octave);
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

TEST_CASE("the lip valve opens and shuts", "[midi][synth][brass]") {
  // What a brass bore carries is made by the lips, not by the bell, so this is
  // read on the bore with the radiation stage and the cuivre shaper both off.
  //
  // The quantity is the second harmonic's share of the fundamental, read across
  // the rest aperture rather than at one setting. A single "open is brighter
  // than shut" comparison would be the wrong shape twice over: the ratio is not
  // monotonic in aperture, and a purely linear stage moves it by more than ten
  // decibels without creating a single harmonic, so its value at one point says
  // nothing about what made it.
  //
  // What only a one-sided valve can produce is the shape. A valve clamped at
  // both ends is cut off below when it rests nearly shut and above when it
  // rests nearly open, so somewhere between the two its flow waveform is at its
  // most symmetric and the even harmonics cancel. That notch is structural. A
  // gain, a filter and a monotone shaper all lack it, and the symmetric path
  // this replaces ignores the aperture altogether, leaving the response flat.
  //
  // Three decibels is the margin because the same ratio moves 0.851 dB across
  // two octaves, 0.211 dB across the whole velocity range and 0.000 dB between
  // repeated renders, and because the hand-written shaper this is meant to make
  // derivable only moves it 1.73 dB. The floor is measured, not assumed, and it
  // is re-measurable: the sibling case tagged [null] prints it. That the closed
  // side did NOT move is not claimed here, because there is nothing in this
  // case to compare it against: the golden manifests say it, against values
  // recorded before the valve.
  const double f0 = 261.6256;                         // C4, note 60
  NativeSynthPatch patch = gm_fallback_patch(0, 56);  // Trumpet
  REQUIRE(patch.mode == SynthEngineMode::kBrass);
  patch.brass.brassiness = 0.0f;
  // Both bell corners, because they are alternatives rather than a pair: the
  // standalone radiation highpass is only consulted when the complementary
  // reflection/radiation cutoff is zero, so zeroing one of them alone leaves
  // the other filtering the very ratio this case reads.
  patch.brass.bell_radiation_hz = 0.0f;
  patch.brass.bell_cutoff_hz = 0.0f;
  patch.cutoff_hz = 20000.0f;

  // Steady portion only: 0.3 s to 0.8 s, averaged over four Hann frames so one
  // unlucky frame cannot carry the verdict.
  const auto ratios_db = [&](float aperture) {
    NativeSynthPatch p = patch;
    p.brass.lip_aperture = aperture;
    const std::vector<float> tone = render_patch(p, 60, 100, 48000);
    REQUIRE(std::isfinite(tone.back()));
    double h1 = 0.0, h2 = 0.0, h4 = 0.0;
    for (int frame = 0; frame < 4; ++frame) {
      const std::size_t from = 14400 + static_cast<std::size_t>(frame) * 5269;
      const std::vector<double> ps = power_spectrum(tone, from);
      h1 += harmonic_power(ps, f0, 1);
      h2 += harmonic_power(ps, f0, 2);
      h4 += harmonic_power(ps, f0, 4);
    }
    REQUIRE(h1 > 0.0);
    const auto db = [h1](double h) { return 10.0 * std::log10(h / h1 + 1e-30); };
    return std::pair<double, double>{db(h2), db(h4)};
  };

  // 0 is the off sentinel and takes the old symmetric path.
  const double off = ratios_db(0.0f).first;
  std::vector<double> h2_db;
  std::ostringstream trace;
  trace << "off " << off << " dB |";
  for (int step = 1; step <= 20; ++step) {
    const float aperture = static_cast<float>(step) * 0.05f;
    const auto r = ratios_db(aperture);
    h2_db.push_back(r.first);
    trace << ' ' << aperture << ':' << r.first;
  }
  INFO(trace.str());

  double reach = 0.0;
  for (double v : h2_db) reach = std::max(reach, std::abs(v - off));
  INFO("furthest from off: " << reach << " dB");

  const double ends = std::min(h2_db.front(), h2_db.back());
  double notch = 0.0;
  for (std::size_t i = 1; i + 1 < h2_db.size(); ++i) notch = std::max(notch, ends - h2_db[i]);
  INFO("deepest interior notch below the shallower end: " << notch << " dB");

  REQUIRE(reach >= 3.0);
  REQUIRE(notch >= 3.0);
}

TEST_CASE("bore harmonic ratios against causes that are not the lip", "[.][midi][synth][null]") {
  // Not a gate — it reports rather than asserts. A margin on h2/h1 is only
  // meaningful against the spread the same quantity already shows when the lip
  // valve is held fixed, so this measures that spread: repeated renders, the
  // individual analysis frames, velocity, note, and the two stages that sit
  // after the bore. Neither stage can create a harmonic, but a linear filter
  // moves the ratio, which is why they belong in the null rather than outside it.
  const auto hz = [](int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); };
  NativeSynthPatch base = gm_fallback_patch(0, 56);  // Trumpet
  REQUIRE(base.mode == SynthEngineMode::kBrass);
  const float gm_brassiness = base.brass.brassiness;
  // The corner this voice actually uses. The standalone radiation highpass is
  // consulted only when the complementary reflection/radiation cutoff is zero,
  // and this voice sets the cutoff, so reading the highpass would report a
  // field the render never looks at and leave the bell in both arms.
  const float gm_bell_hz = base.brass.bell_cutoff_hz;
  base.brass.brassiness = 0.0f;
  base.brass.bell_radiation_hz = 0.0f;
  base.brass.bell_cutoff_hz = 0.0f;
  base.cutoff_hz = 20000.0f;

  // Per-frame ratios over the same 0.3-0.8 s steady window the gated case reads.
  const auto frames_db = [&](const NativeSynthPatch& p, uint8_t note, uint8_t vel) {
    const std::vector<float> tone = render_patch(p, note, vel, 48000);
    REQUIRE(std::isfinite(tone.back()));
    const double f0 = hz(note);
    std::vector<std::pair<double, double>> out;
    for (int frame = 0; frame < 4; ++frame) {
      const std::size_t from = 14400 + static_cast<std::size_t>(frame) * 5269;
      const std::vector<double> ps = power_spectrum(tone, from);
      const double h1 = harmonic_power(ps, f0, 1);
      REQUIRE(h1 > 0.0);
      const auto db = [h1](double h) { return 10.0 * std::log10(h / h1 + 1e-30); };
      out.emplace_back(db(harmonic_power(ps, f0, 2)), db(harmonic_power(ps, f0, 4)));
    }
    return out;
  };
  const auto mean_db = [](const std::vector<std::pair<double, double>>& f) {
    double a = 0.0, b = 0.0;
    for (const auto& x : f) {
      a += x.first;
      b += x.second;
    }
    return std::pair<double, double>{a / static_cast<double>(f.size()),
                                     b / static_cast<double>(f.size())};
  };
  const auto report = [&](const char* label, const NativeSynthPatch& p, uint8_t note, uint8_t vel) {
    const auto f = frames_db(p, note, vel);
    const auto m = mean_db(f);
    double lo2 = f[0].first, hi2 = f[0].first, lo4 = f[0].second, hi4 = f[0].second;
    for (const auto& x : f) {
      lo2 = std::min(lo2, x.first);
      hi2 = std::max(hi2, x.first);
      lo4 = std::min(lo4, x.second);
      hi4 = std::max(hi4, x.second);
    }
    WARN(label << "  h2/h1 " << m.first << " dB (frame span " << (hi2 - lo2) << ")"
               << "  h4/h1 " << m.second << " dB (frame span " << (hi4 - lo4) << ")"
               << "  centroid " << spectral_centroid(render_patch(p, note, vel, 48000), 14400)
               << " Hz");
    return m;
  };

  const auto a = report("repeat 1        note 60 vel 100", base, 60, 100);
  const auto b = report("repeat 2        note 60 vel 100", base, 60, 100);
  WARN("run-to-run  h2 " << (b.first - a.first) << " dB  h4 " << (b.second - a.second) << " dB");

  for (uint8_t vel : {uint8_t{32}, uint8_t{64}, uint8_t{100}, uint8_t{127}}) {
    report("velocity                note 60", base, 60, vel);
  }
  for (uint8_t note : {uint8_t{48}, uint8_t{60}, uint8_t{72}}) {
    report("note                    vel 100", base, note, 100);
  }

  NativeSynthPatch shaped = base;
  shaped.brass.brassiness = gm_brassiness;
  report("brassiness at its GM value      ", shaped, 60, 100);
  NativeSynthPatch radiated = base;
  radiated.brass.bell_cutoff_hz = gm_bell_hz;
  WARN("bell_cutoff_hz GM value " << gm_bell_hz << " Hz, brassiness GM value " << gm_brassiness);
  report("bell_cutoff_hz at its GM value   ", radiated, 60, 100);

  // The two corners a real flare and a bore with no flare would give. Whether
  // these already separate decides whether a radiation test needs the lip valve
  // in front of it to be able to fail.
  for (float corner : {1000.0f, 13000.0f}) {
    NativeSynthPatch p = base;
    p.brass.bell_cutoff_hz = corner;
    report(
        corner < 2000.0f ? "bell corner 1 kHz               " : "bell corner 13 kHz              ",
        p, 60, 100);
  }
}

TEST_CASE("brightness against the note, for causes that are not bore propagation",
          "[.][midi][synth][null]") {
  // Not a gate — it reports rather than asserts. An amplitude-dependent
  // propagation term modulates the whole bore delay by a fixed FRACTION of the
  // period, so the modulation in samples grows with the bore length while the
  // phase index it produces does not: the period cancels, and every note sees
  // the same index. The note slope below is therefore the engine's own — the
  // cuivre drive is scaled by a pitch-dependent factor, the output filters sit
  // at fixed hertz, and the lip valve is a nonlinearity inside the loop.
  // Read at the shipping patch rather than with the shaper switched off, since
  // that is the configuration the term has to improve on.
  const auto hz = [](int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); };
  NativeSynthPatch shipped = gm_fallback_patch(0, 56);  // Trumpet
  REQUIRE(shipped.mode == SynthEngineMode::kBrass);
  // The control arm zeroes the term rather than reading a patch that happens to
  // have it off: this voice now ships with it on.
  shipped.brass.bore_nonlinearity = 0.0f;
  REQUIRE(shipped.brass.cuivre_dynamics > 0.0f);  // the shaper tracks the live envelope

  // Centroid over the fundamental: the note carries the raw centroid with it
  // almost exactly (48/60/72 measured at 178.9 / 351.4 / 698.1 Hz), so only the
  // ratio can be compared across notes.
  const auto harmonic_centroid = [&](const NativeSynthPatch& p, uint8_t note, uint8_t vel) {
    const std::vector<float> tone = render_patch(p, note, vel, 48000);
    REQUIRE(std::isfinite(tone.back()));
    return static_cast<double>(spectral_centroid(tone, 14400)) / hz(note);
  };
  const auto note_ratio = [&](const char* label, const NativeSynthPatch& p, uint8_t vel) {
    const double low = harmonic_centroid(p, 48, vel);
    const double mid = harmonic_centroid(p, 60, vel);
    const double high = harmonic_centroid(p, 72, vel);
    WARN(label << "  h-centroid  note 48 " << low << "  note 60 " << mid << "  note 72 " << high
               << "   ratio 48/72 " << (low / high));
    return low / high;
  };
  const auto pct = [](double a, double b) { return 100.0 * std::fabs(a - b) / std::fabs(b); };

  const double v64 = note_ratio("shipped, velocity 64  ", shipped, 64);
  const double v64b = note_ratio("shipped, velocity 64  ", shipped, 64);
  WARN("run-to-run moves the ratio by " << pct(v64b, v64) << " %");
  const double v127 = note_ratio("shipped, velocity 127 ", shipped, 127);
  WARN("velocity 64 -> 127 moves the ratio by " << pct(v127, v64) << " %");

  NativeSynthPatch no_cuivre = shipped;
  no_cuivre.brass.brassiness = 0.0f;
  no_cuivre.brass.cuivre_dynamics = 0.0f;
  const double valve = note_ratio("cuivre off, velocity 64", no_cuivre, 64);
  WARN("the shaper accounts for " << pct(v64, valve) << " % of the shipped ratio; what is left is "
                                  << "the lip valve and the bell");

  WARN(
      "the lumped form multiplies the note-48 modulation by 4.0 against note 72 in SAMPLES, "
      "but the phase index it produces is 2*pi*k*span*p and carries no period term at all, "
      "so the factor on the index is 1.0 and this ratio is not a propagation statistic");

  // How the same ratio responds across the propagation term's whole range. The
  // delay modulation is phase modulation, whose individual sideband amplitudes
  // follow Bessel functions and are NOT monotonic in the index -- but their
  // power-weighted first moment is (2.21 f0 at an index of 2, 6.98 at 10), so a
  // centroid that falls is never the Bessel oscillation. Read as the shape of
  // the curve rather than as an extrapolation from two points.
  for (float depth : {0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f, 1.0f}) {
    NativeSynthPatch p = shipped;
    p.brass.bore_nonlinearity = depth;
    std::ostringstream label;
    label << "bore_nonlinearity " << depth << " ";
    const double r = note_ratio(label.str().c_str(), p, 64);
    WARN("   moved from the term-off ratio by " << pct(r, v64) << " %");
  }

  // The absolute brightening at each note, which the ratio above divides the
  // common part out of, and how it grows with the played dynamic -- the
  // literature defines brassiness as the RATE of spectral enrichment with
  // level, so a term modelling it should widen as the dynamic rises.
  const auto hc = [&](const NativeSynthPatch& p, uint8_t note, uint8_t vel) {
    const std::vector<float> tone = render_patch(p, note, vel, 48000);
    REQUIRE(std::isfinite(tone.back()));
    return static_cast<double>(spectral_centroid(tone, 14400)) / hz(note);
  };
  NativeSynthPatch full = shipped;
  full.brass.bore_nonlinearity = 1.0f;
  for (uint8_t vel : {uint8_t{64}, uint8_t{100}, uint8_t{127}}) {
    const double lo_off = hc(shipped, 48, vel), lo_on = hc(full, 48, vel);
    const double hi_off = hc(shipped, 72, vel), hi_on = hc(full, 72, vel);
    WARN("velocity " << static_cast<int>(vel) << "  note 48 brightens " << pct(lo_on, lo_off)
                     << " %   note 72 brightens " << pct(hi_on, hi_off) << " %   ratio of the two "
                     << ((lo_on / lo_off - 1.0) / std::max(1e-9, hi_on / hi_off - 1.0)));
  }
  // Yardsticks for the same absolute quantity at note 48, with the term off.
  WARN("note 48 yardsticks with the term off: velocity 64->127 moves it "
       << pct(hc(shipped, 48, 127), hc(shipped, 48, 64)) << " %, cuivre on->off "
       << pct(hc(shipped, 48, 64), hc(no_cuivre, 48, 64)) << " %");

  // The term's own response to the played dynamic, with the shaper out of the
  // way. The loop holds its pressure near one level at every velocity -- the amp
  // VCA carries the dynamic, not the breath -- so the modulator moves only as
  // far as the mouth pressure does, which is 7.6 % from velocity 64 to 127.
  NativeSynthPatch bare_on = no_cuivre;
  bare_on.brass.bore_nonlinearity = 1.0f;
  for (uint8_t note : {uint8_t{48}, uint8_t{72}}) {
    for (uint8_t vel : {uint8_t{32}, uint8_t{64}, uint8_t{100}, uint8_t{127}}) {
      const double off = hc(no_cuivre, note, vel), on = hc(bare_on, note, vel);
      WARN("shaper off  note " << static_cast<int>(note) << " velocity " << static_cast<int>(vel)
                               << "  base " << off << "  increment " << (on - off) << " ("
                               << pct(on, off) << " %)");
    }
    WARN("shaper off  note " << static_cast<int>(note) << "  ff/mp centroid contrast: term off "
                             << (hc(no_cuivre, note, 127) / hc(no_cuivre, note, 64)) << "  term on "
                             << (hc(bare_on, note, 127) / hc(bare_on, note, 64)));
  }

  // Whether the shaper is what masks the term at a loud velocity. With
  // cuivre_dynamics off the effective brassiness IS the patch field, and the
  // live shaper's steady state is 0.165 + 0.693*vel01^2 -- so replaying those
  // three values at one velocity holds the term's own pressure fixed and moves
  // only the shaper. If the three rows reproduce the three velocities above,
  // the falloff is the shaper's rather than the loop's.
  for (float b_eff : {0.3410f, 0.5947f, 0.8580f}) {
    NativeSynthPatch replay = shipped;
    replay.brass.cuivre_dynamics = 0.0f;
    replay.brass.brassiness = b_eff;
    NativeSynthPatch replay_on = replay;
    replay_on.brass.bore_nonlinearity = 1.0f;
    const double off = hc(replay, 48, 64), on = hc(replay_on, 48, 64);
    WARN("velocity 64 held, shaper replayed at b_eff "
         << b_eff << "  base " << off << "  increment " << (on - off) << " (" << pct(on, off)
         << " %)");
  }

  // The centroid is blind to the sign of the pressure dependence, so read the
  // slope asymmetry of the raw bore output instead: peaks arriving sooner than
  // troughs steepen the rising edge, which is a ratio above one.
  NativeSynthPatch raw = no_cuivre;
  raw.brass.bell_radiation_hz = 0.0f;
  raw.brass.bell_cutoff_hz = 0.0f;
  raw.cutoff_hz = 20000.0f;
  raw.body = sonare::midi::synth::BodyType::kNone;
  raw.body_mix = 0.0f;
  const auto slope_ratio = [&](const NativeSynthPatch& p) {
    const std::vector<float> y = render_patch(p, 48, 127, 48000);
    REQUIRE(std::isfinite(y.back()));
    double up = 0.0, down = 0.0;
    for (size_t i = 14401; i < y.size(); ++i) {
      const double d = static_cast<double>(y[i]) - static_cast<double>(y[i - 1]);
      up = std::max(up, d);
      down = std::min(down, d);
    }
    return up / std::max(1.0e-12, -down);
  };
  const double s_off = slope_ratio(raw);
  WARN("raw bore slope asymmetry (rise/fall): term off " << s_off << "  repeat " << slope_ratio(raw)
                                                         << "  (the lip "
                                                         << "valve's own asymmetry, and its null)");
  // Swept rather than read at full depth: the first-order direction is the sign
  // of the propagation, while at full depth the waveform is reshaped enough that
  // this ratio stops reading an edge and starts reading the sidebands.
  for (float depth : {0.0625f, 0.125f, 0.25f, 0.5f, 1.0f}) {
    NativeSynthPatch raw_on = raw;
    raw_on.brass.bore_nonlinearity = depth;
    WARN("   depth " << depth << " -> " << slope_ratio(raw_on) << "  (moved "
                     << pct(slope_ratio(raw_on), s_off) << " %)");
  }
}

namespace {

using sonare::midi::synth::BrassPatchParams;
using sonare::midi::synth::BrassVoiceCore;
using sonare::midi::synth::ExcitationAxes;
using sonare::midi::synth::kAxisBrightness;

/// Runs a brass core standalone and reports the two bell coefficients it ends
/// up on. @p cc74 below 0 leaves the note at the patch's own brightness;
/// otherwise it is sent after the note has started, which is the path a moving
/// controller takes.
BrassVoiceCore::BellCoefficients bell_coefficients(const BrassPatchParams& params, double sr,
                                                   uint8_t note, float cc74 = -1.0f) {
  std::vector<float> slab(16384, 0.0f);
  BrassVoiceCore core;
  core.attach(slab.data(), static_cast<int>(slab.size()));
  core.start(params, sr, note, 100, 1u);
  if (cc74 >= 0.0f) {
    ExcitationAxes axes;
    axes.brightness = cc74;
    core.set_excitation_base(axes, kAxisBrightness);
  }
  // Long enough for the control ramp to settle: it is a one-pole of a few
  // milliseconds, and this is tens of time constants at every rate tested.
  const int samples = static_cast<int>(sr * 0.5);
  for (int i = 0; i < samples; ++i) core.render(1.0f);
  return core.bell_coefficients();
}

/// The hertz corner a one-pole coefficient stands for. Computed here rather
/// than read from the engine so the check does not share its source with what
/// it checks.
double corner_hz(float alpha, double sr) {
  return -std::log(1.0 - static_cast<double>(alpha)) * sr / sonare::constants::kTwoPiD;
}

BrassPatchParams bell_base_params() {
  BrassPatchParams p;
  p.breath_pressure = 0.8f;
  p.vel_to_breath = 0.5f;
  p.lip_tension = 0.5f;
  p.lip_damping = 0.5f;
  p.brightness = 0.5f;
  p.damping = 0.3f;
  p.conical = false;
  return p;
}

}  // namespace

TEST_CASE("one bell corner drives both of the bell's filters", "[midi][synth][brass]") {
  // A bell reflects what it does not radiate, so the two filters are one
  // object. Giving each its own corner lets a fit reach a bell that reflects
  // and radiates at unrelated frequencies, which no flare does.
  //
  // This reads the coefficients rather than the sound because the sound cannot
  // separate them: both act on the same signal. That the pairing is
  // unreachable rather than merely unused is the grep in this unit's checks,
  // not something a run can show.
  BrassPatchParams dark = bell_base_params();
  dark.bell_cutoff_hz = 1200.0f;
  BrassPatchParams bright = bell_base_params();
  bright.bell_cutoff_hz = 9000.0f;

  const auto a = bell_coefficients(dark, 48000.0, 60);
  const auto b = bell_coefficients(bright, 48000.0, 60);
  INFO("reflect " << a.reflect_alpha << " -> " << b.reflect_alpha);
  INFO("radiate " << a.radiate_alpha << " -> " << b.radiate_alpha);
  REQUIRE(b.reflect_alpha > a.reflect_alpha * 1.5f);
  REQUIRE(b.radiate_alpha > a.radiate_alpha * 1.5f);

  // The live path. rad_alpha_ used to be written only by start(), so a bell
  // that moves under CC74 moved one of its two halves.
  const auto closed = bell_coefficients(dark, 48000.0, 60, 0.1f);
  const auto opened = bell_coefficients(dark, 48000.0, 60, 0.9f);
  INFO("live reflect " << closed.reflect_alpha << " -> " << opened.reflect_alpha);
  INFO("live radiate " << closed.radiate_alpha << " -> " << opened.radiate_alpha);
  REQUIRE(opened.reflect_alpha > closed.reflect_alpha * 1.5f);
  REQUIRE(opened.radiate_alpha > closed.radiate_alpha * 1.5f);
}

TEST_CASE("the bell corner is a frequency, not a coefficient", "[midi][synth][brass]") {
  // The coefficient is expected to differ at each rate; the corner it stands
  // for is not. Requiring the coefficient to hold still would be green on the
  // defect this guards and red on the fix.
  BrassPatchParams params = bell_base_params();
  params.bell_cutoff_hz = 2400.0f;
  double reference = 0.0;
  for (double sr : {24000.0, 48000.0, 96000.0}) {
    const auto c = bell_coefficients(params, sr, 60);
    const double reflect = corner_hz(c.reflect_alpha, sr);
    const double radiate = corner_hz(c.radiate_alpha, sr);
    INFO("sr " << sr << " reflect " << reflect << " Hz radiate " << radiate << " Hz");
    REQUIRE(std::fabs(radiate / reflect - 1.0) < 0.01);
    if (reference == 0.0)
      reference = reflect;
    else
      REQUIRE(std::fabs(reflect / reference - 1.0) < 0.01);
  }
}

TEST_CASE("the bell corner does not move with the note", "[midi][synth][brass]") {
  // A regression guard rather than a success condition: a bell is a fixed piece
  // of brass, and today's mapping already has no note term. It exists so that
  // naming the corner in hertz does not quietly introduce one.
  BrassPatchParams params = bell_base_params();
  params.bell_cutoff_hz = 2400.0f;
  const double low = corner_hz(bell_coefficients(params, 48000.0, 41).reflect_alpha, 48000.0);
  const double high = corner_hz(bell_coefficients(params, 48000.0, 65).reflect_alpha, 48000.0);
  INFO("note 41 " << low << " Hz, note 65 " << high << " Hz");
  REQUIRE(std::fabs(high / low - 1.0) < 0.01);
}

namespace {

/// Renders the trumpet with the lip valve open and the cuivre shaper off, and
/// reports where the radiated energy sits. The bell is the only thing allowed
/// to differ between calls.
double bell_centroid_hz(float corner) {
  NativeSynthPatch p = gm_fallback_patch(0, 56);
  p.brass.brassiness = 0.0f;
  p.cutoff_hz = 20000.0f;
  p.brass.lip_aperture = 0.7f;
  p.brass.bell_cutoff_hz = corner;
  return spectral_centroid(render_patch(p, 60, 100, 48000), 14400);
}

}  // namespace

TEST_CASE("closing the bell darkens the radiated sound", "[midi][synth][brass]") {
  // The audible half, read as where the radiated energy sits rather than as one
  // harmonic's share of the fundamental.
  //
  // A harmonic ratio is the wrong lens for a bell that is ONE object: what the
  // flare stops radiating it starts reflecting, so the bore loses the partial
  // at the same time the radiation stage stops attenuating it, and the two
  // nearly cancel. Measured, h4/h1 moves 1.00 dB across the whole corner range
  // and is flat above 4 kHz, which is not a quantity anything should be gated
  // on. The centroid does not cancel, because it reads the redistribution both
  // effects agree about.
  //
  // The margin is a ratio because the centroid scales with the note. Its floor
  // is measured by the sibling case tagged [null]: repeated renders move it
  // 0.000%, the whole velocity range 0.159%, and the hand-written shaper the
  // bell competes with 11.3%. A factor of 1.5 is roughly half an octave — well
  // clear of all three, and audible rather than merely detectable.
  const double flared = bell_centroid_hz(1000.0f);  // a real flare
  const double open = bell_centroid_hz(13000.0f);   // what a bore with no bell keeps
  INFO("centroid " << flared << " Hz -> " << open << " Hz");
  REQUIRE(open >= flared * 1.5);
}

TEST_CASE("radiated energy against the bell corner", "[.][midi][synth][null]") {
  // Not a gate. Where the margin in the case above comes from, and the record
  // that a harmonic ratio saturates while the centroid does not.
  const double f0 = 261.6256;  // C4, note 60
  NativeSynthPatch patch = gm_fallback_patch(0, 56);
  patch.brass.brassiness = 0.0f;
  patch.cutoff_hz = 20000.0f;
  patch.brass.lip_aperture = 0.7f;
  for (float corner : {1000.0f, 2000.0f, 4000.0f, 8000.0f, 13000.0f}) {
    NativeSynthPatch p = patch;
    p.brass.bell_cutoff_hz = corner;
    const std::vector<float> tone = render_patch(p, 60, 100, 48000);
    double h1 = 0.0, h2 = 0.0, h4 = 0.0, h8 = 0.0;
    for (int frame = 0; frame < 4; ++frame) {
      const std::size_t from = 14400 + static_cast<std::size_t>(frame) * 5269;
      const std::vector<double> ps = power_spectrum(tone, from);
      h1 += harmonic_power(ps, f0, 1);
      h2 += harmonic_power(ps, f0, 2);
      h4 += harmonic_power(ps, f0, 4);
      h8 += harmonic_power(ps, f0, 8);
    }
    const auto db = [](double a, double b) { return 10.0 * std::log10(a / b + 1e-30); };
    WARN(corner << " Hz  h2/h1 " << db(h2, h1) << "  h4/h1 " << db(h4, h1) << "  h8/h1 "
                << db(h8, h1) << "  rms " << 20.0 * std::log10(rms(tone, 14400, 38400) + 1e-30)
                << "  centroid " << spectral_centroid(tone, 14400) << " Hz");
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

namespace {

/// Spectral centroid over the fundamental. The note carries the raw centroid
/// with it, so only this normalised form compares across notes.
double brass_harmonic_centroid(const NativeSynthPatch& p, uint8_t note, uint8_t velocity) {
  const double hz = 440.0 * std::pow(2.0, (static_cast<int>(note) - 69) / 12.0);
  const std::vector<float> tone = render_patch(p, note, velocity, 48000);
  REQUIRE(std::isfinite(tone.back()));
  return static_cast<double>(spectral_centroid(tone, 14400)) / hz;
}

}  // namespace

TEST_CASE("bore nonlinearity gate is off by default (bit-identical)", "[midi][synth][brass]") {
  NativeSynthPatch patch = brass_base_patch();
  REQUIRE(!(patch.brass.bore_nonlinearity > 0.0f));
  const std::vector<float> plain = render_patch(patch, 53, 100, 16384);
  patch.brass.bore_nonlinearity = 0.0f;
  REQUIRE(render_patch(patch, 53, 100, 16384) == plain);
}

TEST_CASE("bore nonlinearity brightens a loud note more than a soft one", "[midi][synth][brass]") {
  // The literature defines brassiness as the RATE at which the spectrum enriches
  // with the dynamic level, so the term is read as the ff/mp contrast of the
  // normalised centroid and not as any single note's brightness. The shaper is
  // switched off here on purpose: it carries a dynamics response of its own
  // (1.256 on the shipping patch) and masks this term by 3.8x at ff, so the
  // shipping composite cannot separate the two. The bar is one and a half times
  // the largest cause that is not this term, in the statistic's own units:
  // measured on the control arm before the term existed, the contrast is 1.019
  // at note 48 and a repeat render reproduces it exactly, so everything else in
  // the engine contributes 0.019 of contrast and the bar is 0.029.
  NativeSynthPatch off = gm_fallback_patch(0, 56);  // Trumpet
  REQUIRE(off.mode == SynthEngineMode::kBrass);
  // The control arm zeroes the term rather than reading a patch that happens to
  // have it off: this voice now ships with it on, so the shipped value is the
  // treatment here and not the control.
  off.brass.bore_nonlinearity = 0.0f;
  off.brass.brassiness = 0.0f;
  off.brass.cuivre_dynamics = 0.0f;
  NativeSynthPatch on = off;
  on.brass.bore_nonlinearity = 1.0f;

  const double contrast_off =
      brass_harmonic_centroid(off, 48, 127) / brass_harmonic_centroid(off, 48, 64);
  const double contrast_on =
      brass_harmonic_centroid(on, 48, 127) / brass_harmonic_centroid(on, 48, 64);
  INFO("ff/mp centroid contrast: term off " << contrast_off << "  term on " << contrast_on
                                            << "  gain " << (contrast_on - contrast_off));
  REQUIRE(contrast_on - contrast_off >= 0.029);

  // Signed and ordered, because a contrast gain alone cannot tell a term that
  // follows the dynamic from one that merely moves with it.
  double previous = 0.0;
  for (uint8_t velocity : {uint8_t{32}, uint8_t{64}, uint8_t{100}, uint8_t{127}}) {
    const double lift =
        brass_harmonic_centroid(on, 48, velocity) / brass_harmonic_centroid(off, 48, velocity);
    INFO("velocity " << static_cast<int>(velocity) << " lifts the centroid by " << lift);
    REQUIRE(lift > previous);
    previous = lift;
  }
}
