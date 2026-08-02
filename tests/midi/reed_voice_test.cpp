/// @file reed_voice_test.cpp
/// @brief Sustained reed-woodwind waveguide (midi/synth/reed_voice):
///        fundamental tuning, the cylinder's odd-harmonic (clarinet) vs the
///        cone's full-harmonic (sax/oboe) spectrum, prompt speech + steady
///        sustain, note-off ring-down, unconditional stability across the
///        keyboard and dynamics, and deterministic rendering.

#include "midi/synth/reed_voice.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "core/fft.h"
#include "midi/midi_event.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/synth_presets.h"
#include "midi/ump.h"
#include "support/alloc_guard.h"
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

double swell_centroid(const std::vector<float>& buf, size_t from) {
  const std::vector<double> ps = power_spectrum(buf, from);
  double num = 0.0;
  double den = 0.0;
  for (size_t b = 1; b < ps.size(); ++b) {
    num += static_cast<double>(b) * kRate / kFft * ps[b];
    den += ps[b];
  }
  return den > 0.0 ? num / den : 0.0;
}

/// A filter-bypassed reed test patch (raw bore, no body resonance so the pitch
/// and harmonic measurements read the air column, not a formant EQ).
NativeSynthPatch reed_base_patch() {
  NativeSynthPatch p;
  p.mode = SynthEngineMode::kReed;
  p.cutoff_hz = 20000.0f;
  p.amp_env.attack_ms = 5.0f;
  p.amp_env.sustain = 1.0f;
  p.amp_env.release_ms = 100.0f;
  p.reed.breath_pressure = 0.7f;
  p.reed.reed_stiffness = 0.5f;
  p.reed.reed_opening = 0.5f;
  p.reed.brightness = 0.5f;
  p.reed.damping = 0.3f;
  p.reed.conical = false;  // clarinet cylinder
  return p;
}

}  // namespace

TEST_CASE("reed rendering is deterministic", "[midi][synth][reed]") {
  const NativeSynthPatch patch = reed_base_patch();
  const std::vector<float> first = render_patch(patch, 50, 100, 8192);
  const std::vector<float> second = render_patch(patch, 50, 100, 8192);
  REQUIRE(peak(first) > 0.01f);
  REQUIRE(first == second);
}

TEST_CASE("reed is unconditionally stable", "[midi][synth][reed]") {
  // Across the keyboard, dynamics and both bore topologies, the reed loop must
  // stay bounded.
  for (bool conical : {false, true}) {
    for (uint8_t note : {34, 46, 58, 70, 82}) {
      for (uint8_t velocity : {40, 100, 127}) {
        NativeSynthPatch patch = reed_base_patch();
        patch.reed.conical = conical;
        const std::vector<float> tone = render_patch(patch, note, velocity, 48000);
        REQUIRE(peak(tone) < 4.0f);
        REQUIRE(std::isfinite(tone.back()));
      }
    }
  }
}

TEST_CASE("reed is stable at extreme reed stiffness and breath", "[midi][synth][reed]") {
  for (float stiffness : {0.0f, 0.5f, 1.0f}) {
    for (float breath : {0.2f, 1.0f}) {
      NativeSynthPatch patch = reed_base_patch();
      patch.reed.reed_stiffness = stiffness;
      patch.reed.breath_pressure = breath;
      patch.reed.damping = 0.1f;  // least loop loss (hardest to hold bounded)
      const std::vector<float> tone = render_patch(patch, 58, 120, 48000);
      REQUIRE(peak(tone) < 4.0f);
      REQUIRE(std::isfinite(tone.back()));
    }
  }
}

TEST_CASE("reed tuning is accurate", "[midi][synth][reed]") {
  for (bool conical : {false, true}) {
    NativeSynthPatch patch = reed_base_patch();
    patch.reed.conical = conical;
    for (const auto& [note, expected] :
         {std::pair<uint8_t, double>{57, 220.0}, std::pair<uint8_t, double>{50, 146.8324},
          std::pair<uint8_t, double>{62, 293.6648}}) {
      const std::vector<float> tone = render_patch(patch, note, 110, 48000);
      const double estimated = fft_fundamental(tone, 16000, expected);
      REQUIRE(std::fabs(estimated / expected - 1.0) < 0.02);
    }
  }
}

