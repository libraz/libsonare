/// @file free_reed_voice_test.cpp
/// @brief Free-reed slot flow (midi/synth/free_reed_voice): the shipped saw
///        shaper stays reachable and bit-identical, the slot flow lifts the
///        second partial over the first where the saw cannot, its duty places
///        the null the measured references carry, its swing darkens with the
///        register where a fixed shape cannot, it leaves no steady flow under
///        the note, and it stays bounded across the keyboard. The musette taper
///        holds the beat rate across the compass. The onset bellows overshoot is
///        the identity at 0 dB, falls toward the treble when key-tracked, and
///        yields to a streamed bellows.

#include "midi/synth/free_reed_voice.h"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"
#include "support/audio_fixtures.h"
#include "support/midi_render.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::FreeReedPatchParams;
using sonare::midi::synth::FreeReedVoiceCore;
using sonare::midi::synth::gm_fallback_patch;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::SynthEngineMode;

using sonare::test::event;
using sonare::test::harmonic_power;
using sonare::test::kFft;
using sonare::test::kRate;
using sonare::test::power_spectrum;
using sonare::test::render_left;

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

/// A free reed with no musette pair, so one note gives one clean ladder.
NativeSynthPatch free_reed_patch() {
  NativeSynthPatch patch{};
  patch.mode = SynthEngineMode::kFreeReed;
  patch.amp_env.attack_ms = 5.0f;
  patch.amp_env.sustain = 1.0f;
  patch.amp_env.release_ms = 120.0f;
  patch.cutoff_hz = 20000.0f;
  patch.gain = 0.8f;
  patch.free_reed.brightness = 0.85f;
  patch.free_reed.detune = 0.0f;
  patch.free_reed.breath_noise = 0.0f;
  return patch;
}

float peak(const std::vector<float>& buf) {
  float p = 0.0f;
  for (float s : buf) p = std::max(p, std::fabs(s));
  return p;
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

double rms(const std::vector<float>& buf, size_t from, size_t to) {
  double acc = 0.0;
  size_t n = 0;
  for (size_t i = from; i < to && i < buf.size(); ++i) {
    acc += static_cast<double>(buf[i]) * buf[i];
    ++n;
  }
  return n > 0 ? std::sqrt(acc / static_cast<double>(n)) : 0.0;
}

/// Loudest 10 ms of the first 150 ms over the 0.2-0.4 s held level, in dB.
double onset_lift_db(const std::vector<float>& buf) {
  const size_t win = static_cast<size_t>(0.010 * kRate);
  double top = 0.0;
  for (size_t i = 0; i + win <= static_cast<size_t>(0.150 * kRate); i += win / 2) {
    top = std::max(top, rms(buf, i, i + win));
  }
  const double held = rms(buf, static_cast<size_t>(0.2 * kRate), static_cast<size_t>(0.4 * kRate));
  return 20.0 * std::log10(top / held);
}

bool all_finite(const std::vector<float>& buf) {
  for (float s : buf) {
    if (!std::isfinite(s)) return false;
  }
  return true;
}

double note_hz(uint8_t note) {
  return 440.0 * std::pow(2.0, (static_cast<double>(note) - 69.0) / 12.0);
}

double rms_after(sonare::midi::synth::FreeReedVoiceCore& voice, float pitch_ratio, int warmup,
                 int samples) {
  for (int i = 0; i < warmup; ++i) voice.render(pitch_ratio);
  double sum = 0.0;
  for (int i = 0; i < samples; ++i) {
    const double sample = voice.render(pitch_ratio);
    sum += sample * sample;
  }
  return std::sqrt(sum / static_cast<double>(samples));
}

double rms_after_pitch_change(sonare::midi::synth::FreeReedVoiceCore& voice, float start_ratio,
                              float target_ratio, int warmup, int samples) {
  for (int i = 0; i < warmup; ++i) voice.render(start_ratio);
  return rms_after(voice, target_ratio, 0, samples);
}

/// Amplitude of the rendered tone's component at @p frequency, by projection.
double amplitude_at(const FreeReedPatchParams& params, double sample_rate, uint8_t note,
                    double frequency, int warmup, int samples) {
  FreeReedVoiceCore voice;
  voice.start(params, sample_rate, note, sonare::midi::Velocity16::from7(100), 42);
  for (int i = 0; i < warmup; ++i) voice.render(1.0f);
  const double omega = 2.0 * 3.14159265358979323846 * frequency / sample_rate;
  std::complex<double> projection(0.0, 0.0);
  for (int i = 0; i < samples; ++i) {
    projection += static_cast<double>(voice.render(1.0f)) *
                  std::exp(std::complex<double>(0.0, -omega * static_cast<double>(i)));
  }
  return 2.0 * std::abs(projection) / static_cast<double>(samples);
}

double fundamental_amplitude(const FreeReedPatchParams& params, double sample_rate, uint8_t note,
                             int warmup, int samples) {
  return amplitude_at(params, sample_rate, note, note_hz(note), warmup, samples);
}

}  // namespace

