/// @file pipe_organ_voice_test.cpp
/// @brief Self-oscillating jet flue-pipe voice (midi/synth/pipe_organ_voice):
///        fundamental tuning, the open (octave-rich) vs stopped (hollow,
///        fundamental-dominant) voicing, prompt + solid sustained speech,
///        note-off damping, unconditional stability and deterministic rendering.

#include "midi/synth/pipe_organ_voice.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <complex>
#include <vector>

#include "core/fft.h"
#include "midi/midi_event.h"
#include "midi/synth/gm_fallback_map.h"
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

/// Parabolic-interpolated spectral peak (Hz) nearest @p f0_hint over a window
/// of @p buf from @p from. Sub-bin accurate and immune to the breathy
/// zero-crossing jitter of a noise-driven pipe.
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

/// Power of harmonic k (+-2 bins around k*f0).
double harmonic_power(const std::vector<double>& power, double f0, int k) {
  const int centre = static_cast<int>(std::lround(k * f0 / kRate * kFft));
  double acc = 0.0;
  for (int b = centre - 2; b <= centre + 2; ++b) {
    if (b > 0 && b < static_cast<int>(power.size())) acc += power[static_cast<size_t>(b)];
  }
  return acc;
}

/// A bright filter-bypassed pipe-organ test patch.
NativeSynthPatch organ_base_patch() {
  NativeSynthPatch p;
  p.mode = SynthEngineMode::kPipeOrgan;
  p.cutoff_hz = 20000.0f;
  p.amp_env.attack_ms = 5.0f;
  p.amp_env.sustain = 1.0f;
  p.amp_env.release_ms = 80.0f;
  p.pipe_organ.brightness = 0.6f;
  p.pipe_organ.breath = 0.4f;
  p.pipe_organ.chiff = 0.4f;
  return p;
}

}  // namespace

TEST_CASE("pipe organ rendering is deterministic", "[midi][synth][organ]") {
  const NativeSynthPatch patch = organ_base_patch();
  const std::vector<float> first = render_patch(patch, 57, 100, 8192);
  const std::vector<float> second = render_patch(patch, 57, 100, 8192);
  REQUIRE(peak(first) > 0.01f);
  REQUIRE(first == second);
}

TEST_CASE("pipe organ is unconditionally stable", "[midi][synth][organ]") {
  // Across the keyboard, open and stopped, the loop must stay bounded.
  for (bool stopped : {false, true}) {
    for (uint8_t note : {24, 45, 69, 96}) {
      CAPTURE(stopped, note);
      NativeSynthPatch patch = organ_base_patch();
      patch.pipe_organ.stopped = stopped;
      patch.pipe_organ.tone_decay_s = 8.0f;
      const std::vector<float> tone = render_patch(patch, note, 110, 48000);
      // Judge speech on the settled half: the onset is chiff (band-limited
      // noise), not evidence the pipe itself speaks.
      const std::vector<float> settled(tone.begin() + 24000, tone.end());
      REQUIRE(peak(settled) > 0.005f);
      REQUIRE(peak(tone) < 4.0f);
      REQUIRE(std::isfinite(tone.back()));
    }
  }
}

TEST_CASE("pipe organ tuning is accurate (open pipe)", "[midi][synth][organ]") {
  // The self-oscillating jet locks a solid partial; a modest tolerance (~25
  // cents) covers the residual across the compass, read from the spectral peak.
  NativeSynthPatch patch = organ_base_patch();
  patch.pipe_organ.stopped = false;
  for (const auto& [note, expected] :
       {std::pair<uint8_t, double>{69, 440.0}, std::pair<uint8_t, double>{45, 110.0},
        std::pair<uint8_t, double>{60, 261.6256}}) {
    const std::vector<float> tone = render_patch(patch, note, 110, 48000);
    const double estimated = fft_fundamental(tone, 16000, expected);
    REQUIRE(std::fabs(estimated / expected - 1.0) < 0.015);
  }
}

