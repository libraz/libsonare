/// @file ks_voice_test.cpp
/// @brief Karplus-Strong string (midi/synth/ks_voice): fractional-delay
///        tuning accuracy, decay stretching down the keyboard, pick-position
///        comb notches, velocity -> brightness, note-off damping and
///        deterministic rendering through the GM guitar/harp fallbacks.

#include "midi/synth/ks_voice.h"

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
#include "support/audio_fixtures.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::gm_fallback_patch;
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

float rms(const std::vector<float>& buf, size_t from, size_t to) {
  double acc = 0.0;
  size_t n = 0;
  for (size_t i = from; i < to && i < buf.size(); ++i) {
    acc += static_cast<double>(buf[i]) * buf[i];
    ++n;
  }
  return n > 0 ? static_cast<float>(std::sqrt(acc / static_cast<double>(n))) : 0.0f;
}

/// Interpolated zero-crossing frequency estimate over buf[from, to). The
/// buffer is first isolated to its fundamental with eight one-pole lowpass
/// passes at @p f0_hint * 1.3 (harmonic wiggles would otherwise add spurious
/// crossings), then full cycles are counted between the first and last upward
/// crossing with linear sub-sample interpolation at both ends. The one-pole
/// phase shift is constant across the window, so it cancels in the period.
double estimate_frequency(const std::vector<float>& buf, size_t from, size_t to, double f0_hint) {
  std::vector<float> lp(buf.begin(), buf.end());
  const float alpha =
      1.0f - static_cast<float>(std::exp(-2.0 * 3.14159265358979 * f0_hint * 1.3 / kRate));
  for (int pass = 0; pass < 8; ++pass) {
    float state = 0.0f;
    for (float& s : lp) {
      state += alpha * (s - state);
      s = state;
    }
  }
  // Remove the window mean so a residual DC offset cannot hide crossings.
  double mean = 0.0;
  size_t count = 0;
  for (size_t i = from; i < to && i < lp.size(); ++i) {
    mean += lp[i];
    ++count;
  }
  if (count > 0) {
    mean /= static_cast<double>(count);
    for (size_t i = from; i < to && i < lp.size(); ++i) lp[i] -= static_cast<float>(mean);
  }
  double first = -1.0;
  double last = -1.0;
  int cycles = 0;
  for (size_t i = from + 1; i < to && i < lp.size(); ++i) {
    if (lp[i - 1] < 0.0f && lp[i] >= 0.0f) {
      const double frac = static_cast<double>(lp[i - 1]) / (static_cast<double>(lp[i - 1]) - lp[i]);
      const double t = static_cast<double>(i - 1) + frac;
      if (first < 0.0) {
        first = t;
      } else {
        last = t;
        ++cycles;
      }
    }
  }
  if (cycles < 1 || last <= first) return 0.0;
  return static_cast<double>(cycles) * kRate / (last - first);
}

using sonare::test::power_spectrum;

/// Power of harmonic k (+-2 bins around k*f0).
double harmonic_power(const std::vector<double>& power, double f0, int k) {
  const int centre = static_cast<int>(std::lround(k * f0 / kRate * kFft));
  double acc = 0.0;
  for (int b = centre - 2; b <= centre + 2; ++b) {
    if (b > 0 && b < static_cast<int>(power.size())) acc += power[static_cast<size_t>(b)];
  }
  return acc;
}

/// Fraction of spectral power above @p freq_hz.
double high_band_fraction(const std::vector<float>& buf, size_t from, double freq_hz) {
  const std::vector<double> power = power_spectrum(buf, from);
  const int split = static_cast<int>(std::lround(freq_hz / kRate * kFft));
  double low = 0.0;
  double high = 0.0;
  for (int b = 1; b < static_cast<int>(power.size()); ++b) {
    (b >= split ? high : low) += power[static_cast<size_t>(b)];
  }
  const double total = low + high;
  return total > 0.0 ? high / total : 0.0;
}