TEST_CASE("the free reed musette taper is off without a musette", "[midi][synth][free_reed]") {
  NativeSynthPatch plain = free_reed_patch();
  NativeSynthPatch tapered = plain;
  tapered.free_reed.musette_taper = 1.0f;
  tapered.free_reed.musette_flat = true;
  REQUIRE(render_patch(plain, 60, 100, 24000) == render_patch(tapered, 60, 100, 24000));
}

TEST_CASE("the free reed musette taper holds the beat rate across the compass",
          "[midi][synth][free_reed]") {
  FreeReedPatchParams params;
  params.slot_duty = 0.29f;
  params.slot_return = 0.7f;
  params.detune = 1.0f;
  params.breath_noise = 0.0f;
  // detune 1 puts the partner 15 c sharp. Untapered that holds at every note;
  // fully tapered the partner keeps A4's offset in Hz, which at F3 is 37.8 c, so
  // the two laws put the partner on different lines and each leaves the other's empty.
  const uint8_t note = 53;
  const double f0 = note_hz(note);
  const double at_cents = f0 * std::pow(2.0, 15.0 / 1200.0);
  const double at_hz = f0 + 440.0 * (std::pow(2.0, 15.0 / 1200.0) - 1.0);
  const int n = 4 * 48000;
  params.musette_taper = 0.0f;
  const double cents_law_on_cents = amplitude_at(params, 48000.0, note, at_cents, 24000, n);
  const double cents_law_on_hz = amplitude_at(params, 48000.0, note, at_hz, 24000, n);
  params.musette_taper = 1.0f;
  const double hz_law_on_cents = amplitude_at(params, 48000.0, note, at_cents, 24000, n);
  const double hz_law_on_hz = amplitude_at(params, 48000.0, note, at_hz, 24000, n);
  REQUIRE(cents_law_on_cents > 4.0 * cents_law_on_hz);
  REQUIRE(hz_law_on_hz > 4.0 * hz_law_on_cents);
}

TEST_CASE("the free reed's slot flow is off by default", "[midi][synth][free_reed]") {
  NativeSynthPatch plain = free_reed_patch();
  NativeSynthPatch gated = plain;
  // Every companion field at a value that would change the sound if the slot
  // flow were reached; the zero duty is what has to keep it out.
  gated.free_reed.slot_duty = 0.0f;
  gated.free_reed.slot_return = 1.4f;
  gated.free_reed.slot_gap = 0.47f;
  gated.free_reed.slot_swing = 3.0f;
  gated.free_reed.radiation = 0.0f;
  gated.free_reed.radiation_keytrack = 1.0f;
  REQUIRE(render_patch(plain, 60, 100, 24000) == render_patch(gated, 60, 100, 24000));
}