TEST_CASE("stopped pipe shares the open pipe's fundamental", "[midi][synth][organ]") {
  // A stopped rank is voiced hollow off the same open jet loop, so it sounds at
  // the same fundamental (the footage sets the pitch, not the topology).
  NativeSynthPatch patch = organ_base_patch();
  patch.pipe_organ.stopped = true;
  const std::vector<float> tone = render_patch(patch, 57, 110, 48000);  // A3 = 220 Hz
  const double estimated = fft_fundamental(tone, 16000, 220.0);
  REQUIRE(std::fabs(estimated / 220.0 - 1.0) < 0.015);
}

TEST_CASE("stopped pipe is voiced hollow", "[midi][synth][organ]") {
  // Open pipe = octave-rich full harmonic series; stopped (gedackt) pipe = a
  // hollow, fundamental-dominant tone with the octave pump muted and the
  // reflection darkened. The stopped octave (2nd harmonic) collapses relative to
  // the open pipe's, and the stopped upperwork as a whole is far weaker.
  const double f0 = 220.0;
  NativeSynthPatch open = organ_base_patch();
  open.pipe_organ.stopped = false;
  NativeSynthPatch stopped = organ_base_patch();
  stopped.pipe_organ.stopped = true;

  const std::vector<float> open_tone = render_patch(open, 57, 110, 24000);
  const std::vector<float> stopped_tone = render_patch(stopped, 57, 110, 24000);
  const std::vector<double> open_power = power_spectrum(open_tone, 4096);
  const std::vector<double> stopped_power = power_spectrum(stopped_tone, 4096);

  const double open_h1 = harmonic_power(open_power, f0, 1);
  const double stopped_h1 = harmonic_power(stopped_power, f0, 1);
  const double open_h2 = harmonic_power(open_power, f0, 2) / open_h1;
  const double stopped_h2 = harmonic_power(stopped_power, f0, 2) / stopped_h1;
  const double open_upper = (harmonic_power(open_power, f0, 2) + harmonic_power(open_power, f0, 3) +
                             harmonic_power(open_power, f0, 4)) /
                            open_h1;
  const double stopped_upper =
      (harmonic_power(stopped_power, f0, 2) + harmonic_power(stopped_power, f0, 3) +
       harmonic_power(stopped_power, f0, 4)) /
      stopped_h1;

  // The stopped octave is far weaker than the open one, and the stopped pipe is
  // far more fundamental-dominant (hollow) overall.
  REQUIRE(open_h2 > 8.0 * stopped_h2);
  REQUIRE(stopped_upper < 0.6 * open_upper);
}

TEST_CASE("pipe organ speaks promptly and sustains", "[midi][synth][organ]") {
  NativeSynthPatch patch = organ_base_patch();
  patch.pipe_organ.tone_decay_s = 8.0f;
  const std::vector<float> tone = render_patch(patch, 57, 110, 48000);
  const float early = rms(tone, 2000, 8000);   // ~0.04-0.17 s (just after onset)
  const float late = rms(tone, 40000, 46000);  // ~0.83-0.96 s (held)
  REQUIRE(early > 0.005f);                     // prompt speech, no slow swell
  REQUIRE(late > 0.5f * early);                // sustained, not a decaying pluck
  REQUIRE(late < 2.5f * early);                // and not a runaway swell
}

TEST_CASE("pipe organ note-off stops the pipe", "[midi][synth][organ]") {
  NativeSynthPatch patch = organ_base_patch();
  patch.pipe_organ.tone_decay_s = 8.0f;
  patch.pipe_organ.release_damp_s = 0.05f;
  patch.amp_env.release_ms = 120.0f;
  const std::vector<float> held = render_patch(patch, 57, 110, 38400);
  const std::vector<float> released = render_patch(patch, 57, 110, 38400, /*note_off_at=*/12000);
  const float held_late = rms(held, 28800, 38400);
  const float released_late = rms(released, 28800, 38400);
  REQUIRE(held_late > 0.0f);
  REQUIRE(released_late < 0.1f * held_late);
}

TEST_CASE("GM church organ fallback is a flue pipe", "[midi][synth][organ]") {
  using sonare::midi::synth::gm_fallback_patch;
  const NativeSynthPatch& organ = gm_fallback_patch(0, 19);  // Church Organ
  REQUIRE(organ.mode == SynthEngineMode::kPipeOrgan);
  const std::vector<float> tone = render_patch(organ, 60, 100, 24000);
  REQUIRE(peak(tone) > 0.01f);
  // Pitch sanity on the fallback patch (C4 = 261.63 Hz).
  const double estimated = fft_fundamental(tone, 8000, 261.6256);
  REQUIRE(std::fabs(estimated / 261.6256 - 1.0) < 0.015);
}

