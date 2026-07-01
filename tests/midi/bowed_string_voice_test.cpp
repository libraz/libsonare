/// @file bowed_string_voice_test.cpp
/// @brief Sustained bowed-string waveguide (midi/synth/bowed_string_voice):
///        fundamental tuning, the rich Helmholtz (full-harmonic sawtooth)
///        spectrum, prompt speech + steady sustain, note-off ring-down,
///        unconditional stability across the keyboard and dynamics, and
///        deterministic rendering.

#include "midi/synth/bowed_string_voice.h"

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

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::SynthEngineMode;

constexpr double kRate = 48000.0;

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

constexpr int kFft = 8192;
std::vector<double> power_spectrum(const std::vector<float>& buf, size_t from) {
  std::vector<float> windowed(kFft);
  for (int i = 0; i < kFft; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * i / (kFft - 1));
    windowed[static_cast<size_t>(i)] = buf[from + static_cast<size_t>(i)] * static_cast<float>(w);
  }
  sonare::FFT fft(kFft);
  std::vector<std::complex<float>> spectrum(static_cast<size_t>(fft.n_bins()));
  fft.forward(windowed.data(), spectrum.data());
  std::vector<double> power(spectrum.size());
  for (size_t i = 0; i < spectrum.size(); ++i) power[i] = std::norm(spectrum[i]);
  return power;
}

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

/// A filter-bypassed bowed-string test patch (raw string, no body resonance so
/// the pitch measurement reads the string, not the corpus modes).
NativeSynthPatch bowed_base_patch() {
  NativeSynthPatch p;
  p.mode = SynthEngineMode::kBowedString;
  p.cutoff_hz = 20000.0f;
  p.amp_env.attack_ms = 5.0f;
  p.amp_env.sustain = 1.0f;
  p.amp_env.release_ms = 100.0f;
  p.bowed_string.bow_position = 0.13f;
  p.bowed_string.bow_force = 0.5f;
  p.bowed_string.bow_speed = 0.6f;
  p.bowed_string.brightness = 0.5f;
  p.bowed_string.damping = 0.3f;
  return p;
}

}  // namespace

TEST_CASE("bowed string rendering is deterministic", "[midi][synth][bowed]") {
  const NativeSynthPatch patch = bowed_base_patch();
  const std::vector<float> first = render_patch(patch, 57, 100, 8192);
  const std::vector<float> second = render_patch(patch, 57, 100, 8192);
  REQUIRE(peak(first) > 0.01f);
  REQUIRE(first == second);
}

TEST_CASE("bowed string is unconditionally stable", "[midi][synth][bowed]") {
  // Across the keyboard and dynamics, the bow loop must stay bounded.
  for (uint8_t note : {28, 45, 57, 69, 88}) {
    for (uint8_t velocity : {40, 100, 127}) {
      NativeSynthPatch patch = bowed_base_patch();
      const std::vector<float> tone = render_patch(patch, note, velocity, 48000);
      REQUIRE(peak(tone) > 0.005f);
      REQUIRE(peak(tone) < 4.0f);
      REQUIRE(std::isfinite(tone.back()));
    }
  }
}

TEST_CASE("bowed string is stable at extreme bow force", "[midi][synth][bowed]") {
  // A firm, heavy bow drives the string hardest; the bow table's saturation
  // must still bound the loop (no runaway).
  for (float force : {0.0f, 0.5f, 1.0f}) {
    NativeSynthPatch patch = bowed_base_patch();
    patch.bowed_string.bow_force = force;
    patch.bowed_string.damping = 0.1f;  // least loop loss (hardest to hold bounded)
    const std::vector<float> tone = render_patch(patch, 45, 120, 48000);
    REQUIRE(peak(tone) < 4.0f);
    REQUIRE(std::isfinite(tone.back()));
  }
}