TEST_CASE("cylindrical reed radiates odd harmonics", "[midi][synth][reed]") {
  // A clarinet's cylindrical bore is closed at the reed: a quarter-wave
  // resonator that radiates odd harmonics only (the even harmonics are strongly
  // suppressed), exactly like a stopped organ pipe.
  const double f0 = 146.8324;  // D3
  NativeSynthPatch patch = reed_base_patch();
  patch.reed.conical = false;
  const std::vector<float> tone = render_patch(patch, 50, 110, 24000);
  const std::vector<double> ps = power_spectrum(tone, 8000);
  const double h1 = harmonic_power(ps, f0, 1);
  REQUIRE(h1 > 0.0);
  // Odd harmonics carry real energy; each even harmonic sits well below the odd
  // ones bracketing it.
  REQUIRE(harmonic_power(ps, f0, 3) > 0.02 * h1);
  REQUIRE(harmonic_power(ps, f0, 2) < 0.3 * harmonic_power(ps, f0, 3));
  REQUIRE(harmonic_power(ps, f0, 4) < 0.3 * harmonic_power(ps, f0, 3));
}

TEST_CASE("conical reed radiates the full harmonic series", "[midi][synth][reed]") {
  // A saxophone/oboe conical bore behaves like an open pipe: the full harmonic
  // series, so the even harmonics (absent in the clarinet) carry real energy.
  const double f0 = 146.8324;  // D3
  NativeSynthPatch patch = reed_base_patch();
  patch.reed.conical = true;
  const std::vector<float> tone = render_patch(patch, 50, 110, 24000);
  const std::vector<double> ps = power_spectrum(tone, 8000);
  const double h1 = harmonic_power(ps, f0, 1);
  REQUIRE(h1 > 0.0);
  REQUIRE(harmonic_power(ps, f0, 2) > 0.02 * h1);
  REQUIRE(harmonic_power(ps, f0, 3) > 0.01 * h1);
}

TEST_CASE("reed brightness voices the bell", "[midi][synth][reed]") {
  // The brightness knob opens the bell reflection filter, so more upper partials
  // survive each round trip: a brighter bell reads brighter (higher centroid).
  NativeSynthPatch dark = reed_base_patch();
  dark.reed.brightness = 0.2f;
  NativeSynthPatch bright = reed_base_patch();
  bright.reed.brightness = 0.8f;
  const std::vector<float> dark_tone = render_patch(dark, 58, 110, 24000);
  const std::vector<float> bright_tone = render_patch(bright, 58, 110, 24000);
  REQUIRE(peak(bright_tone) > 0.005f);
  REQUIRE(swell_centroid(bright_tone, 8000) > 1.2 * swell_centroid(dark_tone, 8000));
}

TEST_CASE("reed stiffness reshapes the tone", "[midi][synth][reed]") {
  // The reed stiffness knob moves the reed table's operating point, measurably
  // reshaping the timbre while staying inside the stable oscillating band (the
  // bell filter, not the reed, is the clean brightness control).
  NativeSynthPatch soft = reed_base_patch();
  soft.reed.reed_stiffness = 0.05f;
  NativeSynthPatch hard = reed_base_patch();
  hard.reed.reed_stiffness = 0.95f;
  const std::vector<float> soft_tone = render_patch(soft, 58, 110, 24000);
  const std::vector<float> hard_tone = render_patch(hard, 58, 110, 24000);
  REQUIRE(peak(soft_tone) > 0.005f);
  REQUIRE(peak(hard_tone) > 0.005f);
  const double cs = swell_centroid(soft_tone, 8000);
  const double ch = swell_centroid(hard_tone, 8000);
  REQUIRE(std::fabs(ch / cs - 1.0) > 0.05);
}

TEST_CASE("reed speaks promptly and sustains", "[midi][synth][reed]") {
  NativeSynthPatch patch = reed_base_patch();
  const std::vector<float> tone = render_patch(patch, 58, 110, 48000);
  const float early = rms(tone, 4000, 10000);
  const float late = rms(tone, 40000, 46000);
  REQUIRE(early > 0.01f);
  REQUIRE(late > 0.5f * early);
  REQUIRE(late < 2.0f * early);
}