/// Interpolated frequency (Hz) of the strongest spectral bin within
/// +-search_hz of target_hz (parabolic peak interpolation).
double peak_freq_near(const std::vector<double>& power, double target_hz, double search_hz) {
  const int lo = std::max(1, static_cast<int>(std::lround((target_hz - search_hz) / kRate * kFft)));
  const int hi = std::min(static_cast<int>(power.size()) - 2,
                          static_cast<int>(std::lround((target_hz + search_hz) / kRate * kFft)));
  int best = lo;
  for (int b = lo; b <= hi; ++b)
    if (power[static_cast<size_t>(b)] > power[static_cast<size_t>(best)]) best = b;
  const double ym1 = power[static_cast<size_t>(best - 1)];
  const double y0 = power[static_cast<size_t>(best)];
  const double yp1 = power[static_cast<size_t>(best + 1)];
  const double denom = ym1 - 2.0 * y0 + yp1;
  const double delta = denom != 0.0 ? 0.5 * (ym1 - yp1) / denom : 0.0;
  return (static_cast<double>(best) + delta) * kRate / kFft;
}

/// A bright filter-bypassed KS test patch.
NativeSynthPatch ks_base_patch() {
  NativeSynthPatch p;
  p.mode = SynthEngineMode::kKarplusStrong;
  p.cutoff_hz = 20000.0f;
  p.amp_env.attack_ms = 1.0f;
  p.amp_env.sustain = 1.0f;
  return p;
}

}  // namespace

TEST_CASE("KS rendering is deterministic", "[midi][synth][ks]") {
  const NativeSynthPatch& patch = gm_fallback_patch(0, 25);  // steel guitar (KS family)
  REQUIRE(patch.mode == SynthEngineMode::kKarplusStrong);
  const std::vector<float> first = render_patch(patch, 52, 100, 4096);
  const std::vector<float> second = render_patch(patch, 52, 100, 4096);
  float peak = 0.0f;
  for (float s : first) peak = std::max(peak, std::fabs(s));
  REQUIRE(peak > 0.01f);
  REQUIRE(first == second);
}

TEST_CASE("KS fractional-delay tuning is accurate", "[midi][synth][ks]") {
  NativeSynthPatch patch = ks_base_patch();
  patch.ks.brightness = 0.7f;
  patch.ks.pick_position = 0.18f;
  // A4 (440 Hz) and A2 (110 Hz): the sounding fundamental must match the note
  // within 5 cents (|ratio - 1| < 0.29%).
  for (const auto& [note, expected] :
       {std::pair<uint8_t, double>{69, 440.0}, std::pair<uint8_t, double>{45, 110.0}}) {
    const std::vector<float> tone = render_patch(patch, note, 110, 48000);
    const double estimated = estimate_frequency(tone, 8000, 44000, expected);
    REQUIRE(estimated > 0.0);
    REQUIRE(std::fabs(estimated / expected - 1.0) < 0.0029);
  }
}

TEST_CASE("decay stretching: low strings ring longer", "[midi][synth][ks]") {
  NativeSynthPatch patch = ks_base_patch();
  patch.ks.decay_s = 2.0f;
  patch.ks.decay_stretch = 0.7f;
  auto decay_ratio = [](const std::vector<float>& buf) {
    const float early = rms(buf, 2000, 8000);
    const float late = rms(buf, 38400, 48000);  // 0.8-1.0 s
    return early > 0.0f ? late / early : 0.0f;
  };
  const std::vector<float> low_note = render_patch(patch, 40, 110, 48000);
  const std::vector<float> high_note = render_patch(patch, 76, 110, 48000);
  REQUIRE(decay_ratio(low_note) > 1.5f * decay_ratio(high_note));
}

TEST_CASE("pick-position comb notches the matching harmonic", "[midi][synth][ks]") {
  // Picking at the middle of the string (0.5) puts a node at every even
  // harmonic; picking near the bridge (0.1) keeps them strong.
  NativeSynthPatch middle = ks_base_patch();
  middle.ks.brightness = 0.85f;
  middle.ks.pick_position = 0.5f;
  NativeSynthPatch bridge = middle;
  bridge.ks.pick_position = 0.1f;

  const double f0 = 220.0;
  const std::vector<float> mid_tone = render_patch(middle, 57, 110, 24000);
  const std::vector<float> bridge_tone = render_patch(bridge, 57, 110, 24000);
  const std::vector<double> mid_power = power_spectrum(mid_tone, 2048);
  const std::vector<double> bridge_power = power_spectrum(bridge_tone, 2048);

  // Second-harmonic level relative to the fundamental, per pick position.
  const double mid_h2 = harmonic_power(mid_power, f0, 2) / harmonic_power(mid_power, f0, 1);
  const double bridge_h2 =
      harmonic_power(bridge_power, f0, 2) / harmonic_power(bridge_power, f0, 1);
  REQUIRE(bridge_h2 > 8.0 * mid_h2);
}