TEST_CASE("the free reed radiation keytrack restores the monopole's register law",
          "[midi][synth][free_reed]") {
  NativeSynthPatch flat = free_reed_patch();
  flat.free_reed.slot_duty = 0.35f;
  flat.free_reed.slot_return = 0.59f;
  flat.free_reed.radiation = 1.0f;
  NativeSynthPatch tracked = flat;
  tracked.free_reed.radiation_keytrack = 1.0f;
  const auto held_db = [](const NativeSynthPatch& patch, uint8_t note) {
    const std::vector<float> tone = render_patch(patch, note, 100, 24000);
    return 20.0 * std::log10(rms(tone, 12000, 24000));
  };
  // Normalised, two octaves cost the level nothing; tracked, they are the +12 dB
  // a small source radiates, within the ladder's own change across the compass.
  const double flat_rise = held_db(flat, 81) - held_db(flat, 57);
  const double tracked_rise = held_db(tracked, 81) - held_db(tracked, 57);
  INFO("rise over two octaves: normalised " << flat_rise << " dB, tracked " << tracked_rise);
  REQUIRE(tracked_rise - flat_rise > 10.0);
  REQUIRE(tracked_rise - flat_rise < 14.0);
}

TEST_CASE("the free reed's slot flow lifts the second partial over the first",
          "[midi][synth][free_reed]") {
  const uint8_t note = 59;
  const double f0 = note_hz(note);

  NativeSynthPatch saw = free_reed_patch();
  NativeSynthPatch slot = saw;
  slot.free_reed.slot_duty = 0.56f;
  slot.free_reed.slot_return = 0.8f;
  slot.free_reed.slot_gap = 0.41f;
  slot.free_reed.radiation = 1.0f;

  const std::vector<double> saw_ps = power_spectrum(render_patch(saw, note, 100, 24000), 8000);
  const std::vector<double> slot_ps = power_spectrum(render_patch(slot, note, 100, 24000), 8000);
  const double saw_ratio = harmonic_power(saw_ps, f0, 2) / harmonic_power(saw_ps, f0, 1);
  const double slot_ratio = harmonic_power(slot_ps, f0, 2) / harmonic_power(slot_ps, f0, 1);

  // The saw shaper's ladder falls from the fundamental whatever its knobs do;
  // the references' second partial sits some 8 dB above theirs, and only the
  // radiated slot flow gets there.
  REQUIRE(saw_ratio < 1.0);
  REQUIRE(slot_ratio > 2.0);
}

TEST_CASE("the free reed's slot duty places the null", "[midi][synth][free_reed]") {
  const uint8_t note = 47;
  const double f0 = note_hz(note);

  // A flow hump of duty d nulls near the harmonic 2/d, so a narrow slot puts
  // its null high and a wide one puts it low. The references' nulls are what
  // the duty was fitted to.
  NativeSynthPatch narrow = free_reed_patch();
  narrow.free_reed.slot_duty = 0.29f;
  narrow.free_reed.slot_return = 0.0f;
  narrow.free_reed.radiation = 1.0f;
  NativeSynthPatch wide = narrow;
  wide.free_reed.slot_duty = 0.56f;

  const std::vector<double> narrow_ps =
      power_spectrum(render_patch(narrow, note, 100, 24000), 8000);
  const std::vector<double> wide_ps = power_spectrum(render_patch(wide, note, 100, 24000), 8000);

  const auto deepest = [&](const std::vector<double>& ps) {
    int best = 2;
    double best_ratio = 1e30;
    for (int k = 2; k <= 10; ++k) {
      const double r = harmonic_power(ps, f0, k) /
                       (harmonic_power(ps, f0, k - 1) + harmonic_power(ps, f0, k + 1) + 1e-30);
      if (r < best_ratio) {
        best_ratio = r;
        best = k;
      }
    }
    return best;
  };
  REQUIRE(deepest(narrow_ps) > deepest(wide_ps));
}