TEST_CASE("reed rings down after note-off", "[midi][synth][reed]") {
  NativeSynthPatch patch = reed_base_patch();
  const std::vector<float> tone = render_patch(patch, 58, 110, 48000, 24000);
  const float held = rms(tone, 16000, 22000);
  const float released = rms(tone, 40000, 46000);
  REQUIRE(held > 0.01f);
  REQUIRE(released < 0.2f * held);
}

TEST_CASE("reed brightness CC74 voices the tone live", "[midi][synth][reed]") {
  // CC74 opens the bell reflection filter at the note's onset: a high CC74 reads
  // brighter than a low one (the note is seeded at the controller position).
  NativeSynthPatch patch = reed_base_patch();
  const std::vector<float> dark = render_with_initial_cc(patch, 58, 110, 24000, 74, 10);
  const std::vector<float> bright = render_with_initial_cc(patch, 58, 110, 24000, 74, 120);
  REQUIRE(peak(bright) > 0.005f);
  REQUIRE(swell_centroid(bright, 8000) > 1.2 * swell_centroid(dark, 8000));
}

TEST_CASE("reed breath CC2 recolours the tone live", "[midi][synth][reed]") {
  // CC2 moves the mouth pressure within the reed's stable band — a deliberately
  // narrow, safe range (a real reed's usable pressure span is small), so the
  // effect is subtle but real: the tone measurably changes and never silences
  // (the operating point stays clear of the beating-shut regime).
  NativeSynthPatch patch = reed_base_patch();
  const std::vector<float> soft = render_with_initial_cc(patch, 58, 110, 24000, 2, 15);
  const std::vector<float> hard = render_with_initial_cc(patch, 58, 110, 24000, 2, 120);
  REQUIRE(peak(soft) > 0.005f);
  REQUIRE(peak(hard) > 0.005f);
  // The two renders differ measurably (the control is live), while both stay in
  // the oscillating band.
  double diff = 0.0;
  double ref = 0.0;
  for (size_t i = 8000; i < 20000; ++i) {
    const double d = static_cast<double>(soft[i]) - hard[i];
    diff += d * d;
    ref += static_cast<double>(hard[i]) * hard[i];
  }
  REQUIRE(std::sqrt(diff / ref) > 0.05);
}

TEST_CASE("expression (CC11) drives the reed loudness swell", "[midi][synth][reed]") {
  // Pulling the expression pedal down mid-note reduces the shared loudness VCA:
  // the sustained tone gets markedly quieter (a diminuendo).
  NativeSynthPatch patch = reed_base_patch();
  const std::vector<float> tone = render_cc_change(patch, 58, 110, 16000, 32000, 11, 20);
  const float before = rms(tone, 8000, 15000);
  const float after = rms(tone, 40000, 47000);
  REQUIRE(before > 0.005f);
  REQUIRE(after < 0.5f * before);
}

TEST_CASE("rapid reed controller sweeps stay bounded", "[midi][synth][reed]") {
  // A host slamming CC2 / CC74 back and forth must not click or blow up — the
  // per-sample smoothing absorbs the jumps and the reed stays in its band.
  NativeSynthConfig cfg;
  cfg.patch = reed_base_patch();
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 58, 110)));
  std::vector<float> block(512, 0.0f);
  std::vector<float> block_r(512, 0.0f);
  float* chans[2] = {block.data(), block_r.data()};
  float worst = 0.0f;
  for (int i = 0; i < 40; ++i) {
    const uint8_t v = (i % 2 == 0) ? 0 : 127;
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 2, v)));
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 74, 127 - v)));
    synth.process(chans, 2, 512);
    worst = std::max(worst, peak(block));
    REQUIRE(std::isfinite(block.back()));
  }
  REQUIRE(worst < 4.0f);
}

