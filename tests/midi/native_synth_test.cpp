/// @file native_synth_test.cpp
/// @brief NativeSynth VA engine (midi/synth/native_synth, oscillator,
///        gm_fallback_map): PolyBLEP antialiasing regression, deterministic
///        rendering, MidiInstrument channel semantics (sustain / all-sound-off
///        / volume), the Sf2Player synth fallback (every GM program and drum
///        note audible without a SoundFont, one-shot drums, fallback-vs-SF2
///        coexistence) and the allocation-free audio path.

#include "midi/synth/native_synth.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/fft.h"
#include "midi/midi_event.h"
#include "midi/synth/gm_fallback_data.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/oscillator.h"
#include "midi/synth/patch_tuning.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/alloc_guard.h"
#include "support/audio_fixtures.h"
#include "support/sf2_builder.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::MidiInstrumentSourceOutput;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::midi::synth::SynthEngineMode;
using sonare::midi::synth::VaOscillator;
using sonare::midi::synth::VaWaveform;
using sonare::test::AllocationGuard;
using sonare::test::Sf2Builder;

constexpr double kOutRate = 48000.0;

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

struct StereoRender {
  std::vector<float> left;
  std::vector<float> right;
};

template <typename Instrument>
StereoRender render(Instrument& instrument, int num_samples) {
  StereoRender out;
  out.left.assign(static_cast<size_t>(num_samples), 0.0f);
  out.right.assign(static_cast<size_t>(num_samples), 0.0f);
  float* chans[2] = {out.left.data(), out.right.data()};
  instrument.process(chans, 2, num_samples);
  return out;
}

float peak(const std::vector<float>& buf, size_t from = 0) {
  float p = 0.0f;
  for (size_t i = from; i < buf.size(); ++i) p = std::max(p, std::fabs(buf[i]));
  return p;
}

float rms(const std::vector<float>& buf, size_t from = 0) {
  if (from >= buf.size()) return 0.0f;
  double sum = 0.0;
  for (size_t i = from; i < buf.size(); ++i) {
    sum += static_cast<double>(buf[i]) * static_cast<double>(buf[i]);
  }
  return static_cast<float>(std::sqrt(sum / static_cast<double>(buf.size() - from)));
}

double estimate_frequency(const std::vector<float>& buf, double sample_rate, size_t from = 0) {
  std::vector<double> crossings;
  for (size_t i = std::max<size_t>(from, 1); i < buf.size(); ++i) {
    const float prev = buf[i - 1];
    const float cur = buf[i];
    if (prev < 0.0f && cur >= 0.0f) {
      const double denom = static_cast<double>(cur) - static_cast<double>(prev);
      const double frac = denom != 0.0 ? -static_cast<double>(prev) / denom : 0.0;
      crossings.push_back(static_cast<double>(i - 1) + frac);
    }
  }
  if (crossings.size() < 2) return 0.0;
  return sample_rate * static_cast<double>(crossings.size() - 1) /
         (crossings.back() - crossings.front());
}

double dominant_frequency(const std::vector<float>& signal, double sample_rate, size_t from = 0) {
  const size_t n = signal.size() - std::min(from, signal.size());
  if (n < 2) return 0.0;
  std::vector<float> windowed(n);
  for (size_t i = 0; i < n; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * static_cast<double>(i) /
                                          static_cast<double>(n - 1));
    windowed[i] = signal[from + i] * static_cast<float>(w);
  }
  sonare::FFT fft(static_cast<int>(n));
  std::vector<std::complex<float>> spectrum(static_cast<size_t>(fft.n_bins()));
  fft.forward(windowed.data(), spectrum.data());
  int best_bin = 1;
  double best_power = 0.0;
  for (int b = 1; b < fft.n_bins(); ++b) {
    const double hz = static_cast<double>(b) * sample_rate / static_cast<double>(n);
    if (hz < 20.0) continue;
    const double power = static_cast<double>(std::norm(spectrum[static_cast<size_t>(b)]));
    if (power > best_power) {
      best_power = power;
      best_bin = b;
    }
  }
  return static_cast<double>(best_bin) * sample_rate / static_cast<double>(n);
}

/// Ratio of non-harmonic ("alias") spectral power to harmonic power for a
/// periodic signal at @p f0. Harmonics get a +-3 bin window; the lowest bins
/// (DC / window leakage) are skipped.
double alias_ratio(const std::vector<float>& signal, double f0) {
  const int n = static_cast<int>(signal.size());
  std::vector<float> windowed(signal.size());
  for (int i = 0; i < n; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * i / (n - 1));
    windowed[static_cast<size_t>(i)] = signal[static_cast<size_t>(i)] * static_cast<float>(w);
  }
  sonare::FFT fft(n);
  std::vector<std::complex<float>> spectrum(static_cast<size_t>(fft.n_bins()));
  fft.forward(windowed.data(), spectrum.data());

  std::set<int> harmonic_bins;
  for (int k = 1; k * f0 < 0.5 * kOutRate; ++k) {
    const int centre = static_cast<int>(std::lround(k * f0 / kOutRate * n));
    for (int b = centre - 3; b <= centre + 3; ++b) harmonic_bins.insert(b);
  }

  double harmonic = 0.0;
  double alias = 0.0;
  for (int b = 8; b < fft.n_bins(); ++b) {
    const double p = static_cast<double>(std::norm(spectrum[static_cast<size_t>(b)]));
    if (harmonic_bins.count(b) > 0) {
      harmonic += p;
    } else {
      alias += p;
    }
  }
  return harmonic > 0.0 ? alias / harmonic : 1.0;
}

/// Sf2Player with no SoundFont: every note resolves through the GM fallback.
Sf2Player make_fallback_player() {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  Sf2Player player(cfg);
  player.prepare(kOutRate, 256);
  return player;
}

/// Single-preset SoundFont (program 0 only) for coexistence tests.
std::shared_ptr<Sf2File> make_single_preset_fixture() {
  Sf2Builder b;
  std::vector<float> sine(96);
  for (size_t i = 0; i < sine.size(); ++i) {
    sine[i] =
        0.9f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * static_cast<double>(i) / 32.0));
  }
  const int sine_id = b.add_sample("sine1k", sine, 32000, 60, 32, 96);
  Sf2Builder::ZoneSpec looped;
  looped.gens.push_back({54 /*sampleModes*/, 1});
  looped.target = sine_id;
  const int melodic = b.add_instrument("melodic", {looped});
  Sf2Builder::ZoneSpec pz;
  pz.target = melodic;
  b.add_preset("Sine", 0, 0, {pz});
  auto sf2 = std::make_shared<Sf2File>();
  const std::vector<uint8_t> bytes = b.build();
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), nullptr));
  return sf2;
}

}  // namespace

TEST_CASE("PolyBLEP saw suppresses aliasing versus the naive saw", "[midi][synth]") {
  constexpr int kN = 8192;
  // A high lead note (~3.1 kHz) where a trivially sampled saw aliases badly.
  const double f0 = 3133.7;

  VaOscillator osc;
  osc.start(kOutRate, VaWaveform::kSaw, 0.0f, 0);
  osc.set_frequency(static_cast<float>(f0));
  std::vector<float> blep(kN);
  for (float& s : blep) s = osc.next();

  std::vector<float> naive(kN);
  double phase = 0.0;
  for (float& s : naive) {
    s = static_cast<float>(2.0 * phase - 1.0);
    phase += f0 / kOutRate;
    if (phase >= 1.0) phase -= 1.0;
  }

  const double blep_ratio = alias_ratio(blep, f0);
  const double naive_ratio = alias_ratio(naive, f0);
  // The naive saw folds audible alias energy; PolyBLEP must sit at least an
  // order of magnitude lower and below -25 dB overall.
  REQUIRE(naive_ratio > 0.01);
  REQUIRE(blep_ratio < 0.1 * naive_ratio);
  REQUIRE(blep_ratio < 0.003);
}