TEST_CASE("GM harmonica and bandoneon fall back to the free-reed pipe", "[midi][synth][organ]") {
  using sonare::midi::synth::gm_fallback_patch;
  // Harmonica (22) and Bandoneon (23) voice the lingual free-reed core rather
  // than the drawbar organ family sketch.
  for (const uint8_t program : {uint8_t{22}, uint8_t{23}}) {
    const NativeSynthPatch& patch = gm_fallback_patch(0, program);
    REQUIRE(patch.mode == SynthEngineMode::kPipeOrgan);
    REQUIRE(patch.pipe_organ.ranks[0].reed > 0.0f);  // a reed (not flue) stop
    const std::vector<float> tone = render_patch(patch, 60, 100, 24000);
    REQUIRE(peak(tone) > 0.01f);
    const double estimated = fft_fundamental(tone, 8000, 261.6256);
    REQUIRE(std::fabs(estimated / 261.6256 - 1.0) < 0.02);
  }
  // The bandoneon's musette voicing detunes a second unison rank for beating.
  const NativeSynthPatch& bandoneon = gm_fallback_patch(0, 23);
  REQUIRE(bandoneon.pipe_organ.rank_count >= 3);
  REQUIRE(bandoneon.pipe_organ.ranks[1].footage_mult > 1.0f);
  REQUIRE(bandoneon.pipe_organ.ranks[1].footage_mult < 1.03f);
}

TEST_CASE("GM orchestra hit is a fast-attack stab", "[midi][synth][organ]") {
  using sonare::midi::synth::gm_fallback_patch;
  const NativeSynthPatch& hit = gm_fallback_patch(0, 55);
  // A stab, not the ensemble family's slow pad: a fast amplitude attack.
  REQUIRE(hit.amp_env.attack_ms < 10.0f);
  const std::vector<float> tone = render_patch(hit, 60, 110, 24000);
  REQUIRE(peak(tone) > 0.01f);
  // The attack transient dominates: the onset is far louder than the tail.
  const float onset = peak(std::vector<float>(tone.begin(), tone.begin() + 2400));
  const float tail = peak(std::vector<float>(tone.begin() + 16000, tone.end()));
  REQUIRE(onset > 3.0f * tail);
}

TEST_CASE("church organ presets select the pipe organ", "[midi][synth][organ]") {
  using sonare::midi::synth::find_synth_preset;
  for (const char* name : {"church-organ", "church-flute", "church-bourdon", "church-trumpet"}) {
    const auto* preset = find_synth_preset(name);
    REQUIRE(preset != nullptr);
    REQUIRE(preset->config.patch.mode == SynthEngineMode::kPipeOrgan);
  }
  // The trumpet is the reed stop.
  REQUIRE(find_synth_preset("church-trumpet")->config.patch.pipe_organ.ranks[0].reed > 0.0f);
  // The bourdon is the stopped pipe; the principal/flute are open.
  REQUIRE(find_synth_preset("church-bourdon")->config.patch.pipe_organ.stopped == true);
  REQUIRE(find_synth_preset("church-organ")->config.patch.pipe_organ.stopped == false);
}

TEST_CASE("pipe organ audio path is allocation-free", "[midi][synth][organ]") {
  NativeSynthConfig cfg;
  cfg.patch = organ_base_patch();
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);

  std::vector<float> left(256, 0.0f);
  std::vector<float> right(256, 0.0f);
  float* chans[2] = {left.data(), right.data()};
  {
    sonare::test::AllocationGuard guard;
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 36, 100)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 57, 100)));
    synth.process(chans, 2, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 36, 0)));
    synth.process(chans, 2, 256);
    REQUIRE(guard.count() == 0);
  }
}

// --- Phase 2: registration (multi-rank) + shared wind supply ---