TEST_CASE("the free reed's slot swing darkens with the register where the hump pair cannot",
          "[midi][synth][free_reed]") {
  NativeSynthPatch humps = free_reed_patch();
  humps.free_reed.slot_duty = 0.35f;
  humps.free_reed.slot_return = 0.59f;
  humps.free_reed.slot_gap = 0.04f;
  humps.free_reed.radiation = 1.0f;
  NativeSynthPatch swing = humps;
  swing.free_reed.slot_swing = 2.0f;

  // h2-h6 against h1, in dB, over the held note.
  const auto stack_db = [](const NativeSynthPatch& patch, uint8_t note) {
    const double f0 = note_hz(note);
    const std::vector<double> ps = power_spectrum(render_patch(patch, note, 100, 24000), 8000);
    double sum = 0.0;
    for (int k = 2; k <= 6; ++k) {
      sum += 10.0 * std::log10(harmonic_power(ps, f0, k) / harmonic_power(ps, f0, 1) + 1e-30);
    }
    return sum / 5.0;
  };
  const auto ratio = [](const NativeSynthPatch& patch, uint8_t note, int k, int j) {
    const double f0 = note_hz(note);
    const std::vector<double> ps = power_spectrum(render_patch(patch, note, 100, 24000), 8000);
    return harmonic_power(ps, f0, k) / harmonic_power(ps, f0, j);
  };

  // A fixed flow shape gives every note the same ladder, so with the body filter
  // open the hump pair's stack barely moves between F3 and B5; the swing's falls
  // by more than 10 dB as the tongue stops saturating the slot.
  const double humps_tilt = stack_db(humps, 53) - stack_db(humps, 83);
  const double swing_tilt = stack_db(swing, 53) - stack_db(swing, 83);
  REQUIRE(humps_tilt < 5.0);
  REQUIRE(swing_tilt > 10.0);

  // The saturated bass flow is odd-dominant (the measured accordion's h3 and h5
  // sit over h2 and h4); the hump pair puts the even partials on top.
  REQUIRE(ratio(swing, 53, 3, 2) > 1.0);
  REQUIRE(ratio(swing, 53, 5, 4) > 1.0);
  REQUIRE(ratio(humps, 53, 3, 2) < 1.0);
}

TEST_CASE("the free reed's slot flow leaves no steady flow under the note",
          "[midi][synth][free_reed]") {
  for (uint8_t note : {41, 59, 77}) {
    const double f0 = note_hz(note);
    NativeSynthPatch patch = free_reed_patch();
    patch.free_reed.slot_duty = 0.56f;
    patch.free_reed.slot_return = 0.8f;
    patch.free_reed.slot_gap = 0.41f;
    // Radiating differentiates the flow and would remove the steady part on its
    // own; at zero it is the closed-form mean that has to.
    patch.free_reed.radiation = 0.0f;

    const std::vector<double> ps = power_spectrum(render_patch(patch, note, 100, 24000), 8000);
    const int top = static_cast<int>(std::lround(0.8 * f0 / kRate * kFft));
    double below = 0.0;
    for (int b = 1; b < top && b < static_cast<int>(ps.size()); ++b) {
      below += ps[static_cast<size_t>(b)];
    }
    REQUIRE(below < 0.02 * harmonic_power(ps, f0, 1));
  }
}

TEST_CASE("the free reed's slot flow stays bounded across the keyboard",
          "[midi][synth][free_reed]") {
  for (uint8_t note : {28, 46, 64, 82}) {
    for (float duty : {0.1f, 0.29f, 0.56f, 0.9f}) {
      for (float ret : {0.0f, 0.4f, 1.5f}) {
        for (float radiation : {0.0f, 0.6f, 1.0f}) {
          NativeSynthPatch patch = free_reed_patch();
          patch.free_reed.slot_duty = duty;
          patch.free_reed.slot_return = ret;
          patch.free_reed.radiation = radiation;
          const std::vector<float> tone = render_patch(patch, note, 120, 12000);
          REQUIRE(all_finite(tone));
          // The differentiator's normalisation is what holds the low notes in:
          // it divides by 2*sin(pi*f0/sr), which at 41 Hz is a gain of 186.
          REQUIRE(peak(tone) < 4.0f);
        }
      }
    }
  }
}