TEST_CASE("bowed string tuning is accurate", "[midi][synth][bowed]") {
  NativeSynthPatch patch = bowed_base_patch();
  for (const auto& [note, expected] :
       {std::pair<uint8_t, double>{69, 440.0}, std::pair<uint8_t, double>{45, 110.0},
        std::pair<uint8_t, double>{60, 261.6256}, std::pair<uint8_t, double>{52, 164.8138}}) {
    const std::vector<float> tone = render_patch(patch, note, 110, 48000);
    const double estimated = fft_fundamental(tone, 16000, expected);
    REQUIRE(std::fabs(estimated / expected - 1.0) < 0.02);
  }
}

TEST_CASE("bowed string produces a rich Helmholtz spectrum", "[midi][synth][bowed]") {
  // A bowed string is a full-harmonic sawtooth (Helmholtz motion), not a sine:
  // the low harmonics all carry real energy, and the even harmonics (absent in a
  // stopped pipe) are present — the stick-slip nonlinearity generating them.
  const double f0 = 220.0;  // A3
  NativeSynthPatch patch = bowed_base_patch();
  const std::vector<float> tone = render_patch(patch, 57, 110, 24000);
  const std::vector<double> ps = power_spectrum(tone, 8000);
  const double h1 = harmonic_power(ps, f0, 1);
  REQUIRE(h1 > 0.0);
  // Several harmonics each carry a meaningful share of the fundamental (a pure
  // sine would leave these near zero).
  REQUIRE(harmonic_power(ps, f0, 2) > 0.02 * h1);
  REQUIRE(harmonic_power(ps, f0, 3) > 0.01 * h1);
  REQUIRE(harmonic_power(ps, f0, 4) > 0.005 * h1);
}

TEST_CASE("bow force brightens the tone", "[midi][synth][bowed]") {
  // Harder bow force widens the sticking region and roughens the slip, adding
  // upper partials: a firmer bow reads brighter (higher spectral centroid).
  NativeSynthPatch light = bowed_base_patch();
  light.bowed_string.bow_force = 0.15f;
  NativeSynthPatch firm = bowed_base_patch();
  firm.bowed_string.bow_force = 0.9f;
  const std::vector<float> light_tone = render_patch(light, 57, 110, 24000);
  const std::vector<float> firm_tone = render_patch(firm, 57, 110, 24000);
  REQUIRE(peak(firm_tone) > 0.005f);
  REQUIRE(swell_centroid(firm_tone, 8000) > 1.1 * swell_centroid(light_tone, 8000));
}

TEST_CASE("bowed string speaks promptly and sustains", "[midi][synth][bowed]") {
  NativeSynthPatch patch = bowed_base_patch();
  const std::vector<float> tone = render_patch(patch, 57, 110, 48000);
  const float early = rms(tone, 6000, 12000);  // ~0.13-0.25 s (bow up to speed)
  const float late = rms(tone, 40000, 46000);  // ~0.83-0.96 s (held)
  REQUIRE(early > 0.005f);                     // the string locks into Helmholtz motion
  REQUIRE(late > 0.5f * early);                // sustained, not a decaying pluck
  REQUIRE(late < 2.0f * early);                // and not a runaway swell
}

TEST_CASE("bowed string note-off rings the string down", "[midi][synth][bowed]") {
  NativeSynthPatch patch = bowed_base_patch();
  patch.bowed_string.release_ms = 60.0f;
  patch.amp_env.release_ms = 120.0f;
  const std::vector<float> held = render_patch(patch, 57, 110, 38400);
  const std::vector<float> released = render_patch(patch, 57, 110, 38400, /*note_off_at=*/12000);
  const float held_late = rms(held, 28800, 38400);
  const float released_late = rms(released, 28800, 38400);
  REQUIRE(held_late > 0.0f);
  REQUIRE(released_late < 0.1f * held_late);
}