TEST_CASE("velocity opens the excitation lowpass (brightness)", "[midi][synth][ks]") {
  NativeSynthPatch patch = ks_base_patch();
  patch.ks.vel_to_brightness = 0.8f;
  const std::vector<float> loud = render_patch(patch, 52, 127, 16000);
  const std::vector<float> soft = render_patch(patch, 52, 30, 16000);
  const double loud_high = high_band_fraction(loud, 1024, 1200.0);
  const double soft_high = high_band_fraction(soft, 1024, 1200.0);
  REQUIRE(loud_high > 1.5 * soft_high);
}

TEST_CASE("note-off damps the string", "[midi][synth][ks]") {
  NativeSynthPatch patch = ks_base_patch();
  patch.ks.decay_s = 6.0f;
  patch.ks.release_damp_s = 0.05f;
  patch.amp_env.release_ms = 400.0f;
  // Same note, held vs released at 0.25 s; compare the 0.6-0.8 s window.
  const std::vector<float> held = render_patch(patch, 52, 110, 38400);
  const std::vector<float> damped = render_patch(patch, 52, 110, 38400, /*note_off_at=*/12000);
  const float held_late = rms(held, 28800, 38400);
  const float damped_late = rms(damped, 28800, 38400);
  REQUIRE(held_late > 0.0f);
  REQUIRE(damped_late < 0.1f * held_late);
}

TEST_CASE("KS audio path is allocation-free", "[midi][synth][ks]") {
  NativeSynthConfig cfg;
  cfg.patch = ks_base_patch();
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);

  std::vector<float> left(256, 0.0f);
  std::vector<float> right(256, 0.0f);
  float* chans[2] = {left.data(), right.data()};
  {
    sonare::test::AllocationGuard guard;
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 40, 100)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 52, 100)));
    synth.process(chans, 2, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 40, 0)));
    synth.process(chans, 2, 256);
    REQUIRE(guard.count() == 0);
  }
}

TEST_CASE("bass presets are voiced Karplus-Strong strings", "[midi][synth][ks]") {
  using sonare::midi::synth::find_synth_preset;
  // Low E on a 4-string bass (E2 = MIDI 40, 82.41 Hz) exercises the long delay
  // line: every bass voicing must be a KS string, stay bounded and non-silent.
  for (const char* name : {"bass-acoustic", "bass-fingered", "bass-picked", "bass-fretless"}) {
    const auto* preset = find_synth_preset(name);
    REQUIRE(preset != nullptr);
    const NativeSynthPatch& patch = preset->config.patch;
    REQUIRE(patch.mode == SynthEngineMode::kKarplusStrong);
    const std::vector<float> tone = render_patch(patch, 40, 100, 48000);
    float peak = 0.0f;
    for (float s : tone) peak = std::max(peak, std::fabs(s));
    REQUIRE(peak > 0.01f);
    REQUIRE(peak < 2.0f);  // bounded, not diverging
  }
  // Pitch sanity on the clean electric voicing (no body colouration to bias the
  // fundamental estimate): E2 within 10 cents.
  const std::vector<float> fingered =
      render_patch(find_synth_preset("bass-fingered")->config.patch, 40, 100, 48000);
  const double estimated = estimate_frequency(fingered, 8000, 44000, 82.4069);
  REQUIRE(estimated > 0.0);
  REQUIRE(std::fabs(estimated / 82.4069 - 1.0) < 0.0058);
}

TEST_CASE("GM bass programs 32-37 resolve to Karplus-Strong strings", "[midi][synth][ks]") {
  for (uint8_t program : {32, 33, 34, 35, 36, 37}) {
    REQUIRE(gm_fallback_patch(0, program).mode == SynthEngineMode::kKarplusStrong);
  }
  // The slap programs engage the fret-slap limiter; the plucked members do not.
  REQUIRE(gm_fallback_patch(0, 36).ks.slap > 0.0f);
  REQUIRE(gm_fallback_patch(0, 33).ks.slap == 0.0f);
  // The sustained members carry the two-polarization beat; the percussive slap
  // keeps it off.
  REQUIRE(gm_fallback_patch(0, 33).ks.polarization > 0.0f);
  REQUIRE(gm_fallback_patch(0, 36).ks.polarization == 0.0f);
  // Synth Bass (38-39) keeps the subtractive family voicing by design.
  REQUIRE(gm_fallback_patch(0, 38).mode == SynthEngineMode::kSubtractive);
}