TEST_CASE("the free reed radiation blend preserves its fundamental level",
          "[midi][synth][free_reed][boundary]") {
  for (const double sample_rate : {48000.0, 96000.0}) {
    FreeReedPatchParams params;
    params.slot_duty = 0.29f;
    params.slot_return = 0.7f;
    params.detune = 0.0f;
    params.breath_noise = 0.0f;
    std::array<double, 5> mono{};
    const std::array<float, 5> radiations = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    for (size_t i = 0; i < radiations.size(); ++i) {
      params.radiation = radiations[i];
      mono[i] = fundamental_amplitude(params, sample_rate, 69, static_cast<int>(sample_rate),
                                      static_cast<int>(sample_rate));
      REQUIRE(mono[i] > 1.0e-4);
      INFO("sample rate = " << sample_rate << ", radiation = " << radiations[i]);
      CHECK(std::isfinite(mono[i]));
      CHECK(std::abs(20.0 * std::log10(mono[i] / mono.front())) < 0.05);
    }

    // Per-tongue compensation leaves only beating/window projection residual.
    params.detune = 0.3f;
    std::array<double, 5> musette{};
    for (size_t i = 0; i < radiations.size(); ++i) {
      params.radiation = radiations[i];
      musette[i] = fundamental_amplitude(params, sample_rate, 69, static_cast<int>(sample_rate),
                                         static_cast<int>(sample_rate));
      REQUIRE(musette[i] > 1.0e-4);
      INFO("musette sample rate = " << sample_rate << ", radiation = " << radiations[i]);
      CHECK(std::isfinite(musette[i]));
      CHECK(std::abs(20.0 * std::log10(musette[i] / musette.front())) < 0.1);
    }
  }
}

TEST_CASE("the free reed radiation normalization follows live pitch", "[midi][synth][free_reed]") {
  using sonare::midi::Velocity16;
  using sonare::midi::synth::FreeReedPatchParams;
  using sonare::midi::synth::FreeReedVoiceCore;

  FreeReedPatchParams params;
  params.slot_duty = 0.29f;
  params.slot_return = 0.7f;
  params.detune = 0.0f;
  params.breath_noise = 0.0f;
  const auto compare_bend = [&](float radiation, uint8_t note, float target_ratio,
                                uint8_t fresh_note) {
    params.radiation = radiation;

    FreeReedVoiceCore bent;
    bent.start(params, kRate, note, Velocity16::from7(100), 7);
    const double bent_rms = rms_after_pitch_change(bent, 1.0f, target_ratio, 24000, 24000);

    FreeReedVoiceCore fresh;
    fresh.start(params, kRate, fresh_note, Velocity16::from7(100), 7);
    const double fresh_rms = rms_after(fresh, 1.0f, 24000, 24000);
    REQUIRE(fresh_rms > 1.0e-4);
    return bent_rms / fresh_rms;
  };

  // Note 69 bent up an octave has the same sounding pitch as a fresh note 81.
  // A fixed fundamental normalizer makes the differentiated slot flow twice as
  // loud after the bend; the live-pitch normalizer must keep the RMS aligned.
  const double up_ratio = compare_bend(1.0f, 69, 2.0f, 81);
  REQUIRE(up_ratio > 0.95);
  REQUIRE(up_ratio < 1.05);

  // The same law must hold when a high note bends down an octave.
  const double down_ratio = compare_bend(1.0f, 81, 0.5f, 69);
  REQUIRE(down_ratio > 0.95);
  REQUIRE(down_ratio < 1.05);

  // The no-radiation path must stay independent of the differentiator, while a
  // mixed source still needs the live correction for its radiated component.
  const double dry_ratio = compare_bend(0.0f, 69, 2.0f, 81);
  REQUIRE(dry_ratio > 0.99);
  REQUIRE(dry_ratio < 1.01);
  const double mixed_ratio = compare_bend(0.5f, 69, 2.0f, 81);
  REQUIRE(mixed_ratio > 0.95);
  REQUIRE(mixed_ratio < 1.05);

  // After a bend the cached normalizer must follow the pitch back to ratio 1.
  params.radiation = 1.0f;
  FreeReedVoiceCore returning;
  returning.start(params, kRate, 69, Velocity16::from7(100), 7);
  for (int i = 0; i < 24000; ++i) returning.render(1.0f);
  for (int i = 0; i < 24000; ++i) returning.render(2.0f);
  const double returned_rms = rms_after(returning, 1.0f, 0, 24000);

  FreeReedVoiceCore base;
  base.start(params, kRate, 69, Velocity16::from7(100), 7);
  const double base_rms = rms_after(base, 1.0f, 24000, 24000);
  REQUIRE(returned_rms / base_rms > 0.95);
  REQUIRE(returned_rms / base_rms < 1.05);
}

