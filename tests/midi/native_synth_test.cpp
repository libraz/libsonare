/// @file native_synth_test.cpp
/// @brief NativeSynth VA engine (midi/synth/native_synth, oscillator,
///        gm_fallback_map): PolyBLEP antialiasing regression, deterministic
///        rendering, MidiInstrument channel semantics (sustain / all-sound-off
///        / volume), the Sf2Player synth fallback (every GM program and drum
///        note audible without a SoundFont, one-shot drums, fallback-vs-SF2
///        coexistence) and the allocation-free audio path.

#include "midi/synth/native_synth.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstdint>
#include <memory>
#include <set>
#include <vector>

#include "core/fft.h"
#include "midi/midi_event.h"
#include "midi/synth/oscillator.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/alloc_guard.h"
#include "support/sf2_builder.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
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