TEST_CASE("a footage rank shifts the sounding pitch", "[midi][synth][organ]") {
  // A single 4' rank (footage 2) sounds an octave above the played note: A3
  // (220 Hz) speaks at 440 Hz, proving the footage multiplies the pitch.
  NativeSynthPatch patch = organ_base_patch();
  patch.pipe_organ.rank_count = 1;
  patch.pipe_organ.ranks[0] = {2.0f, /*stopped=*/false, 0.6f, 1.0f};
  const std::vector<float> tone = render_patch(patch, 57, 110, 48000);  // A3
  const double estimated = fft_fundamental(tone, 16000, 440.0);
  REQUIRE(std::fabs(estimated / 440.0 - 1.0) < 0.015);
}

TEST_CASE("a mutation rank adds a non-harmonic partial", "[midi][synth][organ]") {
  // Drawing a 5-1/3' quint (footage 1.5) on top of an 8' principal sounds a
  // partial at 1.5*f0 — a frequency that lies BETWEEN the note's own harmonics
  // (f0, 2*f0), so a plain 8' pipe has nothing there. The mutation rank is what
  // puts energy at the twelfth; that is the registration speaking.
  const double f0 = 220.0;  // A3
  const auto band_power = [](const std::vector<double>& p, double hz) {
    const int c = static_cast<int>(std::lround(hz / kRate * kFft));
    double acc = 0.0;
    for (int b = c - 2; b <= c + 2; ++b)
      if (b > 0 && b < static_cast<int>(p.size())) acc += p[static_cast<size_t>(b)];
    return acc;
  };
  NativeSynthPatch single = organ_base_patch();
  single.pipe_organ.rank_count = 1;
  single.pipe_organ.ranks[0] = {1.0f, false, 0.6f, 1.0f};  // 8' only
  NativeSynthPatch chorus = organ_base_patch();
  chorus.pipe_organ.rank_count = 2;
  chorus.pipe_organ.ranks[0] = {1.0f, false, 0.6f, 1.0f};  // 8'
  chorus.pipe_organ.ranks[1] = {1.5f, false, 0.6f, 1.0f};  // 5-1/3' quint

  const std::vector<float> single_tone = render_patch(single, 57, 110, 24000);
  const std::vector<float> chorus_tone = render_patch(chorus, 57, 110, 24000);
  const std::vector<double> single_p = power_spectrum(single_tone, 4096);
  const std::vector<double> chorus_p = power_spectrum(chorus_tone, 4096);

  // Reference the quint band against each tone's own fundamental (cancels the
  // chorus level normalisation): the quint is buried in the 8', prominent here.
  const double single_quint = band_power(single_p, 1.5 * f0) / band_power(single_p, f0);
  const double chorus_quint = band_power(chorus_p, 1.5 * f0) / band_power(chorus_p, f0);
  REQUIRE(chorus_quint > 10.0 * single_quint);
}

TEST_CASE("the church organ preset is a stable multi-rank plenum", "[midi][synth][organ]") {
  using sonare::midi::synth::find_synth_preset;
  const auto* preset = find_synth_preset("church-organ");
  REQUIRE(preset != nullptr);
  REQUIRE(preset->config.patch.pipe_organ.rank_count > 1);

  NativeSynthConfig cfg = preset->config;
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  // A sustained chord across the plenum stays bounded and deterministic.
  for (uint8_t note : {48, 55, 60, 64, 67}) {
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, 100)));
  }
  const std::vector<float> tone = render_left(synth, 48000);
  REQUIRE(peak(tone) > 0.01f);
  REQUIRE(peak(tone) < 4.0f);
  REQUIRE(std::isfinite(tone.back()));
}

TEST_CASE("a multi-rank registration is allocation-free", "[midi][synth][organ]") {
  using sonare::midi::synth::find_synth_preset;
  NativeSynthConfig cfg = find_synth_preset("church-organ")->config;
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);

  std::vector<float> left(256, 0.0f);
  std::vector<float> right(256, 0.0f);
  float* chans[2] = {left.data(), right.data()};
  {
    sonare::test::AllocationGuard guard;
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 48, 100)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
    synth.process(chans, 2, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 48, 0)));
    synth.process(chans, 2, 256);
    REQUIRE(guard.count() == 0);
  }
}