TEST_CASE("the free reed radiation normalization stays bounded above Nyquist",
          "[midi][synth][free_reed]") {
  using sonare::midi::Velocity16;
  using sonare::midi::synth::FreeReedPatchParams;
  using sonare::midi::synth::FreeReedVoiceCore;

  FreeReedPatchParams params;
  params.slot_duty = 0.29f;
  params.slot_return = 0.7f;
  params.detune = 0.0f;
  params.breath_noise = 0.0f;
  params.radiation = 1.0f;

  // At a low host rate the base note can put sin(pi*f/sr) on its negative
  // half-cycle. The ratio-1 start path must use its magnitude as the
  // normalizer, or it falls through the positive-only floor and explodes.
  for (uint8_t note : {120, 127}) {
    FreeReedVoiceCore voice;
    voice.start(params, 8000.0, note, Velocity16::from7(100), 7);
    float peak = 0.0f;
    bool all_finite = true;
    for (int i = 0; i < 8000; ++i) {
      const float sample = voice.render(1.0f);
      all_finite = all_finite && std::isfinite(sample);
      peak = std::max(peak, std::fabs(sample));
    }
    INFO("note " << static_cast<int>(note));
    CHECK(all_finite);
    CHECK(peak > 1.0e-3f);
    CHECK(peak < 4.0f);
  }
}

TEST_CASE("the free reed musette radiation stays bounded at the sample-rate edge",
          "[midi][synth][free_reed]") {
  using sonare::midi::Velocity16;
  using sonare::midi::synth::FreeReedPatchParams;
  using sonare::midi::synth::FreeReedVoiceCore;

  FreeReedPatchParams params;
  params.slot_duty = 0.29f;
  params.slot_return = 0.7f;
  params.detune = 0.3f;
  params.breath_noise = 0.0f;

  // The A tongue reaches exactly the sample rate here. A shared normalizer
  // evaluated at the un-detuned tongue sees sin(pi) and can amplify the first
  // difference by its numerical floor. Both the full radiated and mixed paths
  // must remain finite and bounded after an established note.
  const float edge_ratio = 48000.0f / 440.0f;
  for (float radiation : {1.0f, 0.5f}) {
    params.radiation = radiation;
    FreeReedVoiceCore voice;
    voice.start(params, 48000.0, 69, Velocity16::from7(100), 7);
    for (int i = 0; i < 24000; ++i) voice.render(1.0f);

    float peak_value = 0.0f;
    bool finite = true;
    for (int i = 0; i < 4096; ++i) {
      const float sample = voice.render(edge_ratio);
      finite = finite && std::isfinite(sample);
      peak_value = std::max(peak_value, std::fabs(sample));
    }
    INFO("radiation " << radiation);
    CHECK(finite);
    CHECK(peak_value < 4.0f);
  }
}

TEST_CASE("the free reed primes low-frequency mono radiation before the attack",
          "[midi][synth][free_reed]") {
  using sonare::midi::Velocity16;
  using sonare::midi::synth::FreeReedPatchParams;
  using sonare::midi::synth::FreeReedVoiceCore;

  FreeReedPatchParams params;
  params.attack_ms = 1.0f;
  params.slot_duty = 0.29f;
  params.slot_return = 0.7f;
  params.detune = 0.0f;
  params.radiation = 1.0f;
  params.breath_noise = 0.0f;

  FreeReedVoiceCore voice;
  voice.start(params, 8000.0, 0, Velocity16::from7(100), 7);

  float peak_value = 0.0f;
  bool finite = true;
  for (int i = 0; i < 512; ++i) {
    const float sample = voice.render(0.01f);
    finite = finite && std::isfinite(sample);
    peak_value = std::max(peak_value, std::fabs(sample));
  }
  CHECK(finite);
  CHECK(peak_value < 4.0f);
}