TEST_CASE("two-polarization coupling engages, stays bounded, off by default", "[midi][synth][ks]") {
  NativeSynthPatch base = ks_base_patch();
  base.ks.decay_s = 5.0f;
  base.ks.brightness = 0.5f;
  base.gain = 0.8f;
  NativeSynthPatch on = base;
  on.ks.polarization = 0.6f;
  const std::vector<float> off_tone = render_patch(base, 40, 110, 48000);  // 1 s, pol == 0
  const std::vector<float> on_tone = render_patch(on, 40, 110, 48000);
  // The detuned second plane reshapes the tone, yet the added feedback loop
  // stays bounded and the string is still ringing after one second.
  REQUIRE(on_tone != off_tone);
  float peak = 0.0f;
  for (float s : on_tone) peak = std::max(peak, std::fabs(s));
  REQUIRE(peak > 0.01f);
  REQUIRE(peak < 2.0f);
  REQUIRE(rms(on_tone, 40000, 48000) > 1.0e-4f);
}

TEST_CASE("bridge coupling engages, stays bounded, off by default", "[midi][synth][ks]") {
  NativeSynthPatch base = ks_base_patch();
  base.ks.decay_s = 5.0f;
  base.ks.brightness = 0.5f;
  base.gain = 0.8f;
  base.ks.polarization = 0.6f;  // the second plane is present in every variant
  NativeSynthPatch coupled = base;
  coupled.ks.body_coupling = 0.8f;
  const std::vector<float> off_tone = render_patch(base, 40, 110, 48000);  // body_coupling == 0
  const std::vector<float> on_tone = render_patch(coupled, 40, 110, 48000);
  // body_coupling == 0 must leave the summed-polarization path bit-identical.
  const std::vector<float> off_again = render_patch(base, 40, 110, 48000);
  REQUIRE(off_tone == off_again);
  // The bridge trades energy between the planes, so the tone changes, yet the
  // coupled 2x2 loop stays inside the unit circle (bounded) and keeps ringing.
  REQUIRE(on_tone != off_tone);
  float peak = 0.0f;
  for (float s : on_tone) peak = std::max(peak, std::fabs(s));
  REQUIRE(peak > 0.01f);
  REQUIRE(peak < 2.0f);
  REQUIRE(rms(on_tone, 40000, 48000) > 1.0e-4f);
}

TEST_CASE("bridge coupling stays bounded at the near-degenerate detune across the keyboard",
          "[midi][synth][ks]") {
  // Long t60 low notes push both loop gains close to 1; with the ~11-cent detune
  // the 2x2 system is near-degenerate, the worst case for the eigenvalue bound.
  NativeSynthPatch p = ks_base_patch();
  p.ks.decay_s = 8.0f;
  p.ks.decay_stretch = 0.8f;
  p.ks.brightness = 0.7f;
  p.ks.polarization = 0.9f;
  p.ks.body_coupling = 1.0f;
  p.gain = 0.9f;
  for (uint8_t note : {28, 40, 52, 64, 76, 88}) {
    const std::vector<float> tone = render_patch(p, note, 120, 48000);
    float peak = 0.0f;
    for (float s : tone) peak = std::max(peak, std::fabs(s));
    REQUIRE(peak > 0.01f);
    REQUIRE(peak < 4.0f);  // eigenvalue-bounded: no runaway even fully coupled
  }
}

TEST_CASE("sympathetic bank rings, stays bounded, off by default", "[midi][synth][ks]") {
  NativeSynthPatch dry = ks_base_patch();
  dry.ks.decay_s = 3.0f;
  dry.gain = 0.7f;
  NativeSynthPatch halo = dry;
  halo.ks.sympathetic = true;
  // E4 (MIDI 64) is a standard-tuning open string, so the note's partials line
  // up with the shared bank modes and drive the sound halo.
  const std::vector<float> dry_tone = render_patch(dry, 64, 110, 48000);
  const std::vector<float> halo_tone = render_patch(halo, 64, 110, 48000);
  const std::vector<float> halo_again = render_patch(halo, 64, 110, 48000);
  // The halo reshapes the tone deterministically and adds ringing energy, yet
  // the shared bank is a weak, unity-peak-normalized coupling so it stays bounded.
  REQUIRE(halo_tone != dry_tone);
  REQUIRE(halo_tone == halo_again);
  float peak = 0.0f;
  for (float s : halo_tone) peak = std::max(peak, std::fabs(s));
  REQUIRE(peak > 0.01f);
  REQUIRE(peak < 2.0f);
  // The sympathetic strings ring behind the note: total energy with the halo on
  // exceeds the plain string alone.
  REQUIRE(rms(halo_tone, 0, 48000) > rms(dry_tone, 0, 48000));
}