TEST_CASE("PolyBLEP square and triangle stay below the alias threshold", "[midi][synth]") {
  constexpr int kN = 8192;
  const double f0 = 2477.3;
  for (const VaWaveform wf : {VaWaveform::kSquare, VaWaveform::kTriangle}) {
    VaOscillator osc;
    osc.start(kOutRate, wf, 0.0f, 0);
    osc.set_frequency(static_cast<float>(f0));
    std::vector<float> buf(kN);
    for (float& s : buf) s = osc.next();
    REQUIRE(alias_ratio(buf, f0) < 0.003);
  }
}

TEST_CASE("NativeSynth renders deterministically", "[midi][synth]") {
  NativeSynthConfig cfg;
  cfg.patch.unison = 5;
  cfg.patch.detune_cents = 12.0f;
  cfg.patch.drift_cents = 4.0f;
  cfg.patch.env_to_cutoff_cents = 1800.0f;
  cfg.patch.cutoff_hz = 2500.0f;

  auto run = [&cfg]() {
    NativeSynth synth(cfg);
    synth.prepare(kOutRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 67, 90)));
    StereoRender a = render(synth, 1024);
    synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
    StereoRender b = render(synth, 1024);
    a.left.insert(a.left.end(), b.left.begin(), b.left.end());
    a.right.insert(a.right.end(), b.right.begin(), b.right.end());
    return a;
  };

  const StereoRender first = run();
  const StereoRender second = run();
  REQUIRE(peak(first.left) > 0.01f);
  REQUIRE(first.left == second.left);
  REQUIRE(first.right == second.right);
}

TEST_CASE("NativeSynth GM mode follows program changes and routes channel 10 to drums",
          "[midi][synth]") {
  NativeSynthConfig fixed_config;
  NativeSynth fixed(fixed_config);
  fixed.prepare(kOutRate, 256);
  fixed.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 4)));  // e-piano
  fixed.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  const StereoRender fixed_render = render(fixed, 2048);

  NativeSynthConfig gm_config;
  gm_config.use_gm_programs = true;
  NativeSynth gm(gm_config);
  gm.prepare(kOutRate, 256);
  gm.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 4)));  // e-piano
  gm.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  const StereoRender melodic = render(gm, 2048);

  REQUIRE(peak(melodic.left) > 0.001f);
  REQUIRE(melodic.left != fixed_render.left);

  NativeSynth drums(gm_config);
  drums.prepare(kOutRate, 256);
  drums.on_event(0, event(sonare::midi::make_midi1_program_change(0, 9, 8)));  // room kit
  drums.on_event(0, event(sonare::midi::make_midi1_note_on(0, 9, 36, 110)));   // kick
  const StereoRender drum_render = render(drums, 2048);
  REQUIRE(peak(drum_render.left) > 0.001f);
  REQUIRE(drum_render.left != melodic.left);
}

TEST_CASE("NativeSynth GM mode sounds every melodic program", "[midi][synth]") {
  // prepare() sizes the per-voice delay slabs. In GM mode note_on() resolves the
  // engine from the program, so any engine can be selected regardless of the
  // configured patch: a slab left unallocated silences whole GM families (the
  // waveguide cores render 0 while their span is unattached).
  NativeSynthConfig cfg;
  cfg.use_gm_programs = true;
  cfg.gain = 1.0f;
  cfg.polyphony = 4;
  NativeSynth synth(cfg);
  synth.prepare(kOutRate, 256);
  for (int program = 0; program < 128; ++program) {
    synth.on_event(
        0, event(sonare::midi::make_midi1_program_change(0, 0, static_cast<uint8_t>(program))));
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 110)));
    const StereoRender out = render(synth, 2048);
    INFO("GM program " << program);
    REQUIRE(peak(out.left) + peak(out.right) > 1.0e-4f);
    REQUIRE(rms(out.left) + rms(out.right) > 1.0e-5f);
    // Silence the part so the next program starts from a clean pool.
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 120, 0)));
  }
}

TEST_CASE("NativeSynth GM mode tail covers the slowest fallback release", "[midi][synth]") {
  // Any program can sound in GM mode, so the reported tail has to bound the
  // fallback tables rather than the configured patch's own release.
  NativeSynthConfig cfg;
  cfg.use_gm_programs = true;
  cfg.gain = 1.0f;
  NativeSynth synth(cfg);
  synth.prepare(kOutRate, 256);
  const int expected = static_cast<int>(sonare::midi::synth::DahdsrEnvelope::release_tail_samples(
      kOutRate, sonare::midi::synth::gm_fallback_max_release_ms()));
  REQUIRE(synth.tail_samples() >= expected);

  // Behaviourally: the pad (GM 88) carries the slowest fallback release, and it
  // has to fade out inside the reported tail.
  synth.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 88)));
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 110)));
  render(synth, 4096);
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
  const StereoRender out = render(synth, synth.tail_samples() + 4096);
  REQUIRE(peak(out.left, out.left.size() - 256) < 1.0e-3f);
}

TEST_CASE("NativeSynth channel semantics: sustain, volume, all sound off", "[midi][synth]") {
  NativeSynth synth(NativeSynthConfig{});
  synth.prepare(kOutRate, 256);

  // CC64 sustain holds the note across note-off.
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, 127)));
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  render(synth, 512);
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
  StereoRender held = render(synth, 2048);
  REQUIRE(peak(held.left, 1024) > 0.001f);

  // Releasing the pedal releases the held note.
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, 0)));
  const int tail = synth.tail_samples();
  REQUIRE(tail > 0);
  StereoRender released = render(synth, tail + 4096);
  REQUIRE(peak(released.left, released.left.size() - 256) < 0.001f);

  // CC7 volume scales the output level.
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  const float full = peak(render(synth, 1024).left);
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 7, 50)));
  const float quiet = peak(render(synth, 1024).left);
  REQUIRE(quiet < 0.5f * full);

  // CC120 all sound off silences immediately.
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 120, 0)));
  REQUIRE(synth.active_voice_count() == 0);
  StereoRender silent = render(synth, 512);
  REQUIRE(peak(silent.left) == 0.0f);
}

TEST_CASE("NativeSynth pitch bend follows RPN0 bend range", "[midi][synth]") {
  NativeSynthConfig cfg;
  cfg.patch.waveform = VaWaveform::kSine;
  cfg.patch.cutoff_hz = 20000.0f;
  cfg.patch.gain = 0.8f;

  auto bent_frequency = [&](bool wide, bool reset_all_controllers = false) {
    NativeSynth synth(cfg);
    synth.prepare(kOutRate, 256);
    if (wide) {
      synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 101, 0)));
      synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 100, 0)));
      synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 6, 12)));
    }
    if (reset_all_controllers) {
      synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 121, 0)));
    }
    synth.on_event(0, event(sonare::midi::make_midi1_pitch_bend(0, 0, 12288)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 127)));
    const StereoRender out = render(synth, 8192);
    return estimate_frequency(out.left, kOutRate, 1024);
  };

  const double normal = bent_frequency(false);
  const double wide = bent_frequency(true);
  const double wide_after_rac = bent_frequency(true, true);
  REQUIRE(normal > 460.0);
  REQUIRE(normal < 470.0);
  REQUIRE(wide > 610.0);
  REQUIRE(wide < 630.0);
  REQUIRE(wide_after_rac > 610.0);
  REQUIRE(wide_after_rac < 630.0);
}