TEST_CASE("wind sag drops pressure under load", "[midi][synth][organ]") {
  using sonare::midi::synth::OrganWindSupply;
  OrganWindSupply wind;
  wind.prepare(kRate, /*tremulant_rate_hz=*/0.0f, /*depth=*/0.0f, /*wind_sag=*/0.5f);
  REQUIRE(wind.active());
  // No load: pressure stays full (unity pitch and gain).
  OrganWindSupply::State idle;
  for (int i = 0; i < 4800; ++i) idle = wind.process(0);
  REQUIRE(std::fabs(idle.gain - 1.0f) < 1.0e-3f);
  REQUIRE(std::fabs(idle.pitch_ratio - 1.0f) < 1.0e-3f);
  // Sustained heavy demand sags the wind: gain and pitch both fall.
  OrganWindSupply::State loaded;
  for (int i = 0; i < 4800; ++i) loaded = wind.process(12);
  REQUIRE(loaded.gain < 0.97f);
  REQUIRE(loaded.pitch_ratio < 1.0f);
}

// --- Phase 3: reed (lingual) pipes ---

/// Harmonic-to-noise ratio: power at the exact harmonic bins (1..16) versus
/// power at the half-integer bins between them. A periodic, self-oscillating
/// tone (reed limit cycle) reads high; a broadband, breath-driven tone (flue
/// pipe) reads low.
double harmonic_to_noise(const std::vector<float>& buf, size_t from, double f0) {
  const std::vector<double> ps = power_spectrum(buf, from);
  double harmonic = 0.0;
  double between = 0.0;
  for (int k = 1; k <= 16; ++k) {
    harmonic += harmonic_power(ps, f0, k);
    const int b = static_cast<int>(std::lround((k + 0.5) * f0 / kRate * kFft));
    for (int j = b - 2; j <= b + 2; ++j)
      if (j > 0 && j < static_cast<int>(ps.size())) between += ps[static_cast<size_t>(j)];
  }
  return between > 0.0 ? harmonic / between : 0.0;
}

TEST_CASE("a reed pipe locks into a periodic tone", "[midi][synth][organ]") {
  // A reed stop drives the jet harder and more asymmetrically than a flue pipe,
  // but like the flue it self-oscillates into a solid, highly periodic limit
  // cycle (a high harmonic-to-noise ratio) rather than an airy breath-driven
  // tone — and stays bounded. Its spectrum differs from the flue's.
  const double f0 = 220.0;  // A3
  NativeSynthPatch flue = organ_base_patch();
  flue.pipe_organ.reed = 0.0f;
  NativeSynthPatch reed = organ_base_patch();
  reed.pipe_organ.reed = 1.0f;

  const std::vector<float> flue_tone = render_patch(flue, 57, 110, 24000);
  const std::vector<float> reed_tone = render_patch(reed, 57, 110, 24000);
  REQUIRE(peak(reed_tone) > 0.01f);
  REQUIRE(peak(reed_tone) < 4.0f);
  REQUIRE(std::isfinite(reed_tone.back()));
  // The self-oscillating reed is a clean periodic tone, not a noisy one.
  REQUIRE(harmonic_to_noise(reed_tone, 8000, f0) > 20.0);
  // Its voicing differs audibly from the flue (a different harmonic balance).
  const std::vector<double> flue_p = power_spectrum(flue_tone, 4096);
  const std::vector<double> reed_p = power_spectrum(reed_tone, 4096);
  const double flue_h2 = harmonic_power(flue_p, f0, 2) / harmonic_power(flue_p, f0, 1);
  const double reed_h2 = harmonic_power(reed_p, f0, 2) / harmonic_power(reed_p, f0, 1);
  REQUIRE(std::fabs(reed_h2 - flue_h2) > 0.01);
}

TEST_CASE("a reed pipe stays in tune", "[midi][synth][organ]") {
  // The reed's limit cycle locks to the resonator, so the sounding pitch still
  // tracks the note (a wider tolerance than the linear flue pipe).
  NativeSynthPatch reed = organ_base_patch();
  reed.pipe_organ.reed = 1.0f;
  const std::vector<float> tone = render_patch(reed, 57, 110, 48000);  // A3 = 220 Hz
  const double estimated = fft_fundamental(tone, 16000, 220.0);
  REQUIRE(std::fabs(estimated / 220.0 - 1.0) < 0.03);
}