TEST_CASE("the free reed primes a live sub-audio bend before its first sample",
          "[midi][synth][free_reed]") {
  using sonare::midi::Velocity16;
  using sonare::midi::synth::FreeReedPatchParams;
  using sonare::midi::synth::FreeReedVoiceCore;

  FreeReedPatchParams params;
  params.attack_ms = 0.0f;
  params.slot_duty = 0.29f;
  params.slot_return = 0.7f;
  params.detune = 0.0f;
  params.radiation = 1.0f;
  params.breath_noise = 0.0f;

  FreeReedVoiceCore voice;
  voice.start(params, 48000.0, 69, Velocity16::from7(100), 7);

  float peak_value = 0.0f;
  bool finite = true;
  for (int i = 0; i < 128; ++i) {
    const float sample = voice.render(0.01f);
    finite = finite && std::isfinite(sample);
    peak_value = std::max(peak_value, std::fabs(sample));
  }
  CHECK(finite);
  CHECK(peak_value < 4.0f);
}

TEST_CASE("the free reed musette bend follows fresh controls", "[midi][synth][free_reed]") {
  using sonare::midi::Velocity16;
  using sonare::midi::synth::FreeReedPatchParams;
  using sonare::midi::synth::FreeReedVoiceCore;

  FreeReedPatchParams params;
  params.slot_duty = 0.29f;
  params.slot_return = 0.7f;
  params.detune = 0.3f;
  params.radiation = 0.5f;
  params.breath_noise = 0.0f;

  constexpr int kComparisonSamples = 480000;
  const auto compare_bend = [&](uint8_t note, float ratio, uint8_t fresh_note) {
    FreeReedVoiceCore bent;
    bent.start(params, 48000.0, note, Velocity16::from7(100), 7);
    const double bent_rms = rms_after_pitch_change(bent, 1.0f, ratio, 24000, kComparisonSamples);

    FreeReedVoiceCore fresh;
    fresh.start(params, 48000.0, fresh_note, Velocity16::from7(100), 7);
    const double fresh_rms = rms_after(fresh, 1.0f, 24000, kComparisonSamples);
    REQUIRE(fresh_rms > 1.0e-4);
    return bent_rms / fresh_rms;
  };

  const double up_ratio = compare_bend(69, 2.0f, 81);
  INFO("upward bend RMS ratio " << up_ratio);
  CHECK(up_ratio > 0.95);
  CHECK(up_ratio < 1.05);

  const double down_ratio = compare_bend(81, 0.5f, 69);
  INFO("downward bend RMS ratio " << down_ratio);
  CHECK(down_ratio > 0.95);
  CHECK(down_ratio < 1.05);
}

TEST_CASE("free reed onset overshoot is off by default (bit-identical)",
          "[midi][synth][free_reed]") {
  // At 0 dB the settle and the velocity slope have nothing to shape, so neither
  // reaches the render.
  NativeSynthPatch base = gm_fallback_patch(0, 21);  // Accordion
  base.free_reed.onset_breath_overshoot_db = 0.0f;
  NativeSynthPatch same = base;
  same.free_reed.onset_breath_settle_ms = 120.0f;
  same.free_reed.onset_breath_overshoot_vel_db = 4.0f;
  same.free_reed.onset_breath_overshoot_key_db = -6.0f;
  REQUIRE(render_patch(base, 65, 100, 24000) == render_patch(same, 65, 100, 24000));
}

TEST_CASE("a free reed onset overshoot lifts the onset, more at ff when it slopes with velocity",
          "[midi][synth][free_reed]") {
  NativeSynthPatch off = gm_fallback_patch(0, 21);  // Accordion
  off.free_reed.onset_breath_overshoot_db = 0.0f;
  // The patch's slow VCA attack would hide the onset the contour shapes.
  off.amp_env.attack_ms = 5.0f;
  NativeSynthPatch on = off;
  on.free_reed.onset_breath_overshoot_db = 6.0f;
  on.free_reed.onset_breath_settle_ms = 40.0f;
  on.free_reed.onset_breath_overshoot_vel_db = 3.0f;
  const int n = static_cast<int>(0.45 * kRate);
  const double soft_gain =
      onset_lift_db(render_patch(on, 65, 32, n)) - onset_lift_db(render_patch(off, 65, 32, n));
  const double hard_gain =
      onset_lift_db(render_patch(on, 65, 127, n)) - onset_lift_db(render_patch(off, 65, 127, n));
  INFO("onset lift added at velocity 32: " << soft_gain << " dB, at 127: " << hard_gain << " dB");
  REQUIRE(hard_gain > 0.5);
  REQUIRE(hard_gain > soft_gain);
}