TEST_CASE("the reed-family presets are voiced reed woodwinds", "[midi][synth][reed]") {
  using sonare::midi::synth::BodyType;
  using sonare::midi::synth::find_synth_preset;

  // Each member (GM 65-72) is the reed engine coloured by the shared wood-tube
  // bore, speaking a sustained note in its register. The clarinet is the only
  // cylinder (odd harmonics); the rest are conical (full harmonic series).
  struct Member {
    const char* name;
    uint8_t note;
    bool conical;
  };
  for (const Member member : {Member{"clarinet", 50, false}, Member{"soprano-sax", 70, true},
                              Member{"alto-sax", 62, true}, Member{"tenor-sax", 54, true},
                              Member{"baritone-sax", 46, true}, Member{"oboe", 70, true},
                              Member{"english-horn", 58, true}, Member{"bassoon", 42, true}}) {
    INFO(member.name);
    const auto* preset = find_synth_preset(member.name);
    REQUIRE(preset != nullptr);
    const NativeSynthPatch& patch = preset->config.patch;
    REQUIRE(patch.mode == SynthEngineMode::kReed);
    REQUIRE(patch.body == BodyType::kWoodTube);
    REQUIRE(patch.reed.conical == member.conical);

    const std::vector<float> tone = render_patch(patch, member.note, 100, 24000);
    for (float s : tone) REQUIRE(std::isfinite(s));
    REQUIRE(rms(tone, 4000, 8000) > 1e-3f);
    REQUIRE(rms(tone, 18000, 22000) > 0.5f * rms(tone, 4000, 8000));
    REQUIRE(peak(tone) < 4.0f);
  }

  // The catalog spans bright (oboe) to dark (bassoon): the spectral centroid
  // falls from the nasal double reed to the deep one at a common pitch.
  const double oboe =
      swell_centroid(render_patch(find_synth_preset("oboe")->config.patch, 58, 100, 24000), 8000);
  const double bassoon = swell_centroid(
      render_patch(find_synth_preset("bassoon")->config.patch, 58, 100, 24000), 8000);
  REQUIRE(oboe > bassoon);
}

// --- Off-by-default advanced physics -----------------------------------------

TEST_CASE("dynamic reed adds a resonance and stays stable", "[midi][synth][reed]") {
  // The mass-spring reed rings at its natural frequency, biasing the table and
  // boosting the partials near the reed formant — the tone measurably changes
  // and the loop stays bounded across the keyboard.
  for (uint8_t note : {34, 50, 58, 70}) {
    NativeSynthPatch plain = reed_base_patch();
    NativeSynthPatch dyn = reed_base_patch();
    dyn.reed.dynamic_reed = true;
    dyn.reed.reed_resonance = 0.5f;
    const std::vector<float> a = render_patch(plain, note, 110, 24000);
    const std::vector<float> b = render_patch(dyn, note, 110, 24000);
    REQUIRE(peak(b) > 0.005f);
    REQUIRE(peak(b) < 4.0f);
    REQUIRE(std::isfinite(b.back()));
    REQUIRE(a != b);  // the reed resonance really changed the tone
  }
  // At a common pitch the reed formant lifts the spectral centroid.
  NativeSynthPatch plain = reed_base_patch();
  NativeSynthPatch dyn = reed_base_patch();
  dyn.reed.dynamic_reed = true;
  const double c_off = swell_centroid(render_patch(plain, 58, 110, 24000), 8000);
  const double c_on = swell_centroid(render_patch(dyn, 58, 110, 24000), 8000);
  REQUIRE(c_on > 1.2 * c_off);
}

TEST_CASE("register vent lifts the tone toward the upper register", "[midi][synth][reed]") {
  // Opening the register vent damps the fundamental so the upper modes dominate:
  // the tone brightens markedly and stays audible (a register note is thinner
  // and quieter, never silent) and bounded.
  NativeSynthPatch plain = reed_base_patch();
  NativeSynthPatch vent = reed_base_patch();
  vent.reed.register_vent = 1.0f;
  const std::vector<float> off = render_patch(plain, 58, 110, 24000);
  const std::vector<float> on = render_patch(vent, 58, 110, 24000);
  REQUIRE(peak(on) > 0.01f);
  REQUIRE(peak(on) < 4.0f);
  REQUIRE(std::isfinite(on.back()));
  REQUIRE(swell_centroid(on, 8000) > 1.5 * swell_centroid(off, 8000));
}