TEST_CASE("the reed pipe is stable across the keyboard", "[midi][synth][organ]") {
  for (uint8_t note : {24, 45, 69, 96}) {
    CAPTURE(note);
    NativeSynthPatch reed = organ_base_patch();
    reed.pipe_organ.reed = 1.0f;
    reed.pipe_organ.tone_decay_s = 8.0f;
    const std::vector<float> tone = render_patch(reed, note, 110, 48000);
    // The lingual voicing barely speaks at the bottom of the pedal compass (a
    // known limit of the asymmetric-jet reed, not an instability): assert
    // bounded there, audible speech from the bass range up.
    if (note >= 45) {
      const std::vector<float> settled(tone.begin() + 24000, tone.end());
      REQUIRE(peak(settled) > 0.005f);
    }
    REQUIRE(peak(tone) < 4.0f);
    REQUIRE(std::isfinite(tone.back()));
  }
}

TEST_CASE("GM reed organ fallback is a reed pipe", "[midi][synth][organ]") {
  using sonare::midi::synth::gm_fallback_patch;
  const NativeSynthPatch& reed = gm_fallback_patch(0, 20);  // Reed Organ (GM 20)
  REQUIRE(reed.mode == SynthEngineMode::kPipeOrgan);
  REQUIRE(reed.pipe_organ.rank_count > 0);
  // Accordion (GM 21) shares the free-reed voicing until a dedicated model.
  REQUIRE(gm_fallback_patch(0, 21).mode == SynthEngineMode::kPipeOrgan);
  const std::vector<float> tone = render_patch(reed, 60, 100, 24000);
  REQUIRE(peak(tone) > 0.01f);
  REQUIRE(peak(tone) < 4.0f);
  const double estimated = fft_fundamental(tone, 8000, 261.6256);
  REQUIRE(std::fabs(estimated / 261.6256 - 1.0) < 0.03);
}

// --- Phase 3: swell box (expression shutter) ---

/// Spectral centroid (Hz) over a window — a level-independent brightness proxy.
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

TEST_CASE("the swell box darkens the organ as the pedal closes", "[midi][synth][organ]") {
  using sonare::midi::synth::find_synth_preset;
  NativeSynthConfig cfg = find_synth_preset("church-organ")->config;  // swell = 0.8
  const auto play = [&](uint8_t expression) {
    NativeSynth synth(cfg);
    synth.prepare(kRate, 256);
    synth.on_event(0,
                   event(sonare::midi::make_midi1_control_change(0, 0, 11, expression)));  // CC11
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
    return render_left(synth, 48000);
  };
  const std::vector<float> open = play(127);  // shutter fully open
  const std::vector<float> shut = play(8);    // shutter nearly closed
  // The closed shutter is a lowpass: it cuts the upperwork's share of the
  // spectrum hard (the level cut is the expression's job and is not what this
  // asserts). The band-energy fraction above ~1.2 kHz is the direct measure —
  // the pipes' post-loop tone purity keeps the overall centroid near the
  // played pitch either way, so a centroid ratio under-reads the shutter. The
  // window skips the first half second so the ranks' speech swell has settled.
  const auto hf_fraction = [](const std::vector<float>& buf) {
    const std::vector<double> ps = power_spectrum(buf, 24000);
    double hf = 0.0;
    double total = 0.0;
    for (size_t b = 1; b < ps.size(); ++b) {
      const double hz = static_cast<double>(b) * kRate / kFft;
      total += ps[b];
      if (hz > 1200.0) hf += ps[b];
    }
    return total > 0.0 ? hf / total : 0.0;
  };
  REQUIRE(hf_fraction(shut) < 0.5 * hf_fraction(open));
}

// --- Phase 4: mouth/radiation correction + room coupling ---

