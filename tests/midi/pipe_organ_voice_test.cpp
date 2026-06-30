/// @file pipe_organ_voice_test.cpp
/// @brief Sustained flue-pipe waveguide (midi/synth/pipe_organ_voice):
///        fundamental tuning, the open vs stopped harmonic signature (all
///        harmonics vs odd only), prompt + sustained speech, note-off damping,
///        unconditional stability and deterministic rendering.

#include "midi/synth/pipe_organ_voice.h"

#include <catch2/catch_test_macros.hpp>
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
      NativeSynthPatch patch = organ_base_patch();
      patch.pipe_organ.stopped = stopped;
      patch.pipe_organ.tone_decay_s = 8.0f;
      const std::vector<float> tone = render_patch(patch, note, 110, 48000);
      REQUIRE(peak(tone) > 0.01f);
      REQUIRE(peak(tone) < 4.0f);
      REQUIRE(std::isfinite(tone.back()));
    }
  }
}

TEST_CASE("pipe organ tuning is accurate (open pipe)", "[midi][synth][organ]") {
  // A noise-driven flue pipe has a slightly broadened/breathy partial, so the
  // tolerance is wider than a struck-string voice (~25 cents) — the resonance
  // is read from the spectral peak, not the breathy zero crossings.
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
  // A stopped pipe is a half-length negative comb: same sounding fundamental,
  // odd harmonics only.
  NativeSynthPatch patch = organ_base_patch();
  patch.pipe_organ.stopped = true;
  const std::vector<float> tone = render_patch(patch, 57, 110, 48000);  // A3 = 220 Hz
  const double estimated = fft_fundamental(tone, 16000, 220.0);
  REQUIRE(std::fabs(estimated / 220.0 - 1.0) < 0.015);
}

TEST_CASE("stopped pipe suppresses even harmonics", "[midi][synth][organ]") {
  // Open pipe = full harmonic series; stopped pipe = odd harmonics only. The
  // second harmonic relative to the fundamental must collapse when stopped,
  // while the third harmonic stays present.
  const double f0 = 220.0;
  NativeSynthPatch open = organ_base_patch();
  open.pipe_organ.stopped = false;
  NativeSynthPatch stopped = organ_base_patch();
  stopped.pipe_organ.stopped = true;

  const std::vector<float> open_tone = render_patch(open, 57, 110, 24000);
  const std::vector<float> stopped_tone = render_patch(stopped, 57, 110, 24000);
  const std::vector<double> open_power = power_spectrum(open_tone, 4096);
  const std::vector<double> stopped_power = power_spectrum(stopped_tone, 4096);

  const double open_h2 = harmonic_power(open_power, f0, 2) / harmonic_power(open_power, f0, 1);
  const double stopped_h2 =
      harmonic_power(stopped_power, f0, 2) / harmonic_power(stopped_power, f0, 1);
  const double stopped_h3 =
      harmonic_power(stopped_power, f0, 3) / harmonic_power(stopped_power, f0, 1);

  // The stopped even harmonic is far weaker than the open one, and the stopped
  // odd (third) harmonic survives.
  REQUIRE(open_h2 > 8.0 * stopped_h2);
  REQUIRE(stopped_h3 > 4.0 * stopped_h2);
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

TEST_CASE("church organ presets select the pipe organ", "[midi][synth][organ]") {
  using sonare::midi::synth::find_synth_preset;
  for (const char* name : {"church-organ", "church-flute", "church-bourdon"}) {
    const auto* preset = find_synth_preset(name);
    REQUIRE(preset != nullptr);
    REQUIRE(preset->config.patch.mode == SynthEngineMode::kPipeOrgan);
  }
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