TEST_CASE("NativeSynth applies pitch offset outside the subtractive engine", "[midi][synth]") {
  auto peak_hz = [](float offset_cents) {
    NativeSynthConfig cfg;
    cfg.patch.mode = SynthEngineMode::kAdditive;
    cfg.patch.pitch_offset_cents = offset_cents;
    cfg.patch.gain = 0.8f;
    NativeSynth synth(cfg);
    synth.prepare(kOutRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 127)));
    const StereoRender out = render(synth, 8192);
    return dominant_frequency(out.left, kOutRate, 1024);
  };

  const double base = peak_hz(0.0f);
  const double shifted = peak_hz(1200.0f);
  REQUIRE(base > 0.0);
  REQUIRE(shifted > base * 1.5);
}

TEST_CASE("Sf2Player without a SoundFont plays every GM program via the fallback",
          "[midi][sf2][synth]") {
  Sf2Player player = make_fallback_player();
  for (int program = 0; program < 128; ++program) {
    player.on_event(
        0, event(sonare::midi::make_midi1_program_change(0, 0, static_cast<uint8_t>(program))));
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 110)));
    const StereoRender out = render(player, 2048);
    INFO("program " << program);
    REQUIRE(peak(out.left) + peak(out.right) > 1.0e-4f);
    // Silence the part so the next program starts from a clean pool.
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 120, 0)));
  }
}

TEST_CASE("physical-model GM programs route to their waveguide engines", "[midi][synth]") {
  using sonare::midi::synth::gm_fallback_patch;
  // Harpsichord (GM 6) voices its own jack-and-plectrum engine; the clavinet (7)
  // stays FM (struck string + pickup, no dedicated model yet).
  REQUIRE(gm_fallback_patch(0, 6).mode == SynthEngineMode::kHarpsichord);  // Harpsichord
  REQUIRE(gm_fallback_patch(0, 7).mode == SynthEngineMode::kFm);           // Clavi
  // Bowed string family (GM 40-43) now voices the friction waveguide.
  REQUIRE(gm_fallback_patch(0, 40).mode == SynthEngineMode::kBowedString);  // Violin
  REQUIRE(gm_fallback_patch(0, 43).mode == SynthEngineMode::kBowedString);  // Contrabass
  // Brass family (GM 56-61); SynthBrass (62-63) stays FM.
  REQUIRE(gm_fallback_patch(0, 56).mode == SynthEngineMode::kBrass);  // Trumpet
  REQUIRE(gm_fallback_patch(0, 60).mode == SynthEngineMode::kBrass);  // French Horn
  REQUIRE(gm_fallback_patch(0, 61).mode == SynthEngineMode::kBrass);  // Brass Section
  REQUIRE(gm_fallback_patch(0, 62).mode == SynthEngineMode::kFm);     // Synth Brass 1
  // String Ensemble 1/2 (GM 48-49) are the bowed waveguide in section; the two
  // Synth Strings above them stay on the family's supersaw.
  REQUIRE(gm_fallback_patch(0, 48).mode == SynthEngineMode::kBowedString);
  REQUIRE(gm_fallback_patch(0, 49).mode == SynthEngineMode::kBowedString);
  REQUIRE(gm_fallback_patch(0, 50).mode == SynthEngineMode::kSubtractive);
  // Reed family (GM 64-71); the clarinet is the only cylinder, the saxes cones.
  REQUIRE(gm_fallback_patch(0, 64).mode == SynthEngineMode::kReed);  // Soprano Sax
  REQUIRE(gm_fallback_patch(0, 71).mode == SynthEngineMode::kReed);  // Clarinet
  REQUIRE_FALSE(gm_fallback_patch(0, 71).reed.conical);              // clarinet = cylinder
  REQUIRE(gm_fallback_patch(0, 64).reed.conical);                    // soprano sax = cone
  // Air-jet flute family (GM 72-79).
  REQUIRE(gm_fallback_patch(0, 72).mode == SynthEngineMode::kFlute);  // Piccolo
  REQUIRE(gm_fallback_patch(0, 79).mode == SynthEngineMode::kFlute);  // Ocarina
  // Free-reed family (GM 20-23): reed organ / accordion / harmonica / bandoneon.
  REQUIRE(gm_fallback_patch(0, 20).mode == SynthEngineMode::kFreeReed);  // Reed Organ
  REQUIRE(gm_fallback_patch(0, 21).mode == SynthEngineMode::kFreeReed);  // Accordion
  REQUIRE(gm_fallback_patch(0, 22).mode == SynthEngineMode::kFreeReed);  // Harmonica
  REQUIRE(gm_fallback_patch(0, 23).mode == SynthEngineMode::kFreeReed);  // Bandoneon
  // Vocal family (GM 52-54): choir / voice as a glottal-source formant voice.
  REQUIRE(gm_fallback_patch(0, 52).mode == SynthEngineMode::kVocal);  // Choir Aahs
  REQUIRE(gm_fallback_patch(0, 53).mode == SynthEngineMode::kVocal);  // Voice Oohs
  REQUIRE(gm_fallback_patch(0, 54).mode == SynthEngineMode::kVocal);  // Synth Voice
  // Buzzing-bridge plucked family (GM 104/106/107): sitar / shamisen / koto.
  REQUIRE(gm_fallback_patch(0, 104).mode == SynthEngineMode::kPluckedString);  // Sitar
  REQUIRE(gm_fallback_patch(0, 106).mode == SynthEngineMode::kPluckedString);  // Shamisen
  REQUIRE(gm_fallback_patch(0, 107).mode == SynthEngineMode::kPluckedString);  // Koto
  // The rest of the ethnic family is plucked only by GM's filing. A kalimba is a
  // bar, a fiddle is bowed and the two double reeds are blown, so none of them
  // belongs on the family's Karplus-Strong string; 105 Banjo does and stays.
  REQUIRE(gm_fallback_patch(0, 105).mode == SynthEngineMode::kKarplusStrong);  // Banjo
  REQUIRE(gm_fallback_patch(0, 108).mode == SynthEngineMode::kModal);          // Kalimba
  REQUIRE(gm_fallback_patch(0, 109).mode == SynthEngineMode::kReed);           // Bag pipe
  REQUIRE(gm_fallback_patch(0, 109).reed.vel_to_breath == 0.0f);  // the bag, not the player
  REQUIRE(gm_fallback_patch(0, 110).mode == SynthEngineMode::kBowedString);  // Fiddle
  REQUIRE(gm_fallback_patch(0, 111).mode == SynthEngineMode::kReed);         // Shanai
  // Neighbours that intentionally stay on the signal-model family sketch.
  REQUIRE(gm_fallback_patch(0, 80).mode == SynthEngineMode::kSubtractive);  // Square Lead
}