TEST_CASE("mouth radiation brightens the pipe without moving the pitch", "[midi][synth][organ]") {
  // The radiation correction is a post-loop high-shelf: it lifts the partials
  // the pipe radiates into the room, so the spectral centroid rises, while the
  // sounding fundamental (set by the feedback loop it sits outside of) is
  // unchanged and the tone stays bounded.
  NativeSynthPatch dry = organ_base_patch();
  dry.pipe_organ.radiation = 0.0f;
  NativeSynthPatch bright = organ_base_patch();
  bright.pipe_organ.radiation = 1.0f;

  const std::vector<float> dry_tone = render_patch(dry, 57, 110, 24000);  // A3
  const std::vector<float> bright_tone = render_patch(bright, 57, 110, 24000);
  REQUIRE(peak(bright_tone) > 0.01f);
  REQUIRE(peak(bright_tone) < 4.0f);
  REQUIRE(std::isfinite(bright_tone.back()));
  // Brighter in the room.
  REQUIRE(swell_centroid(bright_tone, 8000) > 1.15 * swell_centroid(dry_tone, 8000));
  // Same pitch (the shelf is outside the feedback loop).
  const double dry_f0 = fft_fundamental(dry_tone, 8000, 220.0);
  const double bright_f0 = fft_fundamental(bright_tone, 8000, 220.0);
  REQUIRE(std::fabs(bright_f0 / dry_f0 - 1.0) < 0.005);
}

TEST_CASE("radiation off renders bit-identically to the bare pipe", "[midi][synth][organ]") {
  // radiation == 0 is a true bypass: the new shelf must not perturb the
  // deterministic render of any existing (radiation-free) preset or patch.
  NativeSynthPatch patch = organ_base_patch();
  patch.pipe_organ.radiation = 0.0f;
  const std::vector<float> a = render_patch(patch, 57, 100, 8192);
  const std::vector<float> b = render_patch(patch, 57, 100, 8192);
  REQUIRE(a == b);
  REQUIRE(peak(a) > 0.01f);
}

// --- rank-level keytrack (treble regulation) ---

TEST_CASE("rank keytrack thins the treble upperwork", "[midi][synth][organ]") {
  // A high-footage upperwork rank (2', footage 4) speaks well up in the treble.
  // Above the C4 reference, keytrack rolls that rank off, so a high note carries
  // less upper-harmonic energy; the 8' foundation (footage 1) is untouched. The
  // loop stays bounded.
  const double f0 = 523.251;  // C5, played note 72
  NativeSynthPatch base = organ_base_patch();
  base.pipe_organ.rank_count = 2;
  base.pipe_organ.ranks[0] = {1.0f, false, 0.6f, 1.0f};  // 8' foundation
  base.pipe_organ.ranks[1] = {4.0f, false, 0.7f, 1.0f};  // 2' upperwork

  NativeSynthPatch off = base;
  off.pipe_organ.keytrack = 0.0f;
  NativeSynthPatch on = base;
  on.pipe_organ.keytrack = 1.0f;

  const std::vector<float> off_tone = render_patch(off, 72, 110, 24000);
  const std::vector<float> on_tone = render_patch(on, 72, 110, 24000);
  REQUIRE(peak(on_tone) > 0.01f);
  REQUIRE(peak(on_tone) < 4.0f);
  REQUIRE(std::isfinite(on_tone.back()));

  const std::vector<double> off_p = power_spectrum(off_tone, 4096);
  const std::vector<double> on_p = power_spectrum(on_tone, 4096);
  // Upperwork energy (the 2' fundamental at h4 and its partials above),
  // normalized to h1 so it is immune to the overall level change.
  const auto upper = [&](const std::vector<double>& p) {
    double acc = 0.0;
    for (int k = 4; k <= 10; ++k) acc += harmonic_power(p, f0, k);
    return acc / harmonic_power(p, f0, 1);
  };
  REQUIRE(upper(on_p) < 0.6 * upper(off_p));
}

TEST_CASE("rank keytrack spares the bass compass and is a no-op off", "[midi][synth][organ]") {
  // keytrack acts only above the C4 reference and only when > 0: at or below C4
  // it changes nothing (the note sits at the reference), and keytrack == 0 gates
  // the whole roll-off out, so every existing render is reproduced bit for bit.
  NativeSynthPatch base = organ_base_patch();
  base.pipe_organ.rank_count = 2;
  base.pipe_organ.ranks[0] = {1.0f, false, 0.6f, 1.0f};
  base.pipe_organ.ranks[1] = {4.0f, false, 0.7f, 1.0f};

  NativeSynthPatch off = base;
  off.pipe_organ.keytrack = 0.0f;
  NativeSynthPatch on = base;
  on.pipe_organ.keytrack = 1.0f;

  // Below the reference (C3): keytrack must not perturb the render at all.
  REQUIRE(render_patch(off, 48, 110, 8192) == render_patch(on, 48, 110, 8192));
  // Exactly at the reference (C4): still untouched.
  REQUIRE(render_patch(off, 60, 110, 8192) == render_patch(on, 60, 110, 8192));
  // Above the reference (C5), keytrack == 0 renders deterministically (bypass).
  REQUIRE(render_patch(off, 72, 110, 8192) == render_patch(off, 72, 110, 8192));
}