TEST_CASE("growl sidebands the tone and stays stable", "[midi][synth][reed]") {
  // The growl LFO modulates the breath, adding a rough vocal edge: the tone
  // measurably changes and stays bounded.
  NativeSynthPatch plain = reed_base_patch();
  NativeSynthPatch gr = reed_base_patch();
  gr.reed.growl = 0.6f;
  const std::vector<float> off = render_patch(plain, 58, 110, 24000);
  const std::vector<float> on = render_patch(gr, 58, 110, 24000);
  REQUIRE(peak(on) > 0.005f);
  REQUIRE(peak(on) < 4.0f);
  REQUIRE(std::isfinite(on.back()));
  double diff = 0.0;
  double ref = 0.0;
  for (size_t i = 8000; i < 20000; ++i) {
    const double d = static_cast<double>(off[i]) - on[i];
    diff += d * d;
    ref += static_cast<double>(off[i]) * off[i];
  }
  REQUIRE(std::sqrt(diff / ref) > 0.05);
}

TEST_CASE("growth cone blooms the conical fundamental", "[midi][synth][reed]") {
  // The Phase-1 cone is a bare positive-feedback comb (a cylinder pretending to
  // be a cone), so its fundamental is weak. The growth cone recovers the
  // truncated cone's apex 1/r pressure, blooming the fundamental relative to the
  // upper partials — the way a saxophone's strong low end reads.
  const double f0 = 146.8324;  // D3
  NativeSynthPatch plain = reed_base_patch();
  plain.reed.conical = true;
  NativeSynthPatch grown = reed_base_patch();
  grown.reed.conical = true;
  grown.reed.cone_growth = 1.0f;
  const std::vector<float> off = render_patch(plain, 50, 110, 24000);
  const std::vector<float> on = render_patch(grown, 50, 110, 24000);
  REQUIRE(peak(on) < 4.0f);
  REQUIRE(std::isfinite(on.back()));
  REQUIRE(off != on);  // the growth term really changed the tone

  const std::vector<double> ps_off = power_spectrum(off, 8000);
  const std::vector<double> ps_on = power_spectrum(on, 8000);
  const double bloom_off = harmonic_power(ps_off, f0, 1) /
                           (harmonic_power(ps_off, f0, 2) + harmonic_power(ps_off, f0, 3) + 1e-12);
  const double bloom_on = harmonic_power(ps_on, f0, 1) /
                          (harmonic_power(ps_on, f0, 2) + harmonic_power(ps_on, f0, 3) + 1e-12);
  REQUIRE(bloom_on > 1.1 * bloom_off);
}

TEST_CASE("growth cone is ignored for a cylinder", "[midi][synth][reed]") {
  // The apex term is a conical geometry; a clarinet's cylindrical bore has no
  // truncated apex, so cone_growth is a no-op there (bit-identical render).
  NativeSynthPatch plain = reed_base_patch();
  plain.reed.conical = false;
  NativeSynthPatch grown = reed_base_patch();
  grown.reed.conical = false;
  grown.reed.cone_growth = 1.0f;
  REQUIRE(render_patch(plain, 58, 110, 8192) == render_patch(grown, 58, 110, 8192));
}

TEST_CASE("growth cone stays bounded and in tune", "[midi][synth][reed]") {
  // Because the apex integrator sits at radiation (not in the resonant loop), a
  // fully-grown cone cannot detune or destabilise the bore: the tuning is
  // unchanged and the loop stays bounded across the keyboard.
  for (const auto& [note, expected] :
       {std::pair<uint8_t, double>{42, 92.4986}, std::pair<uint8_t, double>{54, 184.9972},
        std::pair<uint8_t, double>{66, 369.9944}}) {
    NativeSynthPatch patch = reed_base_patch();
    patch.reed.conical = true;
    patch.reed.cone_growth = 1.0f;
    const std::vector<float> tone = render_patch(patch, note, 110, 48000);
    REQUIRE(peak(tone) < 4.0f);
    REQUIRE(std::isfinite(tone.back()));
    const double estimated = fft_fundamental(tone, 16000, expected);
    REQUIRE(std::fabs(estimated / expected - 1.0) < 0.02);
  }
}