TEST_CASE("model-first program set matches the GM fallback routing", "[midi][synth]") {
  using sonare::midi::synth::gm_fallback_patch;
  using sonare::midi::synth::gm_program_has_dedicated_model;
  using sonare::midi::synth::is_dedicated_model_engine;
  // Golden set: the GM programs whose data-free fallback is a dedicated model
  // (physical waveguide / modal / percussion / free reed) rather than a signal
  // sketch or the formant vocal voice — i.e. the families where the model is
  // preferred over an SF2 sample. A change here means a program's fallback
  // engine changed: reconcile the routing and this expectation together.
  static constexpr int kModelFirst[] = {
      0,   1,   2,   3,   6,                   // piano + harpsichord
      8,   9,   10,  11,  12,  13,  14,  15,   // chromatic percussion (modal / KS)
      19,  20,  21,  22,  23,                  // church organ + free reeds
      24,  25,  26,  27,  28,  29,  30,  31,   // guitars (KS)
      32,  33,  34,  35,  36,  37,             // acoustic / electric basses (KS)
      40,  41,  42,  43,  45,  46,  47,        // bowed strings + pizz / harp / timpani
      48,  49,                                 // string ensembles (bowed, in section)
      56,  57,  58,  59,  60,  61,             // brass (lip reed), section included
      64,  65,  66,  67,  68,  69,  70,  71,   // reeds (sax / oboe / clarinet ...)
      72,  73,  74,  75,  76,  77,  78,  79,   // air-jet flutes
      104, 105, 106, 107, 108, 109, 110, 111,  // ethnic plucked / bowed / reed
      112, 113, 114, 115, 116, 117, 118, 119,  // pitched / synth percussion
  };
  bool expected[128] = {};
  for (int program : kModelFirst) expected[program] = true;

  for (int program = 0; program < 128; ++program) {
    const auto p = static_cast<uint8_t>(program);
    INFO("GM program " << program);
    const bool has_model = gm_program_has_dedicated_model(0, p);
    // The predicate must match the golden model-first membership, and it must
    // agree with the engine the fallback actually resolves the program to.
    REQUIRE(has_model == expected[program]);
    REQUIRE(has_model == is_dedicated_model_engine(gm_fallback_patch(0, p).mode));
  }
}

TEST_CASE("harpsichord GS/GM2 banks select registration variations", "[midi][synth]") {
  using sonare::midi::synth::gm_fallback_patch;
  // Bank 0 (capital tone): a single 8' choir, no second unison, no 4' and no
  // mechanism noise at note-off.
  const auto& base = gm_fallback_patch(0, 6);
  REQUIRE(base.mode == SynthEngineMode::kHarpsichord);
  REQUIRE(base.harpsichord.eight_a);
  REQUIRE_FALSE(base.harpsichord.eight_b);
  REQUIRE_FALSE(base.harpsichord.four);
  REQUIRE(base.harpsichord.jack_noise == 0.0f);
  // Bank 1 — octave mix: the 4' choir is drawn, nothing else changes.
  const auto& octave = gm_fallback_patch(1, 6);
  REQUIRE(octave.harpsichord.four);
  REQUIRE_FALSE(octave.harpsichord.eight_b);
  REQUIRE(octave.harpsichord.jack_noise == 0.0f);
  // Bank 2 — wide: the second 8' choir is drawn and spread, no 4'.
  const auto& wide = gm_fallback_patch(2, 6);
  REQUIRE(wide.harpsichord.eight_b);
  REQUIRE(wide.stereo_spread > 0.0f);
  REQUIRE_FALSE(wide.harpsichord.four);
  // Bank 3 — with key off: the jack and damper sound, no extra choir.
  const auto& keyoff = gm_fallback_patch(3, 6);
  REQUIRE(keyoff.harpsichord.jack_noise > 0.0f);
  REQUIRE_FALSE(keyoff.harpsichord.four);
  REQUIRE_FALSE(keyoff.harpsichord.eight_b);
  // Unknown variation banks fall back to the bank-0 capital tone.
  REQUIRE_FALSE(gm_fallback_patch(9, 6).harpsichord.four);
  REQUIRE(gm_fallback_patch(9, 6).harpsichord.jack_noise == 0.0f);
}

TEST_CASE("Sf2Player without a SoundFont plays the GM drum map via the fallback",
          "[midi][sf2][synth]") {
  Sf2Player player = make_fallback_player();
  for (int note = 35; note <= 59; ++note) {
    player.on_event(0,
                    event(sonare::midi::make_midi1_note_on(0, 9, static_cast<uint8_t>(note), 110)));
    const StereoRender out = render(player, 2048);
    INFO("drum note " << note);
    REQUIRE(peak(out.left) + peak(out.right) > 1.0e-4f);
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 9, 120, 0)));
  }
}

TEST_CASE("Sf2Player fallback one-shot drums ring through note-off", "[midi][sf2][synth]") {
  Sf2Player player = make_fallback_player();
  // Crash cymbal: long decay, note-off immediately after the hit.
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 9, 49, 120)));
  player.on_event(0, event(sonare::midi::make_midi1_note_off(0, 9, 49, 0)));
  const StereoRender out = render(player, 9600);  // 200 ms
  REQUIRE(peak(out.left, 4800) > 1.0e-3f);
}

TEST_CASE("Sf2Player fallback renders deterministically", "[midi][sf2][synth]") {
  auto run = []() {
    Sf2Player player = make_fallback_player();
    player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 88)));  // 7-osc pad
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 57, 96)));
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 9, 38, 110)));  // snare (noise)
    StereoRender a = render(player, 2048);
    player.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 57, 0)));
    StereoRender b = render(player, 2048);
    a.left.insert(a.left.end(), b.left.begin(), b.left.end());
    a.right.insert(a.right.end(), b.right.begin(), b.right.end());
    return a;
  };
  const StereoRender first = run();
  const StereoRender second = run();
  REQUIRE(peak(first.left) > 0.001f);
  REQUIRE(first.left == second.left);
  REQUIRE(first.right == second.right);
}

TEST_CASE("Sf2Player prefers SF2 presets and falls back only when uncovered",
          "[midi][sf2][synth]") {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  SECTION("uncovered program plays via the fallback") {
    Sf2Player player(cfg);
    player.set_soundfont(make_single_preset_fixture());
    player.prepare(kOutRate, 256);
    // Program 0 is covered (no GS fallback to bank 0 program 0 kicks in for
    // program 9): pick an uncovered program.
    player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 9)));
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 110)));
    REQUIRE(peak(render(player, 2048).left) > 1.0e-4f);
  }
  SECTION("synth_fallback=false keeps uncovered programs silent") {
    cfg.synth_fallback = false;
    Sf2Player player(cfg);
    player.set_soundfont(make_single_preset_fixture());
    player.prepare(kOutRate, 256);
    player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 9)));
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 110)));
    REQUIRE(peak(render(player, 2048).left) == 0.0f);
    REQUIRE(player.active_voice_count() == 0);
  }
  SECTION("synth_fallback=false also disables the model-first override") {
    cfg.synth_fallback = false;
    cfg.prefer_model_for_modeled_families = true;
    Sf2Player player(cfg);
    player.set_soundfont(make_single_preset_fixture());
    player.prepare(kOutRate, 256);
    // Program 0 is covered by the fixture and has a dedicated physical model.
    // With fallback disabled, model-first must not bypass the SF2 preset.
    player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 0)));
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 110)));
    REQUIRE(peak(render(player, 2048).left) > 1.0e-4f);
  }
}