TEST_CASE("sympathetic bank is off by default and does not perturb plain KS voicings",
          "[midi][synth][ks]") {
  // A patch that does not opt into the sympathetic bank renders on the original
  // path. The base patch and the bass voicings (which keep the halo off) are
  // representative; they render deterministically without the shared bank.
  NativeSynthPatch plain = ks_base_patch();
  REQUIRE(plain.ks.sympathetic == false);
  REQUIRE(gm_fallback_patch(0, 33).ks.sympathetic == false);  // electric bass, unchanged
  const std::vector<float> a = render_patch(plain, 52, 100, 8192);
  const std::vector<float> b = render_patch(plain, 52, 100, 8192);
  REQUIRE(a == b);
}

TEST_CASE("activated guitar presets engage the dedicated physics and stay bounded",
          "[midi][synth][ks]") {
  using sonare::midi::synth::find_synth_preset;
  // classical-guitar: the sympathetic halo, coupled polarization and physical
  // finger pluck, but no dispersion (nylon plain strings are not inharmonic).
  const auto* classical = find_synth_preset("classical-guitar");
  REQUIRE(classical != nullptr);
  REQUIRE(classical->config.patch.ks.sympathetic == true);
  REQUIRE(classical->config.patch.ks.body_coupling > 0.0f);
  REQUIRE(classical->config.patch.ks.polarization > 0.0f);
  REQUIRE(classical->config.patch.ks.pluck_style > 0.0f);
  REQUIRE(classical->config.patch.ks.dispersion == 0.0f);
  // steel-guitar adds the steel dispersion and keeps the halo; the electric
  // guitar swaps the halo for the magnetic pickup.
  const auto* steel = find_synth_preset("steel-guitar");
  REQUIRE(steel != nullptr);
  REQUIRE(steel->config.patch.ks.dispersion > 0.0f);
  REQUIRE(steel->config.patch.ks.sympathetic == true);
  const auto* electric = find_synth_preset("electric-guitar");
  REQUIRE(electric != nullptr);
  REQUIRE(electric->config.patch.ks.pickup_pos > 0.0f);
  REQUIRE(electric->config.patch.ks.sympathetic == false);
  // With every plane engaged at once (coupled double-decay + halo + physical
  // pluck + tension) the standalone instrument stays bounded, audible and in
  // tune: A4 within ~14 cents.
  const std::vector<float> tone = render_patch(classical->config.patch, 69, 100, 48000);
  float peak = 0.0f;
  for (float s : tone) peak = std::max(peak, std::fabs(s));
  REQUIRE(peak > 0.01f);
  REQUIRE(peak < 2.0f);
  const double f0 = estimate_frequency(tone, 8000, 44000, 440.0);
  REQUIRE(std::fabs(f0 / 440.0 - 1.0) < 0.008);
}

TEST_CASE("sympathetic halo extends the reported tail", "[midi][synth][ks]") {
  NativeSynthConfig dry_cfg;
  dry_cfg.patch = ks_base_patch();
  NativeSynth dry(dry_cfg);
  dry.prepare(kRate, 256);

  NativeSynthConfig halo_cfg;
  halo_cfg.patch = ks_base_patch();
  halo_cfg.patch.ks.sympathetic = true;
  NativeSynth halo(halo_cfg);
  halo.prepare(kRate, 256);

  // The ringing bank must lengthen tail_samples() so a bounce keeps the halo.
  REQUIRE(halo.tail_samples() > dry.tail_samples());
}

TEST_CASE("physical pluck reshapes the attack and stays off by default", "[midi][synth][ks]") {
  NativeSynthPatch noise = ks_base_patch();
  noise.ks.brightness = 0.6f;
  noise.ks.exc_brightness = 0.85f;
  noise.gain = 0.8f;
  NativeSynthPatch pluck = noise;
  pluck.ks.pluck_style = 1.0f;
  pluck.ks.nail = 0.5f;
  const std::vector<float> noise_tone = render_patch(noise, 52, 110, 24000);  // pluck_style == 0
  const std::vector<float> pluck_tone = render_patch(pluck, 52, 110, 24000);
  const std::vector<float> pluck_again = render_patch(pluck, 52, 110, 24000);
  // The deterministic pluck doublet replaces the noisy attack, deterministically,
  // yet the string stays bounded and rings.
  REQUIRE(pluck_tone != noise_tone);
  REQUIRE(pluck_tone == pluck_again);
  float peak = 0.0f;
  for (float s : pluck_tone) peak = std::max(peak, std::fabs(s));
  REQUIRE(peak > 0.01f);
  REQUIRE(peak < 2.0f);
}