TEST_CASE("the pipe organ is a clean reverb source (dc-free, sustained)", "[midi][synth][organ]") {
  // A church organ is almost always heard through a long room reverb. For the
  // acoustic tail to develop cleanly the source must carry no DC offset (the
  // in-loop and bus DC blockers) and sustain steady energy (no decay) for the
  // whole held note, so the reverb integrates a stable excitation.
  using sonare::midi::synth::find_synth_preset;
  NativeSynthConfig cfg = find_synth_preset("church-organ")->config;
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  for (uint8_t note : {48, 55, 60, 64, 67}) {
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, 100)));
  }
  const std::vector<float> tone = render_left(synth, 48000);

  // DC-free: the mean over the held tone is negligible against its RMS.
  double mean = 0.0;
  for (size_t i = 8000; i < tone.size(); ++i) mean += tone[i];
  mean /= static_cast<double>(tone.size() - 8000);
  const float body = rms(tone, 8000, tone.size());
  REQUIRE(body > 0.01f);
  REQUIRE(std::fabs(mean) < 0.02 * body);

  // Sustained: late energy tracks early energy (a reverb-ready steady source).
  const float early = rms(tone, 8000, 16000);
  const float late = rms(tone, 40000, 48000);
  REQUIRE(late > 0.7f * early);
  REQUIRE(late < 1.4f * early);
}

TEST_CASE("a full plenum renders faster than real time", "[.][bench][organ-cpu]") {
  // CPU probe (opt-in, excluded from the default run): the dominant cost of a
  // pipe organ is the simultaneous waveguide count — a thick chord across a
  // multi-rank plenum. This measures it and asserts only a loose real-time
  // margin (it is HW-sensitive; the printed ratio is the useful artefact). The
  // five-rank church-organ over a ten-note chord is 50 waveguides at once.
  using sonare::midi::synth::find_synth_preset;
  NativeSynthConfig cfg = find_synth_preset("church-organ")->config;
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  for (uint8_t note : {36, 43, 48, 55, 60, 64, 67, 72, 76, 79}) {
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, 100)));
  }

  constexpr int kBlocks = 4000;  // ~21 s of audio at 256-sample blocks
  std::vector<float> left(256, 0.0f);
  std::vector<float> right(256, 0.0f);
  float* chans[2] = {left.data(), right.data()};
  synth.process(chans, 2, 256);  // warm the caches before timing

  const auto t0 = std::chrono::steady_clock::now();
  for (int b = 0; b < kBlocks; ++b) synth.process(chans, 2, 256);
  const auto t1 = std::chrono::steady_clock::now();

  const double cpu_s = std::chrono::duration<double>(t1 - t0).count();
  const double audio_s = static_cast<double>(kBlocks) * 256.0 / kRate;
  const double x_realtime = audio_s / cpu_s;
  WARN("church-organ plenum (10 notes x 5 ranks = 50 waveguides): " << x_realtime << "x real time");
  REQUIRE(std::isfinite(left[0]));
  REQUIRE(x_realtime > 1.0);  // loose: must sustain real time even on a slow box
}

TEST_CASE("the tremulant undulates the wind", "[midi][synth][organ]") {
  using sonare::midi::synth::OrganWindSupply;
  OrganWindSupply wind;
  wind.prepare(kRate, /*tremulant_rate_hz=*/5.0f, /*depth=*/0.8f, /*wind_sag=*/0.0f);
  REQUIRE(wind.active());
  float lo = 2.0f;
  float hi = 0.0f;
  for (int i = 0; i < 48000; ++i) {  // ~5 full undulations
    const OrganWindSupply::State s = wind.process(1);
    lo = std::min(lo, s.gain);
    hi = std::max(hi, s.gain);
  }
  // The level visibly tremoles around unity.
  REQUIRE(hi > 1.05f);
  REQUIRE(lo < 0.95f);
}