TEST_CASE("Sf2Player skips a malformed-rate zone and keeps the GM fallback audible",
          "[midi][sf2][synth][malformed]") {
  auto sf2 = make_single_preset_fixture();
  auto& samples = const_cast<std::vector<sonare::midi::synth::Sf2Sample>&>(sf2->samples());
  REQUIRE_FALSE(samples.empty());
  samples[0].sample_rate = 0;  // Defense-in-depth for an externally corrupted model.

  Sf2PlayerConfig config;
  config.gain = 1.0f;
  Sf2Player player(config);
  player.set_soundfont(sf2);
  player.prepare(kOutRate, 256);
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 110)));
  const StereoRender output = render(player, 4096);
  const auto [minimum, maximum] = std::minmax_element(output.left.begin(), output.left.end());
  REQUIRE(peak(output.left) > 1.0e-4f);
  REQUIRE(*maximum - *minimum > 1.0e-4f);  // Not a stuck/DC sample position.
  REQUIRE(player.active_voice_count() > 0);
}

TEST_CASE("Sf2Player fallback tail covers the slowest fallback release", "[midi][sf2][synth]") {
  Sf2Player player = make_fallback_player();
  REQUIRE(player.tail_samples() > 0);
  // The longest fallback envelope must fit the reported tail: play the pad
  // (800 ms release), release it and verify silence within the tail.
  player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 88)));
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 110)));
  render(player, 4096);
  player.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
  const StereoRender out = render(player, player.tail_samples() + 4096);
  REQUIRE(peak(out.left, out.left.size() - 256) < 1.0e-3f);
}

TEST_CASE("Sf2Player keeps a sustained wind render DC-free", "[midi][sf2][synth]") {
  // The fallback floor voices the same physical models as the NativeSynth
  // host, and a sustained wind part leaves a DC offset on the mix bus: it eats
  // headroom and skews the peak level a downstream mastering chain measures.
  // A null `block` leaves the config default alone, so the shipped behaviour is
  // measured rather than an explicitly-enabled one.
  auto mean_offset = [](const bool* block) {
    Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
    if (block != nullptr) cfg.dc_block = *block;
    Sf2Player player(cfg);  // no set_soundfont -> fallback floor
    player.prepare(kOutRate, 256);
    // Four sustained wind parts (flute / clarinet / trumpet / oboe).
    const uint8_t programs[4] = {73, 71, 56, 68};
    for (uint8_t part = 0; part < 4; ++part) {
      player.on_event(0, event(sonare::midi::make_midi1_program_change(0, part, programs[part])));
      player.on_event(0, event(sonare::midi::make_midi1_note_on(
                             0, part, static_cast<uint8_t>(60 + part * 4), 100)));
    }
    const StereoRender out = render(player, 96000);
    double sum = 0.0;
    for (size_t i = 48000; i < out.left.size(); ++i) sum += static_cast<double>(out.left[i]);
    return std::fabs(sum / static_cast<double>(out.left.size() - 48000));
  };

  const bool off = false;
  const bool on = true;
  const double unblocked = mean_offset(&off);
  const double blocked = mean_offset(&on);
  const double by_default = mean_offset(nullptr);
  INFO("dc offset unblocked " << unblocked << " blocked " << blocked << " default " << by_default);
  REQUIRE(unblocked > 1.0e-3);  // the offset the blocker exists to remove
  REQUIRE(blocked < 1.0e-3);
  REQUIRE(blocked < 0.01 * unblocked);
  REQUIRE(by_default == blocked);  // the blocker is on unless a host opts out
}

TEST_CASE("a filter sweep is audible on a held note", "[midi][synth]") {
  // cutoffHz / resonanceQ are documented as applying to sounding voices from
  // the next block. A wide-open patch skips the filter stage as a fast path,
  // and deciding that once at note-on froze the sweep out of every note that
  // was already down — the single most common synth automation gesture.
  NativeSynthConfig cfg;
  cfg.patch.waveform = VaWaveform::kSaw;
  cfg.patch.cutoff_hz = 20000.0f;
  cfg.patch.gain = 0.8f;
  cfg.patch.amp_env.attack_ms = 1.0f;
  cfg.patch.amp_env.sustain = 1.0f;
  NativeSynth synth(cfg);
  synth.prepare(kOutRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 110)));

  const StereoRender open = render(synth, 8192);
  const float open_rms = rms(open.left, 4096);
  REQUIRE(open_rms > 0.01f);

  // Same held note, cutoff swept two octaves below its fundamental: the saw
  // has to lose the fundamental with the harmonics.
  REQUIRE(synth.apply_parameter(
      static_cast<unsigned int>(sonare::midi::synth::NativeSynthParamId::kCutoffHz), 110.0f));
  const StereoRender swept = render(synth, 8192);
  REQUIRE(rms(swept.left, 4096) < 0.5f * open_rms);
  REQUIRE(synth.active_voice_count() == 1);  // the same voice, never retriggered

  // And back up: the fast path returns rather than latching the filter in.
  REQUIRE(synth.apply_parameter(
      static_cast<unsigned int>(sonare::midi::synth::NativeSynthParamId::kCutoffHz), 20000.0f));
  const StereoRender reopened = render(synth, 8192);
  REQUIRE(rms(reopened.left, 4096) > 0.8f * open_rms);
}

TEST_CASE("All Sound Off silences the piano bus resonators too", "[midi][synth][sf2]") {
  // "Silence NOW" has to include the resonators the instrument owns: the piano
  // soundboard and the pedal-gated sympathetic bank ring for over a second, so
  // killing only the voices leaks an audible wash past a DAW panic.
  SECTION("NativeSynth") {
    NativeSynthConfig cfg;
    cfg.patch.mode = SynthEngineMode::kPiano;
    cfg.gain = 1.0f;
    NativeSynth synth(cfg);
    synth.prepare(kOutRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, 127)));
    for (uint8_t note = 48; note < 60; note += 4) {
      synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, 110)));
    }
    REQUIRE(peak(render(synth, 4800).left) > 0.01f);
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 120, 0)));
    REQUIRE(synth.active_voice_count() == 0);
    REQUIRE(peak(render(synth, 256).left) < 3.2e-8f);  // -150 dBFS
  }

  SECTION("Sf2Player fallback") {
    // The GS system reverb is a send bus addressed on its own, not something
    // the instrument owns, so it is switched off to leave only the part's
    // body resonators in the measurement.
    Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
#if defined(SONARE_MIDI_WITH_FX)
    cfg.effects.enable_reverb = false;
    cfg.effects.enable_chorus = false;
    cfg.effects.enable_delay = false;
#endif
    Sf2Player player(cfg);  // no set_soundfont -> fallback floor
    player.prepare(kOutRate, 256);
    player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 0)));
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, 127)));
    for (uint8_t note = 48; note < 60; note += 4) {
      player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, 110)));
    }
    REQUIRE(peak(render(player, 4800).left) > 0.01f);
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 120, 0)));
    REQUIRE(player.active_voice_count() == 0);
    REQUIRE(peak(render(player, 256).left) < 3.2e-8f);
  }
}