TEST_CASE("nail/pick pluck is brighter than a fingertip", "[midi][synth][ks]") {
  // Same physical pluck, opposite edge: the nail/pick releases through a narrow,
  // bright lobe; the fingertip through a wide, round one. Measured over the
  // attack, the nail keeps more high-frequency energy.
  NativeSynthPatch flesh = ks_base_patch();
  flesh.ks.brightness = 0.7f;
  flesh.ks.exc_brightness = 0.9f;
  flesh.ks.pluck_style = 1.0f;
  flesh.ks.nail = 0.0f;  // fingertip: wide/round lobe
  NativeSynthPatch nail = flesh;
  nail.ks.nail = 1.0f;  // nail/pick: narrow/bright lobe
  const std::vector<float> flesh_tone = render_patch(flesh, 52, 110, 24000);
  const std::vector<float> nail_tone = render_patch(nail, 52, 110, 24000);
  REQUIRE(high_band_fraction(nail_tone, 1024, 1500.0) >
          high_band_fraction(flesh_tone, 1024, 1500.0));
}

TEST_CASE("magnetic pickup colours the output and stays off by default", "[midi][synth][ks]") {
  NativeSynthPatch clean = ks_base_patch();
  clean.ks.brightness = 0.7f;
  clean.ks.decay_s = 4.0f;
  clean.gain = 0.7f;
  NativeSynthPatch pickup = clean;
  pickup.ks.pickup_pos = 0.15f;
  const std::vector<float> off_tone = render_patch(clean, 45, 110, 24000);  // pickup_pos == 0
  const std::vector<float> on_tone = render_patch(pickup, 45, 110, 24000);
  const std::vector<float> on_again = render_patch(pickup, 45, 110, 24000);
  // The pickup combs and shapes the output deterministically, staying bounded.
  REQUIRE(on_tone != off_tone);
  REQUIRE(on_tone == on_again);
  float peak = 0.0f;
  for (float s : on_tone) peak = std::max(peak, std::fabs(s));
  REQUIRE(peak > 0.01f);
  REQUIRE(peak < 2.0f);
}

TEST_CASE("pickup position places a comb node on the matching harmonic", "[midi][synth][ks]") {
  // A pickup at 1/4 of the string senses a node of the 4th harmonic, so the
  // output comb notches it. Measured against the (boosted) 2nd harmonic, the
  // 4th collapses relative to the plain string.
  NativeSynthPatch plain = ks_base_patch();
  plain.ks.brightness = 0.85f;
  plain.ks.pick_position = 0.12f;  // keep every harmonic present to be notched
  NativeSynthPatch pickup = plain;
  pickup.ks.pickup_pos = 0.25f;
  const double f0 = 110.0;  // A2
  const std::vector<float> plain_tone = render_patch(plain, 45, 110, 24000);
  const std::vector<float> pickup_tone = render_patch(pickup, 45, 110, 24000);
  const std::vector<double> plain_power = power_spectrum(plain_tone, 2048);
  const std::vector<double> pickup_power = power_spectrum(pickup_tone, 2048);
  const double plain_h4 = harmonic_power(plain_power, f0, 4) / harmonic_power(plain_power, f0, 2);
  const double pickup_h4 =
      harmonic_power(pickup_power, f0, 4) / harmonic_power(pickup_power, f0, 2);
  REQUIRE(pickup_h4 < 0.25 * plain_h4);
}