TEST_CASE("a key-tracked free reed onset overshoot falls toward the treble",
          "[midi][synth][free_reed]") {
  NativeSynthPatch flat = gm_fallback_patch(0, 21);  // Accordion
  flat.amp_env.attack_ms = 5.0f;
  flat.amp_env.sustain = 1.0f;
  flat.free_reed.attack_ms = 2.0f;
  flat.free_reed.onset_breath_overshoot_db = 6.0f;
  flat.free_reed.onset_breath_settle_ms = 80.0f;
  flat.free_reed.onset_breath_overshoot_key_db = 0.0f;
  NativeSynthPatch tracked = flat;
  tracked.free_reed.onset_breath_overshoot_key_db = -6.0f;
  const int n = static_cast<int>(0.45 * kRate);
  // Two octaves under A4 the tracked overshoot is 6 + 2 * 6 = 18 dB; two over it is floored at 0.
  const double bass = onset_lift_db(render_patch(tracked, 45, 100, n)) -
                      onset_lift_db(render_patch(flat, 45, 100, n));
  const double treble = onset_lift_db(render_patch(tracked, 93, 100, n)) -
                        onset_lift_db(render_patch(flat, 93, 100, n));
  INFO("tracked minus flat onset lift: bass " << bass << " dB, treble " << treble << " dB");
  REQUIRE(bass > 2.0);
  REQUIRE(treble < -2.0);
}

TEST_CASE(
    "the free reed onset overshoot moves per octave from A4, floored at 0 and unbounded above",
    "[midi][synth][free_reed]") {
  using sonare::midi::synth::free_reed_onset_overshoot_db;
  using sonare::midi::synth::FreeReedPatchParams;
  const auto hz = [](int note) {
    return static_cast<float>(440.0 * std::pow(2.0, (note - 69) / 12.0));
  };
  FreeReedPatchParams p;
  p.onset_breath_overshoot_db = 5.5f;
  p.onset_breath_overshoot_key_db = -3.0f;
  // C2 sits 2.75 octaves under A4: 5.5 + 3 * 2.75, past the 8 dB the A4 value is held to.
  CHECK(free_reed_onset_overshoot_db(p, hz(36)) == Catch::Approx(13.75f).margin(1.0e-3));
  CHECK(free_reed_onset_overshoot_db(p, hz(69)) == Catch::Approx(5.5f).margin(1.0e-5));
  // Two octaves over A4 the slope would go negative; the floor holds it at 0.
  CHECK(free_reed_onset_overshoot_db(p, hz(93)) == 0.0f);
  // A zero A4 value is inert whatever the slope, which is what the tuning gate reads.
  p.onset_breath_overshoot_db = 0.0f;
  for (int note : {21, 36, 69, 108}) CHECK(free_reed_onset_overshoot_db(p, hz(note)) == 0.0f);
  // The shipped accordion receives the same law the voice applies.
  const NativeSynthPatch accordion = gm_fallback_patch(0, 21);
  CHECK(free_reed_onset_overshoot_db(accordion.free_reed, hz(36)) ==
        Catch::Approx(accordion.free_reed.onset_breath_overshoot_db -
                      2.75f * accordion.free_reed.onset_breath_overshoot_key_db)
            .margin(1.0e-3));
}

TEST_CASE("a streamed bellows holds the free reed onset overshoot", "[midi][synth][free_reed]") {
  // CC2 seeds the force axis before the note, so the player's bellows is the
  // onset and the synthesized one must not play over it.
  NativeSynthPatch off = gm_fallback_patch(0, 21);  // Accordion
  off.free_reed.onset_breath_overshoot_db = 0.0f;
  NativeSynthPatch on = off;
  on.free_reed.onset_breath_overshoot_db = 6.0f;
  REQUIRE(render_with_initial_cc(on, 65, 100, 24000, 2, 90) ==
          render_with_initial_cc(off, 65, 100, 24000, 2, 90));
  REQUIRE(render_patch(on, 65, 100, 24000) != render_patch(off, 65, 100, 24000));
}