TEST_CASE("tail_samples covers the stage that actually ends the voice", "[midi][synth]") {
  // A zero-sustain envelope dies at the decay floor and never reaches Release,
  // so for a percussive patch the decay is the terminating stage. Reporting
  // only the release cuts the hit off at a bounce boundary.
  NativeSynthConfig cfg;
  cfg.patch.one_shot = true;  // a strike rings out; note-off never chokes it
  cfg.patch.amp_env.sustain = 0.0f;
  cfg.patch.amp_env.decay_ms = 2000.0f;
  cfg.patch.amp_env.release_ms = 5.0f;
  NativeSynth synth(cfg);
  synth.prepare(kOutRate, 256);
  const int64_t decay_tail =
      sonare::midi::synth::DahdsrEnvelope::release_tail_samples(kOutRate, 2000.0f);
  REQUIRE(synth.tail_samples() >= decay_tail);

  // Behaviourally: the strike has to fade inside the reported tail.
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 110)));
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
  const StereoRender out = render(synth, static_cast<int>(synth.tail_samples()) + 256);
  REQUIRE(peak(out.left) > 0.01f);
  REQUIRE(peak(out.left, out.left.size() - 256) < 1.0e-3f);
}

TEST_CASE("both hosts report a tail that covers the piano body", "[midi][synth][sf2]") {
  // The piano body rings far past the ~120 ms voice release; a tail that
  // covers only the release cuts the bloom off the last chord of a bounce.
  const int64_t body = static_cast<int64_t>(kOutRate * 0.6);  // 2x the bank t60

  NativeSynthConfig cfg;
  cfg.patch.mode = SynthEngineMode::kPiano;
  NativeSynth synth(cfg);
  synth.prepare(kOutRate, 256);
  const int64_t release_only = sonare::midi::synth::DahdsrEnvelope::release_tail_samples(
      kOutRate, cfg.patch.amp_env.release_ms);
  REQUIRE(synth.tail_samples() >= release_only + body);

  Sf2Player player = make_fallback_player();
  REQUIRE(player.tail_samples() >= body);
}

TEST_CASE("GM mode voices a piano program through the piano body", "[midi][synth][sf2]") {
  // The bus body (direct-share attenuation, modal soundboard, pedal-gated
  // sympathetic bank) used to be decided from the construction-time patch. GM
  // mode resolves the engine per program instead, so program 0 resolved to the
  // piano patch and then rendered as a bare string: a different instrument
  // from the same patch played directly, and from the SF2 fallback floor.
  const sonare::midi::synth::NativeSynthPatch& gm_piano =
      sonare::midi::synth::gm_fallback_patch(0, 0);
  REQUIRE(gm_piano.mode == SynthEngineMode::kPiano);

  auto play = [&](bool gm) {
    NativeSynthConfig cfg;
    cfg.gain = 1.0f;
    cfg.use_gm_programs = gm;
    if (!gm) cfg.patch = gm_piano;
    NativeSynth synth(cfg);
    synth.prepare(kOutRate, 256);
    if (gm) synth.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 0)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 48, 100)));
    return render(synth, 16384);
  };

  // One patch, one engine, one bus coupling: reaching it through a GM program
  // change has to sound like configuring it directly.
  const StereoRender through_gm = play(true);
  const StereoRender configured = play(false);
  REQUIRE(peak(configured.left) > 0.01f);
  double diff = 0.0;
  double ref = 0.0;
  for (size_t i = 0; i < configured.left.size(); ++i) {
    diff += std::fabs(static_cast<double>(through_gm.left[i]) - configured.left[i]);
    diff += std::fabs(static_cast<double>(through_gm.right[i]) - configured.right[i]);
    ref += std::fabs(static_cast<double>(configured.left[i]));
    ref += std::fabs(static_cast<double>(configured.right[i]));
  }
  INFO("relative L1 difference " << (diff / ref));
  REQUIRE(diff <= 1.0e-4 * ref);

  // A non-piano GM program sharing the bus must not be pulled through the
  // soundboard just because a piano is sounding next to it.
  NativeSynthConfig cfg;
  cfg.use_gm_programs = true;
  cfg.gain = 1.0f;
  NativeSynth flute_only(cfg);
  flute_only.prepare(kOutRate, 256);
  flute_only.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 73)));
  flute_only.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 72, 100)));
  const StereoRender flute_alone = render(flute_only, 16384);

  NativeSynth mixed(cfg);
  mixed.prepare(kOutRate, 256);
  mixed.on_event(0, event(sonare::midi::make_midi1_program_change(0, 1, 0)));
  mixed.on_event(0, event(sonare::midi::make_midi1_note_on(0, 1, 36, 100)));
  mixed.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 73)));
  mixed.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 72, 100)));
  const StereoRender with_piano = render(mixed, 16384);
  REQUIRE(peak(with_piano.left) >= peak(flute_alone.left) * 0.99f);
}

#if defined(SONARE_TUNING) && SONARE_TUNING

TEST_CASE("patch tuning hands back a clamped patch", "[midi][synth][tuning]") {
  // The override map is arbitrary text — a key can name a non-finite value or
  // one outside the field's range — and the table builders clamp BEFORE
  // applying it. A value that survives unclamped is evaluated during the fit
  // and then truncated when it is written back into the source, so the fitted
  // voice does not reproduce in a shipped build.
  using sonare::midi::synth::apply_patch_tuning;
  using sonare::midi::synth::clamp_synth_patch;
  using sonare::midi::synth::NativeSynthPatch;

  NativeSynthPatch p;
  p.mode = SynthEngineMode::kPiano;
  p = clamp_synth_patch(p);
  p.piano.decay_stretch = 1.2f;  // above the clamp's upper bound of 1
  p.piano.brightness = -5.0f;    // below its lower bound of 0
  p.gain = std::numeric_limits<float>::quiet_NaN();
  apply_patch_tuning(p, "fam0");

  REQUIRE(p.piano.decay_stretch == 1.0f);
  REQUIRE(p.piano.brightness == 0.0f);
  REQUIRE(std::isfinite(p.gain));

  // Nothing else moved: clamping an already-clamped patch is the identity, so
  // this cannot perturb a fit that stayed inside the ranges.
  const NativeSynthPatch again = clamp_synth_patch(p);
  REQUIRE(again.piano.decay_stretch == p.piano.decay_stretch);
  REQUIRE(again.piano.brightness == p.piano.brightness);
  REQUIRE(again.gain == p.gain);
}

TEST_CASE("the tuning field table reaches every percussion field", "[midi][synth][tuning]") {
  // A drum fit that cannot address the shell resonance or the mode ratios
  // converges on the best point of a restricted subspace and reports nothing
  // about the restriction.
  using sonare::midi::synth::kMaxPercussionModes;
  using sonare::midi::synth::kMaxShellModes;
  using sonare::midi::synth::NativeSynthPatch;
  using sonare::midi::synth::patch_tuning_field_paths;

  NativeSynthPatch p;
  p.mode = SynthEngineMode::kPercussion;
  const std::vector<std::string> paths = patch_tuning_field_paths(p);
  const auto has = [&paths](const std::string& path) {
    return std::find(paths.begin(), paths.end(), path) != paths.end();
  };
  for (int i = 0; i < kMaxPercussionModes; ++i) {
    const std::string index = std::to_string(i);
    INFO("mode " << i);
    REQUIRE(has("percussion.mode_ratios" + index));
    REQUIRE(has("percussion.mode_alpha" + index));
  }
  for (int i = 0; i < kMaxShellModes; ++i) {
    const std::string index = std::to_string(i);
    INFO("shell mode " << i);
    REQUIRE(has("percussion.shell_freq_hz" + index));
    REQUIRE(has("percussion.shell_t60_s" + index));
    REQUIRE(has("percussion.shell_weight" + index));
  }
  // Exactly one key per field: a duplicated path would make two knobs fight
  // over the same value, with the later walk step silently winning.
  const std::set<std::string> unique(paths.begin(), paths.end());
  REQUIRE(unique.size() == paths.size());
}