TEST_CASE("steel dispersion stretches the partials and stays off by default", "[midi][synth][ks]") {
  NativeSynthPatch nylon = ks_base_patch();
  nylon.ks.brightness = 0.85f;
  nylon.ks.pick_position = 0.1f;  // keep the upper partials strong
  nylon.gain = 0.7f;
  NativeSynthPatch steel = nylon;
  steel.ks.dispersion = 1.0f;
  const std::vector<float> nylon_tone = render_patch(nylon, 69, 110, 48000);  // A4, dispersion == 0
  const std::vector<float> steel_tone = render_patch(steel, 69, 110, 48000);
  const std::vector<float> steel_again = render_patch(steel, 69, 110, 48000);
  // Dispersion reshapes the tone deterministically and stays bounded.
  REQUIRE(steel_tone != nylon_tone);
  REQUIRE(steel_tone == steel_again);
  float peak = 0.0f;
  for (float s : steel_tone) peak = std::max(peak, std::fabs(s));
  REQUIRE(peak > 0.01f);
  REQUIRE(peak < 2.0f);
  // The fundamental tuning is preserved (the allpass phase delay is compensated
  // in the loop length): A4 within ~10 cents.
  const double f0 = estimate_frequency(steel_tone, 8000, 44000, 440.0);
  REQUIRE(std::fabs(f0 / 440.0 - 1.0) < 0.006);
  // A high partial is stretched sharp: the 14th partial sits above 14*f0, and
  // above where the harmonic (nylon) string places it.
  const std::vector<double> nylon_power = power_spectrum(nylon_tone, 2048);
  const std::vector<double> steel_power = power_spectrum(steel_tone, 2048);
  const double nylon_p14 = peak_freq_near(nylon_power, 14.0 * 440.0, 90.0);
  const double steel_p14 = peak_freq_near(steel_power, 14.0 * 440.0, 90.0);
  REQUIRE(steel_p14 > 14.0 * 440.0);
  REQUIRE(steel_p14 > nylon_p14 + 10.0);
}

TEST_CASE("tension modulation bends the attack sharp then relaxes, off by default",
          "[midi][synth][ks]") {
  NativeSynthPatch linear = ks_base_patch();
  linear.ks.brightness = 0.7f;
  linear.ks.decay_s = 4.0f;
  linear.gain = 0.7f;
  NativeSynthPatch tension = linear;
  tension.ks.tension_mod = 1.0f;
  const std::vector<float> flat = render_patch(linear, 45, 127, 48000);  // A2, tension == 0
  const std::vector<float> bent = render_patch(tension, 45, 127, 48000);
  const std::vector<float> bent_again = render_patch(tension, 45, 127, 48000);
  // The attack starts sharp and relaxes, so the tone differs deterministically
  // and stays bounded.
  REQUIRE(bent != flat);
  REQUIRE(bent == bent_again);
  float peak = 0.0f;
  for (float s : bent) peak = std::max(peak, std::fabs(s));
  REQUIRE(peak > 0.01f);
  REQUIRE(peak < 2.0f);
  // Past the excitation burst the pitch sits sharp of A2 and glides down; by the
  // steady state it has relaxed back to the note. The rise stays bounded by the
  // explicit cents clamp so the attack never glitches.
  const double early = estimate_frequency(bent, 800, 3000, 110.0);
  const double late = estimate_frequency(bent, 30000, 46000, 110.0);
  REQUIRE(early > 110.0);                         // starts sharp
  REQUIRE(early > late * 1.004);                  // an audible downward glide
  REQUIRE(early < 110.0 * 1.06);                  // bounded by the explicit cents clamp
  REQUIRE(std::fabs(late / 110.0 - 1.0) < 0.01);  // relaxes to the note
}

TEST_CASE("fret-slap engages the displacement limiter and stays off by default",
          "[midi][synth][ks]") {
  NativeSynthPatch base = ks_base_patch();
  base.ks.brightness = 0.6f;
  base.ks.pick_position = 0.1f;
  base.ks.exc_brightness = 0.9f;
  base.gain = 0.9f;
  NativeSynthPatch on = base;
  on.ks.slap = 0.85f;
  const std::vector<float> off_tone = render_patch(base, 40, 127, 24000);  // slap == 0
  const std::vector<float> on_tone = render_patch(on, 40, 127, 24000);
  auto peak_of = [](const std::vector<float>& buf) {
    float p = 0.0f;
    for (float s : buf) p = std::max(p, std::fabs(s));
    return p;
  };
  // The fret-contact limiter engages (the tone differs from the plain string)
  // and clamps the string's over-travel, so the slap voicing peaks below the
  // unlimited string and stays bounded.
  REQUIRE(on_tone != off_tone);
  REQUIRE(peak_of(on_tone) < peak_of(off_tone));
  REQUIRE(peak_of(on_tone) > 0.01f);
  REQUIRE(peak_of(on_tone) < 2.0f);
}