TEST_CASE("bow position shapes the spectrum", "[midi][synth][bowed]") {
  // Bowing near the bridge (sul ponticello) is brighter/edgier than bowing over
  // the fingerboard (sul tasto): the delay-line split notches different
  // harmonics, so the centroid differs.
  NativeSynthPatch ponticello = bowed_base_patch();
  ponticello.bowed_string.bow_position = 0.04f;  // near the bridge
  NativeSynthPatch tasto = bowed_base_patch();
  tasto.bowed_string.bow_position = 0.28f;  // over the fingerboard
  const std::vector<float> pont_tone = render_patch(ponticello, 57, 110, 24000);
  const std::vector<float> tasto_tone = render_patch(tasto, 57, 110, 24000);
  REQUIRE(peak(pont_tone) > 0.005f);
  REQUIRE(peak(tasto_tone) > 0.005f);
  REQUIRE(swell_centroid(pont_tone, 8000) > swell_centroid(tasto_tone, 8000));
}

// --- Phase 2: live continuous control (CC11 speed / CC2 force / CC74 position)
//     and vibrato ---

/// Plays a note, renders @p pre samples, sends @p cc = @p value, then renders
/// @p post more (a controller moved WHILE the note sounds).
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

TEST_CASE("expression (CC11) drives the bow-speed swell", "[midi][synth][bowed]") {
  // Pulling the expression pedal down mid-note reduces the bow speed (and level):
  // the sustained tone gets markedly quieter — the string crescendo/diminuendo.
  NativeSynthPatch patch = bowed_base_patch();
  const std::vector<float> tone = render_cc_change(patch, 57, 110, 16000, 32000, 11, 20);
  const float before = rms(tone, 8000, 15000);
  const float after = rms(tone, 40000, 47000);
  REQUIRE(before > 0.005f);
  REQUIRE(after < 0.5f * before);  // the diminuendo really took hold
}

TEST_CASE("breath (CC2) sets the bow force / brightness", "[midi][synth][bowed]") {
  // A firm bow (high CC2) is brighter than a light one; the note is seeded at
  // the controller position sent before it starts.
  NativeSynthPatch patch = bowed_base_patch();
  const std::vector<float> light = render_with_initial_cc(patch, 57, 110, 24000, 2, 10);
  const std::vector<float> firm = render_with_initial_cc(patch, 57, 110, 24000, 2, 115);
  REQUIRE(peak(firm) > 0.005f);
  REQUIRE(swell_centroid(firm, 8000) > 1.1 * swell_centroid(light, 8000));
}

TEST_CASE("bow position (CC74) shapes the tone", "[midi][synth][bowed]") {
  // CC74 sets the bow contact point at the note's onset: near the bridge (low
  // CC74) is the bright, edgy sul ponticello; over the fingerboard (high CC74)
  // is the soft sul tasto. Bow position is primarily an ONSET parameter in the
  // single-scattering-junction model — the standing wave forms around the bow
  // point, so the note is seeded at the controller position before it speaks
  // (a mid-note move is intentionally subtle; a richer friction model would
  // strengthen the continuous response — a Phase 4 refinement).
  NativeSynthPatch patch = bowed_base_patch();
  const std::vector<float> pont = render_with_initial_cc(patch, 57, 110, 24000, 74, 6);  // bright
  const std::vector<float> tasto = render_with_initial_cc(patch, 57, 110, 24000, 74, 120);  // dark
  REQUIRE(peak(pont) > 0.005f);
  REQUIRE(peak(tasto) > 0.005f);
  REQUIRE(swell_centroid(pont, 8000) > 1.1 * swell_centroid(tasto, 8000));
}

TEST_CASE("rapid controller sweeps stay bounded (smoothed)", "[midi][synth][bowed]") {
  // A host slamming the bow controllers back and forth must not click or blow up
  // — the per-sample smoothing absorbs the jumps.
  NativeSynthConfig cfg;
  cfg.patch = bowed_base_patch();
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 45, 110)));
  std::vector<float> left(1024, 0.0f);
  std::vector<float> right(1024, 0.0f);
  float* chans[2] = {left.data(), right.data()};
  float p = 0.0f;
  for (int block = 0; block < 40; ++block) {
    const uint8_t pos = (block & 1) ? 2 : 125;
    const uint8_t force = (block & 1) ? 120 : 8;
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 74, pos)));
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 2, force)));
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    synth.process(chans, 2, 1024);
    p = std::max(p, peak(left));
    REQUIRE(std::isfinite(left[1023]));
  }
  REQUIRE(p > 0.005f);
  REQUIRE(p < 4.0f);
}