#endif  // SONARE_TUNING

TEST_CASE("gm_fallback_max_release_ms bounds every fallback patch table", "[midi][synth]") {
  const float bound = sonare::midi::synth::gm_fallback_max_release_ms();
  const auto covered = [bound](const sonare::midi::synth::NativeSynthPatch& p) {
    return bound >= p.amp_env.release_ms && bound >= p.amp_env.decay_ms;
  };
  for (const auto& p : sonare::midi::synth::detail::family_patches()) {
    REQUIRE(covered(p));
  }
  for (const auto& p : sonare::midi::synth::detail::drum_note_table()) {
    REQUIRE(covered(p));
  }
  // Every program override, including ones added after this test was written:
  // the contiguous view spans the whole ProgramOverrides table.
  const auto* overrides = sonare::midi::synth::detail::program_override_patches(
      sonare::midi::synth::detail::program_overrides());
  for (std::size_t i = 0; i < sonare::midi::synth::detail::kProgramOverrideCount; ++i) {
    REQUIRE(covered(overrides[i]));
  }
}

TEST_CASE("NativeSynth audio path is allocation-free", "[midi][synth]") {
  NativeSynthConfig cfg;
  cfg.patch.unison = 7;
  cfg.patch.detune_cents = 15.0f;
  cfg.patch.drift_cents = 5.0f;
  NativeSynth synth(cfg);
  synth.prepare(kOutRate, 256);

  std::vector<float> left(256, 0.0f);
  std::vector<float> right(256, 0.0f);
  float* chans[2] = {left.data(), right.data()};
  {
    AllocationGuard guard;
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 64, 100)));
    synth.process(chans, 2, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
    synth.process(chans, 2, 256);
    REQUIRE(guard.count() == 0);
  }

  // GM mode tunes the bus piano body at the first piano note-on, on the audio
  // thread; the banks own no heap, so that stays allocation-free too.
  NativeSynthConfig gm;
  gm.use_gm_programs = true;
  NativeSynth gm_synth(gm);
  gm_synth.prepare(kOutRate, 256);
  {
    AllocationGuard guard;
    gm_synth.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 0)));
    gm_synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 48, 100)));
    gm_synth.process(chans, 2, 256);
    gm_synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 48, 0)));
    gm_synth.process(chans, 2, 256);
    REQUIRE(guard.count() == 0);
  }
}

TEST_CASE("Sf2Player fallback audio path is allocation-free", "[midi][sf2][synth]") {
  Sf2Player player = make_fallback_player();
  std::vector<float> left(256, 0.0f);
  std::vector<float> right(256, 0.0f);
  float* chans[2] = {left.data(), right.data()};
  {
    AllocationGuard guard;
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 9, 38, 100)));
    player.process(chans, 2, 256);
    player.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
    player.process(chans, 2, 256);
    REQUIRE(guard.count() == 0);
  }
}

TEST_CASE("Native and SF2 synths preserve source-track voice attribution", "[midi][synth]") {
  const auto exercise = [](auto& synth) {
    MidiEvent first = event(sonare::midi::make_midi1_note_on(0, 0, 60, 100));
    first.source_track_id = 101;
    MidiEvent second = event(sonare::midi::make_midi1_note_on(0, 0, 67, 100));
    second.source_track_id = 202;
    synth.on_event(0, first);
    synth.on_event(0, second);

    std::array<float, 256> fallback_l{};
    std::array<float, 256> fallback_r{};
    std::array<float, 256> first_l{};
    std::array<float, 256> first_r{};
    std::array<float, 256> second_l{};
    std::array<float, 256> second_r{};
    float* fallback[] = {fallback_l.data(), fallback_r.data()};
    float* first_track[] = {first_l.data(), first_r.data()};
    float* second_track[] = {second_l.data(), second_r.data()};
    const MidiInstrumentSourceOutput outputs[] = {
        {0, fallback}, {101, first_track}, {202, second_track}};

    {
      AllocationGuard guard;
      REQUIRE(synth.process_source_tracks(outputs, std::size(outputs), 2, 256));
      REQUIRE(guard.count() == 0);
    }
    float first_peak = 0.0f;
    float second_peak = 0.0f;
    for (size_t i = 0; i < first_l.size(); ++i) {
      first_peak = std::max(first_peak, std::abs(first_l[i]) + std::abs(first_r[i]));
      second_peak = std::max(second_peak, std::abs(second_l[i]) + std::abs(second_r[i]));
    }
    REQUIRE(first_peak > 0.0f);
    REQUIRE(second_peak > 0.0f);
  };

  NativeSynthConfig native_config;
  native_config.dc_block = false;
  NativeSynth native(native_config);
  native.prepare(kOutRate, 256);
  exercise(native);

  Sf2Player sf2 = make_fallback_player();
  exercise(sf2);
}

namespace {

/// Held note from a GM fallback program, left channel.
std::vector<float> render_program(uint8_t program, uint8_t note, int num_samples) {
  NativeSynthConfig cfg;
  cfg.patch = sonare::midi::synth::gm_fallback_patch(0, program);
  NativeSynth synth(cfg);
  synth.prepare(kOutRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, 100)));
  return render(synth, num_samples).left;
}

/// Fraction of spectral power above @p split_hz in a window of @p fft samples
/// starting at @p from. The window is short by design: the shared kFft window
/// is 170 ms of Hann, which tapers an onset transient to nothing.
double high_fraction(const std::vector<float>& buf, size_t from, double split_hz, int fft) {
  const std::vector<double> power = sonare::test::power_spectrum(buf, from, fft);
  const int split = static_cast<int>(std::lround(split_hz / kOutRate * fft));
  double low = 0.0;
  double high = 0.0;
  for (int b = 1; b < static_cast<int>(power.size()); ++b) {
    (b >= split ? high : low) += power[static_cast<size_t>(b)];
  }
  const double total = low + high;
  return total > 0.0 ? high / total : 0.0;
}

}  // namespace

TEST_CASE("the synth leads and pads are sixteen voices, not two", "[midi][synth]") {
  // 80-95 answered to two family patches, so eight leads rendered one sound and
  // eight pads another. Distinctness is the claim the names make; the levels
  // are the claim that they can be sequenced next to each other.
  std::vector<std::vector<float>> tones;
  for (int program = 80; program <= 95; ++program) {
    tones.push_back(render_program(static_cast<uint8_t>(program), 60, 96000));
  }
  float loudest = 0.0f;
  float quietest = 1.0f;
  for (const std::vector<float>& tone : tones) {
    const float p = peak(tone);
    REQUIRE(p > 0.01f);
    REQUIRE(p < 1.0f);
    loudest = std::max(loudest, p);
    quietest = std::min(quietest, p);
  }
  for (size_t i = 0; i < tones.size(); ++i) {
    for (size_t j = i + 1; j < tones.size(); ++j) REQUIRE(tones[i] != tones[j]);
  }
  REQUIRE(loudest < 1.42f * quietest);  // inside 3 dB
}