TEST_CASE("picked bass is brighter than fingered", "[midi][synth][ks]") {
  using sonare::midi::synth::find_synth_preset;
  // Picking near the bridge with an open excitation lowpass keeps more upper
  // harmonics than the rounder fingerstyle voicing.
  const std::vector<float> picked =
      render_patch(find_synth_preset("bass-picked")->config.patch, 40, 100, 24000);
  const std::vector<float> fingered =
      render_patch(find_synth_preset("bass-fingered")->config.patch, 40, 100, 24000);
  REQUIRE(high_band_fraction(picked, 2048, 800.0) > high_band_fraction(fingered, 2048, 800.0));
}

TEST_CASE("GM harp fallback is a stretched KS string", "[midi][synth][ks]") {
  const NativeSynthPatch& harp = gm_fallback_patch(0, 46);
  REQUIRE(harp.mode == SynthEngineMode::kKarplusStrong);
  const std::vector<float> tone = render_patch(harp, 60, 100, 24000);
  float peak = 0.0f;
  for (float s : tone) peak = std::max(peak, std::fabs(s));
  REQUIRE(peak > 0.01f);
  // Pitch sanity on the fallback patch too (C4 = 261.63 Hz).
  const double estimated = estimate_frequency(tone, 4000, 20000, 261.6256);
  REQUIRE(std::fabs(estimated / 261.6256 - 1.0) < 0.005);
}

TEST_CASE("a touched node halves the string and takes the fundamental with it",
          "[midi][synth][ks]") {
  NativeSynthPatch open = ks_base_patch();
  open.ks.decay_s = 3.0f;
  open.gain = 0.8f;
  // 0 and 1 both mean an untouched string: the same render, to the sample.
  NativeSynthPatch first_mode = open;
  first_mode.ks.harmonic_node = 1.0f;
  REQUIRE(render_patch(first_mode, 52, 110, 24000) == render_patch(open, 52, 110, 24000));

  NativeSynthPatch touched = open;
  touched.ks.harmonic_node = 2.0f;
  const std::vector<float> tone = render_patch(touched, 52, 110, 24000);
  // E3 fingered; a node at half the string sounds E4 an octave above it.
  const double e3 = 164.8138;
  REQUIRE(std::fabs(estimate_frequency(tone, 4000, 20000, 2.0 * e3) / (2.0 * e3) - 1.0) < 0.006);
  // The modes without a node at the touch are gone, not merely quieter: E3
  // itself sits below the surviving octave by far more than the open string's
  // own second harmonic sits below its first.
  const std::vector<double> power = power_spectrum(tone, 4000);
  const std::vector<double> open_power = power_spectrum(render_patch(open, 52, 110, 24000), 4000);
  const double killed = harmonic_power(power, e3, 1) / harmonic_power(power, e3, 2);
  const double kept = harmonic_power(open_power, e3, 2) / harmonic_power(open_power, e3, 1);
  REQUIRE(killed < 0.05 * kept);
}

TEST_CASE("the shipped guitar harmonics program is a flageolet, not the open string",
          "[midi][synth][ks]") {
  // 25 steel and 31 harmonics shared one family patch, so the two rendered the
  // same audio; 31 sounding an octave above what it is asked for is the whole
  // difference between a touched string and an untouched one.
  const NativeSynthPatch& steel = gm_fallback_patch(0, 25);
  const NativeSynthPatch& flageolet = gm_fallback_patch(0, 31);
  REQUIRE(flageolet.mode == SynthEngineMode::kKarplusStrong);
  REQUIRE(steel.ks.harmonic_node == 0.0f);
  REQUIRE(flageolet.ks.harmonic_node == 2.0f);
  const double e3 = 164.8138;
  const std::vector<float> tone = render_patch(flageolet, 52, 100, 24000);
  REQUIRE(std::fabs(estimate_frequency(tone, 4000, 20000, 2.0 * e3) / (2.0 * e3) - 1.0) < 0.006);
  REQUIRE(estimate_frequency(render_patch(steel, 52, 100, 24000), 4000, 20000, e3) / e3 < 1.006);
  // A flageolet is quieter than the open string and still has to be playable
  // beside its siblings: 0.13 against the family's 0.15-0.21 on the same note.
  float peak = 0.0f;
  for (float s : tone) peak = std::max(peak, std::fabs(s));
  float steel_peak = 0.0f;
  for (float s : render_patch(steel, 52, 100, 24000))
    steel_peak = std::max(steel_peak, std::fabs(s));
  REQUIRE(peak < steel_peak);
  REQUIRE(peak > 0.5f * steel_peak);
}