TEST_CASE("vibrato modulates the bowed pitch", "[midi][synth][bowed]") {
  // Left-hand vibrato (host LFO -> pitch) spreads the fundamental into
  // sidebands, so the energy just off the peak bin rises relative to the centre
  // versus a dead-steady note.
  const double f0 = 220.0;  // A3
  const auto sideband_ratio = [&](const std::vector<float>& buf) {
    const std::vector<double> ps = power_spectrum(buf, 12000);
    const int c = static_cast<int>(std::lround(f0 / kRate * kFft));
    const double sides = ps[static_cast<size_t>(c - 2)] + ps[static_cast<size_t>(c - 1)] +
                         ps[static_cast<size_t>(c + 1)] + ps[static_cast<size_t>(c + 2)];
    return ps[static_cast<size_t>(c)] > 0.0 ? sides / ps[static_cast<size_t>(c)] : 0.0;
  };
  NativeSynthPatch steady = bowed_base_patch();
  NativeSynthPatch vibrato = bowed_base_patch();
  vibrato.lfo_rate_hz = 6.0f;
  vibrato.lfo_to_pitch_cents = 60.0f;  // a wide, audible vibrato
  const std::vector<float> steady_tone = render_patch(steady, 57, 110, 32000);
  const std::vector<float> vibrato_tone = render_patch(vibrato, 57, 110, 32000);
  REQUIRE(peak(vibrato_tone) > 0.005f);
  REQUIRE(sideband_ratio(vibrato_tone) > 1.2 * sideband_ratio(steady_tone));
}

TEST_CASE("bowed string audio path is allocation-free", "[midi][synth][bowed]") {
  NativeSynthConfig cfg;
  cfg.patch = bowed_base_patch();
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);

  std::vector<float> left(256, 0.0f);
  std::vector<float> right(256, 0.0f);
  float* chans[2] = {left.data(), right.data()};
  {
    sonare::test::AllocationGuard guard;
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 45, 100)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 57, 100)));
    synth.process(chans, 2, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 45, 0)));
    synth.process(chans, 2, 256);
    REQUIRE(guard.count() == 0);
  }
}

TEST_CASE("the violin-family presets are voiced bowed strings", "[midi][synth][bowed]") {
  using sonare::midi::synth::BodyType;
  using sonare::midi::synth::find_synth_preset;

  // Each member (GM 40-43) is the bowed-string engine coloured by the shared
  // violin corpus, and speaks a sustained note in its natural register.
  struct Member {
    const char* name;
    uint8_t note;
  };
  for (const Member member :
       {Member{"violin", 76}, Member{"viola", 64}, Member{"cello", 45}, Member{"contrabass", 33}}) {
    INFO(member.name);
    const auto* preset = find_synth_preset(member.name);
    REQUIRE(preset != nullptr);
    const NativeSynthPatch& patch = preset->config.patch;
    REQUIRE(patch.mode == SynthEngineMode::kBowedString);
    REQUIRE(patch.body == BodyType::kViolin);
    REQUIRE(patch.body_mix > 0.0f);

    const std::vector<float> tone = render_patch(patch, member.note, 100, 24000);
    for (float s : tone) REQUIRE(std::isfinite(s));
    // Speaks promptly and holds under the sustaining bow.
    REQUIRE(rms(tone, 4000, 8000) > 1e-3f);
    REQUIRE(rms(tone, 18000, 22000) > 0.5f * rms(tone, 4000, 8000));
    REQUIRE(peak(tone) < 4.0f);  // the bow table keeps the loop bounded
  }

  // The catalog darkens from violin to contrabass (larger body, less bright
  // bow): the spectral centroid falls across the family at a common pitch.
  const double violin = swell_centroid(
      render_patch(find_synth_preset("violin")->config.patch, 57, 100, 24000), 16000);
  const double cello =
      swell_centroid(render_patch(find_synth_preset("cello")->config.patch, 57, 100, 24000), 16000);
  REQUIRE(violin > cello);
}