TEST_CASE("tonehole scattering jumps the cylinder to its twelfth", "[midi][synth][reed]") {
  // Opening a real tonehole is an in-bore scattering junction (unlike the
  // output-side register_vent): it imposes a pressure node at the hole, so a
  // clarinet's cylindrical bore stops sounding its fundamental and overblows to
  // the twelfth (the 3rd harmonic), the register jump a register key produces.
  const double f0 = 233.0819;  // A#3 (note 58)
  NativeSynthPatch closed = reed_base_patch();
  closed.reed.conical = false;
  NativeSynthPatch open = reed_base_patch();
  open.reed.conical = false;
  open.reed.tonehole = 1.0f;
  const std::vector<float> tc = render_patch(closed, 58, 110, 24000);
  const std::vector<float> to = render_patch(open, 58, 110, 24000);
  REQUIRE(peak(to) < 4.0f);
  REQUIRE(std::isfinite(to.back()));
  const std::vector<double> psc = power_spectrum(tc, 8000);
  const std::vector<double> pso = power_spectrum(to, 8000);
  // Closed: the fundamental outweighs the twelfth. Open: the twelfth takes over.
  REQUIRE(harmonic_power(psc, f0, 1) > harmonic_power(psc, f0, 3));
  REQUIRE(harmonic_power(pso, f0, 3) > harmonic_power(pso, f0, 1));
}

TEST_CASE("tonehole scattering lifts the cone register", "[midi][synth][reed]") {
  // A cone overblows to the octave, not the twelfth, so its tonehole sits a
  // quarter of the way down the bore (half-way is degenerate for a cone). The
  // open hole lifts the upper register — the tone brightens measurably — and
  // stays bounded (the sub-loop cannot run away).
  NativeSynthPatch closed = reed_base_patch();
  closed.reed.conical = true;
  NativeSynthPatch open = reed_base_patch();
  open.reed.conical = true;
  open.reed.tonehole = 1.0f;
  const std::vector<float> tc = render_patch(closed, 58, 110, 24000);
  const std::vector<float> to = render_patch(open, 58, 110, 24000);
  REQUIRE(peak(to) < 4.0f);
  REQUIRE(std::isfinite(to.back()));
  REQUIRE(tc != to);
  REQUIRE(swell_centroid(to, 8000) > 1.2 * swell_centroid(tc, 8000));
}

TEST_CASE("tonehole scattering stays bounded across the keyboard", "[midi][synth][reed]") {
  // The reed<->hole sub-loop must stay bounded alongside the main reed<->bell
  // loop for both topologies, the whole keyboard and the least-damped bore.
  for (bool conical : {false, true}) {
    for (uint8_t note : {34, 46, 58, 70, 82}) {
      NativeSynthPatch patch = reed_base_patch();
      patch.reed.conical = conical;
      patch.reed.damping = 0.1f;
      patch.reed.tonehole = 1.0f;
      const std::vector<float> tone = render_patch(patch, note, 120, 48000);
      REQUIRE(peak(tone) < 4.0f);
      REQUIRE(std::isfinite(tone.back()));
    }
  }
}

TEST_CASE("advanced reed gates compose stably", "[midi][synth][reed]") {
  // All five gates on together, across both topologies and the keyboard, must
  // stay bounded and finite.
  for (bool conical : {false, true}) {
    for (uint8_t note : {34, 50, 58, 70}) {
      NativeSynthPatch patch = reed_base_patch();
      patch.reed.conical = conical;
      patch.reed.dynamic_reed = true;
      patch.reed.reed_resonance = 0.6f;
      patch.reed.register_vent = 0.6f;
      patch.reed.growl = 0.5f;
      patch.reed.cone_growth = 0.8f;
      patch.reed.tonehole = 0.7f;
      const std::vector<float> tone = render_patch(patch, note, 110, 24000);
      REQUIRE(peak(tone) < 4.0f);
      REQUIRE(std::isfinite(tone.back()));
    }
  }
}

TEST_CASE("reed render is allocation-free", "[midi][synth][reed]") {
  NativeSynthConfig cfg;
  cfg.patch = reed_base_patch();
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 58, 110)));
  std::vector<float> left(256, 0.0f);
  std::vector<float> right(256, 0.0f);
  float* chans[2] = {left.data(), right.data()};
  sonare::test::AllocationGuard guard;
  synth.process(chans, 2, 256);
  REQUIRE(guard.count() == 0);
}