TEST_CASE("the chiff lead's brightness is in its onset", "[midi][synth]") {
  // "Chiff" names the breath edge of a flue pipe's speech, so the program is
  // its transient: a filter envelope wide open for the first few tens of
  // milliseconds and shut after. The sawtooth lead is the control — an ordinary
  // lead's brightness does not collapse.
  const std::vector<float> chiff = render_program(83, 60, 96000);
  const std::vector<float> saw = render_program(81, 60, 96000);
  const double chiff_onset = high_fraction(chiff, 0, 3000.0, 1024);
  const double chiff_held = high_fraction(chiff, 40000, 3000.0, 1024);
  const double saw_onset = high_fraction(saw, 0, 3000.0, 1024);
  const double saw_held = high_fraction(saw, 40000, 3000.0, 1024);
  // Measured 36x against the lead's own 3.4x: every lead here brightens a
  // little at the onset, and the chiff is the one where that IS the sound.
  REQUIRE(chiff_onset > 15.0 * chiff_held);
  REQUIRE(saw_onset < 6.0 * saw_held);
}

TEST_CASE("the sweep pad's filter is the program", "[midi][synth]") {
  // Sweep is the one pad whose identity is a modulation rather than a timbre,
  // and the second LFO reaches the cutoff only through the matrix. A route that
  // silently failed to arrive would leave a pad that is merely warm.
  const std::vector<float> sweep = render_program(95, 60, 240000);
  double brightest = 0.0;
  double dullest = 1.0;
  for (int w = 0; w < 5; ++w) {
    const double h = high_fraction(sweep, static_cast<size_t>(w) * 32768 + 16384, 1500.0, 1024);
    brightest = std::max(brightest, h);
    dullest = std::min(dullest, h);
  }
  REQUIRE(brightest > 30.0 * dullest);
}

TEST_CASE("the metallic pad is a band, not a roll-off", "[midi][synth]") {
  // A subtractive synth makes metal with a narrow resonant band, which needs
  // the state-variable filter: it is the only model here with a bandpass
  // output, so this is the one pad that must not be on a ladder. What that
  // buys is a missing bottom, which a lowpass pad cannot have.
  const std::vector<float> metallic = render_program(93, 60, 96000);
  const std::vector<float> warm = render_program(89, 60, 96000);
  const double metallic_low = 1.0 - high_fraction(metallic, 40000, 800.0, 4096);
  const double warm_low = 1.0 - high_fraction(warm, 40000, 800.0, 4096);
  REQUIRE(metallic_low < 0.25 * warm_low);
}

namespace {

/// Depth of whatever modulates the envelope of @p buf, in dB: the RMS residual
/// of the log envelope after its decay has been fitted out with a straight
/// line. A smoothly decaying note leaves almost nothing; beating unisons leave
/// the beat. Windows are 25 ms over the following two seconds.
double beat_depth(const std::vector<float>& buf, size_t from) {
  constexpr size_t kWin = 1200;
  constexpr int kCount = 80;
  std::vector<double> log_env;
  for (int w = 0; w < kCount; ++w) {
    double sum = 0.0;
    const size_t start = from + static_cast<size_t>(w) * kWin;
    for (size_t i = start; i < start + kWin; ++i) sum += double(buf[i]) * double(buf[i]);
    log_env.push_back(std::log(std::sqrt(sum / double(kWin)) + 1e-12));
  }
  double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
  for (int w = 0; w < kCount; ++w) {
    sx += w;
    sy += log_env[static_cast<size_t>(w)];
    sxx += double(w) * w;
    sxy += double(w) * log_env[static_cast<size_t>(w)];
  }
  const double slope = (kCount * sxy - sx * sy) / (kCount * sxx - sx * sx);
  const double intercept = (sy - slope * sx) / kCount;
  double residual = 0.0;
  for (int w = 0; w < kCount; ++w) {
    const double d = log_env[static_cast<size_t>(w)] - (intercept + slope * w);
    residual += d * d;
  }
  return std::sqrt(residual / kCount) * 8.6858896;  // nepers -> dB
}

/// RMS of a 100 ms window three seconds in, where a grand is carrying its
/// aftersound and nothing else.
double aftersound_rms(const std::vector<float>& buf) {
  double sum = 0.0;
  for (size_t i = 144000; i < 148800; ++i) sum += double(buf[i]) * double(buf[i]);
  return std::sqrt(sum / 4800.0);
}

}  // namespace

TEST_CASE("programs 0-3 are four pianos, not one", "[midi][synth]") {
  // All four answered to the family patch, so Bright, Electric Grand and
  // Honky-tonk each rendered the concert grand. Each derives from it and changes
  // the one thing its GM name names, and each of those is measurable.
  const std::vector<float> grand = render_program(0, 60, 240000);
  const std::vector<float> bright = render_program(1, 60, 240000);
  const std::vector<float> electric = render_program(2, 60, 240000);
  const std::vector<float> honky = render_program(3, 60, 240000);
  REQUIRE(bright != grand);
  REQUIRE(electric != grand);
  REQUIRE(honky != grand);

  // Bright: a stiffer felt leaves the string sooner, so the sustained tone
  // keeps high partials the grand has damped. Measured 4.4x.
  REQUIRE(high_fraction(bright, 40000, 3000.0, 1024) >
          2.5 * high_fraction(grand, 40000, 3000.0, 1024));

  // Electric grand: no board, so the aftersound the board carried is gone
  // (measured -7.3 dB at three seconds), and the level is the grand's because
  // an amplified instrument's is not its own.
  REQUIRE(aftersound_rms(electric) < 0.6 * aftersound_rms(grand));
  REQUIRE(peak(electric) > 0.94f * peak(grand));
  REQUIRE(peak(electric) < 1.06f * peak(grand));

  // Honky-tonk: the beat is the instrument. It shows in both registers, and
  // most clearly low, where the grand's near-unison strings barely move at all.
  REQUIRE(beat_depth(honky, 24000) > 1.4 * beat_depth(grand, 24000));
  const std::vector<float> honky_low = render_program(3, 40, 240000);
  const std::vector<float> grand_low = render_program(0, 40, 240000);
  REQUIRE(beat_depth(honky_low, 24000) > 3.0 * beat_depth(grand_low, 24000));
}

TEST_CASE("each piano's wide variation hangs under its own capital", "[midi][synth]") {
  // Programs 1-3 all pointed their variation-8 bank at the grand's wide patch,
  // which was harmless while their capitals were the grand too. Now it would
  // make Bright Piano wide duller than Bright Piano.
  using sonare::midi::synth::gm_fallback_patch;
  using sonare::midi::synth::NativeSynthPatch;
  for (uint8_t program = 1; program <= 3; ++program) {
    const NativeSynthPatch& capital = gm_fallback_patch(0, program);
    const NativeSynthPatch& wide = gm_fallback_patch(8, program);
    INFO("program " << int(program));
    REQUIRE(wide.piano.brightness == capital.piano.brightness);
    REQUIRE(wide.piano.soundboard == capital.piano.soundboard);
    REQUIRE(wide.gain == capital.gain);
    REQUIRE(wide.stereo_spread > capital.stereo_spread);
  }
}
