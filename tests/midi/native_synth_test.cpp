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
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "core/fft.h"
#if defined(SONARE_WITH_MASTERING)
#include "mastering/api/insert_factory.h"
#endif
#include "midi/controller_profile.h"
#include "midi/midi_event.h"
#include "midi/synth/gm_fallback_data.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/oscillator.h"
#include "midi/synth/patch_tuning.h"
#include "midi/synth/sf2_player.h"
#include "midi/synth/synth_presets.h"
#include "midi/ump.h"
#include "support/alloc_guard.h"
#include "support/audio_fixtures.h"
#include "support/midi_render.h"
#include "support/sf2_builder.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/math_utils.h"

namespace {

using sonare::db_to_linear;
using sonare::pearson_correlation;
using sonare::midi::MidiEvent;
using sonare::midi::MidiInstrumentSourceOutput;
using sonare::midi::synth::harpsichord_buffer_capacity;
using sonare::midi::synth::harpsichord_slab_capacity;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::NativeSynthVoice;
using sonare::midi::synth::Sf2ChannelMod;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::midi::synth::SynthEngineMode;
using sonare::midi::synth::VaOscillator;
using sonare::midi::synth::VaWaveform;
using sonare::test::AllocationGuard;
using sonare::test::Sf2Builder;

constexpr double kOutRate = 48000.0;

using sonare::test::event;

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

TEST_CASE("a quarter-width pulse carries no DC and drops its fourth harmonic", "[midi][synth]") {
  // 375 Hz is 128 samples a period, so 8192 samples hold whole periods.
  constexpr int kN = 8192;
  constexpr double kF0 = 375.0;
  VaOscillator osc;
  osc.start(kOutRate, VaWaveform::kSquare, 0.0f, 0);
  osc.set_pulse_width(0.25f);
  osc.set_frequency(static_cast<float>(kF0));
  std::vector<float> buf(kN);
  for (float& s : buf) s = osc.next();
  const auto harmonic = [&](int k) {
    double re = 0.0;
    double im = 0.0;
    for (int i = 0; i < kN; ++i) {
      const double w = sonare::constants::kTwoPiD * k * kF0 * i / kOutRate;
      re += buf[static_cast<size_t>(i)] * std::cos(w);
      im += buf[static_cast<size_t>(i)] * std::sin(w);
    }
    return 2.0 * std::hypot(re, im) / kN;
  };
  double mean = 0.0;
  for (const float s : buf) mean += s;
  mean /= kN;
  REQUIRE(std::abs(mean) < 1.0e-3);
  // |sin(pi k w)| / k: h2 sits 3 dB under h1 and h4 is a null.
  const double h1 = harmonic(1);
  REQUIRE(std::abs(20.0 * std::log10(harmonic(2) / h1) + 3.01) < 0.3);
  REQUIRE(harmonic(4) < 0.01 * h1);
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

TEST_CASE("an explicit zero config gain renders silence on both hosts", "[midi][synth][gain]") {
  auto note_peak = [](auto make_instrument) {
    auto instrument = make_instrument();
    instrument.prepare(kOutRate, 256);
    instrument.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
    return peak(render(instrument, 2048).left);
  };

  NativeSynthConfig native_zero;
  native_zero.gain = 0.0f;
  REQUIRE(note_peak([&] { return NativeSynth(native_zero); }) == 0.0f);

  NativeSynthConfig native_quiet;
  native_quiet.gain = 0.01f;
  REQUIRE(note_peak([&] { return NativeSynth(native_quiet); }) > 0.0f);

  // Negative or non-finite is not a level the caller could have meant, so it
  // still falls back to the library default rather than rendering silence.
  for (float bad :
       {-1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
    NativeSynthConfig native_bad;
    native_bad.gain = bad;
    REQUIRE(note_peak([&] { return NativeSynth(native_bad); }) > 0.0f);
  }

  Sf2PlayerConfig sf2_zero;
  sf2_zero.gain = 0.0f;
  REQUIRE(note_peak([&] { return Sf2Player(sf2_zero); }) == 0.0f);

  Sf2PlayerConfig sf2_quiet;
  sf2_quiet.gain = 0.01f;
  REQUIRE(note_peak([&] { return Sf2Player(sf2_quiet); }) > 0.0f);

  for (float bad :
       {-1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
    Sf2PlayerConfig sf2_bad;
    sf2_bad.gain = bad;
    REQUIRE(note_peak([&] { return Sf2Player(sf2_bad); }) > 0.0f);
  }
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
  const int64_t expected =
      sonare::midi::synth::gm_fallback_max_tail_samples(kOutRate, 1.0f, 1.0f, 1.0f);
  REQUIRE(static_cast<int64_t>(synth.tail_samples()) >= expected);

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

namespace lfo_shape_detail {

// Vibrato in cents per carrier cycle, read off successive rising zero crossings.
struct PitchTrack {
  std::vector<double> time_s;
  std::vector<double> cents;
};

PitchTrack pitch_track(const std::vector<float>& buf, double sample_rate, double ref_hz) {
  PitchTrack track;
  double last = -1.0;
  for (size_t i = 1; i < buf.size(); ++i) {
    if (!(buf[i - 1] < 0.0f && buf[i] >= 0.0f)) continue;
    const double denom = static_cast<double>(buf[i]) - static_cast<double>(buf[i - 1]);
    const double t = static_cast<double>(i - 1) - static_cast<double>(buf[i - 1]) / denom;
    if (last >= 0.0) {
      track.time_s.push_back(0.5 * (t + last) / sample_rate);
      track.cents.push_back(1200.0 * std::log2(sample_rate / (t - last) / ref_hz));
    }
    last = t;
  }
  return track;
}

// Amplitude of the track's component at @p hz over [from_s, to_s).
double component(const PitchTrack& track, double hz, double from_s, double to_s) {
  double re = 0.0;
  double im = 0.0;
  int n = 0;
  for (size_t i = 0; i < track.time_s.size(); ++i) {
    if (track.time_s[i] < from_s || track.time_s[i] >= to_s) continue;
    const double w = sonare::constants::kTwoPiD * hz * track.time_s[i];
    re += track.cents[i] * std::cos(w);
    im += track.cents[i] * std::sin(w);
    ++n;
  }
  return n > 0 ? 2.0 * std::hypot(re, im) / n : 0.0;
}

}  // namespace lfo_shape_detail

TEST_CASE("NativeSynth LFO shape: sine vibrato carries no third harmonic, triangle stays default",
          "[midi][synth][lfo_shape]") {
  using sonare::midi::synth::LfoShape;
  constexpr float kRateHz = 5.0f;
  constexpr float kDepthCents = 40.0f;
  constexpr double kCarrierHz = 1760.0;  // note 93

  REQUIRE(NativeSynthPatch{}.lfo_shape == LfoShape::kTriangle);

  auto render_vibrato = [&](LfoShape shape) {
    NativeSynthConfig cfg;
    cfg.patch.waveform = VaWaveform::kSine;
    cfg.patch.cutoff_hz = 20000.0f;
    cfg.patch.gain = 0.8f;
    cfg.patch.drift_cents = 0.0f;
    cfg.patch.lfo_rate_hz = kRateHz;
    cfg.patch.lfo_to_pitch_cents = kDepthCents;
    cfg.patch.lfo_shape = shape;
    NativeSynth synth(cfg);
    synth.prepare(kOutRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 93, 127)));
    return render(synth, static_cast<int>(kOutRate * 2.0)).left;
  };

  // Eight whole LFO cycles past the attack.
  constexpr double kFrom = 0.2;
  constexpr double kTo = kFrom + 8.0 / kRateHz;
  const auto harmonics = [&](LfoShape shape) {
    const auto track = lfo_shape_detail::pitch_track(render_vibrato(shape), kOutRate, kCarrierHz);
    const double h1 = lfo_shape_detail::component(track, kRateHz, kFrom, kTo);
    const double h3 = lfo_shape_detail::component(track, 3.0 * kRateHz, kFrom, kTo);
    return std::pair<double, double>{h1, 20.0 * std::log10(h3 / h1)};
  };

  const auto [sine_h1, sine_h3_db] = harmonics(LfoShape::kSine);
  const auto [tri_h1, tri_h3_db] = harmonics(LfoShape::kTriangle);
  // Both reach the asked depth: a sine's peak is its fundamental, a triangle's is 8/pi^2 of it.
  CHECK(sine_h1 == Catch::Approx(kDepthCents).epsilon(0.03));
  constexpr double kPi = sonare::constants::kPiD;
  CHECK(tri_h1 == Catch::Approx(kDepthCents * 8.0 / (kPi * kPi)).epsilon(0.03));
  CHECK(sine_h3_db < -40.0);
  // A triangle's third harmonic is 1/9 of its fundamental, -19.1 dB; this is what the sine removes.
  CHECK(tri_h3_db == Catch::Approx(-19.1).margin(1.5));

  // The default patch takes the triangle path sample for sample.
  NativeSynthConfig implicit;
  implicit.patch.waveform = VaWaveform::kSine;
  implicit.patch.lfo_to_pitch_cents = kDepthCents;
  NativeSynthConfig explicit_tri = implicit;
  explicit_tri.patch.lfo_shape = LfoShape::kTriangle;
  NativeSynth a(implicit);
  NativeSynth b(explicit_tri);
  a.prepare(kOutRate, 256);
  b.prepare(kOutRate, 256);
  a.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 100)));
  b.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 100)));
  REQUIRE(render(a, 9600).left == render(b, 9600).left);
}

TEST_CASE("NativeSynth LFO shape: a sine LFO2 tremolo carries no third-order sideband",
          "[midi][synth][lfo_shape]") {
  using sonare::midi::synth::LfoShape;
  using sonare::midi::synth::ModDestination;
  using sonare::midi::synth::ModSource;
  constexpr float kRateHz = 4.0f;
  constexpr float kDepth = 0.35f;
  constexpr double kCarrierHz = 440.0;  // note 69

  auto render_tremolo = [&](LfoShape shape) {
    NativeSynthConfig cfg;
    cfg.patch.waveform = VaWaveform::kSine;
    cfg.patch.cutoff_hz = 20000.0f;
    cfg.patch.gain = 0.8f;
    cfg.patch.drift_cents = 0.0f;
    cfg.patch.amp_env.attack_ms = 1.0f;
    cfg.patch.amp_env.sustain = 1.0f;
    cfg.patch.lfo2_rate_hz = kRateHz;
    cfg.patch.mod_matrix.routes[0] = {ModSource::kLfo2, ModDestination::kAmpGain, kDepth};
    cfg.patch.lfo_shape = shape;
    NativeSynth synth(cfg);
    synth.prepare(kOutRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 127)));
    return render(synth, static_cast<int>(kOutRate * 2.5)).left;
  };

  // Hann-weighted DFT over eight whole LFO cycles past the attack, in dB re the carrier.
  const auto sidebands = [&](LfoShape shape) {
    const auto buf = render_tremolo(shape);
    const size_t from = static_cast<size_t>(0.25 * kOutRate);
    const size_t len = static_cast<size_t>(8.0 / kRateHz * kOutRate);
    REQUIRE(buf.size() >= from + len);
    const auto level = [&](double hz) {
      double re = 0.0;
      double im = 0.0;
      for (size_t i = 0; i < len; ++i) {
        const double w =
            0.5 - 0.5 * std::cos(sonare::constants::kTwoPiD * static_cast<double>(i) / len);
        const double ph = sonare::constants::kTwoPiD * hz * static_cast<double>(i) / kOutRate;
        re += w * buf[from + i] * std::cos(ph);
        im += w * buf[from + i] * std::sin(ph);
      }
      return std::hypot(re, im);
    };
    const double carrier = level(kCarrierHz);
    const auto rel_db = [&](int k) {
      const double side = 0.5 * (level(kCarrierHz - k * kRateHz) + level(kCarrierHz + k * kRateHz));
      return 20.0 * std::log10(side / carrier);
    };
    return std::pair<double, double>{rel_db(1), rel_db(3)};
  };

  const auto [sine_k1_db, sine_k3_db] = sidebands(LfoShape::kSine);
  const auto [tri_k1_db, tri_k3_db] = sidebands(LfoShape::kTriangle);
  // Linear AM at depth m puts each first sideband at m/2: -15.1 dB for the sine, 8/pi^2 of it
  // (-17.0 dB) for the triangle, whose third harmonic is 1/9 of that again (-36.1 dB).
  CHECK(sine_k1_db == Catch::Approx(-15.1).margin(1.0));
  CHECK(tri_k1_db == Catch::Approx(-17.0).margin(1.0));
  CHECK(sine_k3_db < -60.0);
  CHECK(tri_k3_db > -40.0);
}

namespace va_section_detail {

// Hann-weighted DFT magnitude of @p buf at @p hz over [from, from + len), as a sinusoid's
// amplitude.
double hann_level(const std::vector<float>& buf, double sample_rate, double hz, size_t from,
                  size_t len) {
  double re = 0.0;
  double im = 0.0;
  double wsum = 0.0;
  for (size_t i = 0; i < len; ++i) {
    const double w =
        0.5 - 0.5 * std::cos(sonare::constants::kTwoPiD * static_cast<double>(i) / len);
    const double ph = sonare::constants::kTwoPiD * hz * static_cast<double>(i) / sample_rate;
    re += w * buf[from + i] * std::cos(ph);
    im += w * buf[from + i] * std::sin(ph);
    wsum += w;
  }
  return 2.0 * std::hypot(re, im) / wsum;
}

std::vector<float> lfo_run(sonare::midi::synth::LfoShape shape, float rate_hz, uint64_t seed,
                           size_t n) {
  sonare::midi::synth::Sf2Lfo lfo;
  lfo.start(kOutRate, 0.0f, rate_hz, shape, seed);
  std::vector<float> out(n);
  for (float& v : out) v = lfo.next();
  return out;
}

}  // namespace va_section_detail

TEST_CASE("Sf2Lfo random shape: seeded, bounded, and without a line at the rate",
          "[midi][synth][lfo_shape]") {
  using sonare::midi::synth::LfoShape;
  using va_section_detail::hann_level;
  using va_section_detail::lfo_run;
  constexpr float kRateHz = 5.0f;
  const size_t n = static_cast<size_t>(64.0 / kRateHz * kOutRate);

  const std::vector<float> a = lfo_run(LfoShape::kRandom, kRateHz, 11, n);
  REQUIRE(a.front() == 0.0f);
  REQUIRE(lfo_run(LfoShape::kRandom, kRateHz, 11, n) == a);
  REQUIRE(lfo_run(LfoShape::kRandom, kRateHz, 12, n) != a);
  float lo = 0.0f;
  float hi = 0.0f;
  for (float v : a) {
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  CHECK(lo >= -1.0f);
  CHECK(hi <= 1.0f);
  // Not vacuous: 64 uniform targets reach well into both halves.
  CHECK(hi - lo > 1.0f);

  // The half-cosine glide has spectral nulls at every multiple of the rate, where the
  // periodic shapes put their lines; the band between them is where the wander lives.
  const std::vector<float> sine = lfo_run(LfoShape::kSine, kRateHz, 11, n);
  const std::vector<float> tri = lfo_run(LfoShape::kTriangle, kRateHz, 11, n);
  CHECK(hann_level(sine, kOutRate, kRateHz, 0, n) > 0.9);
  CHECK(hann_level(tri, kOutRate, 3.0 * kRateHz, 0, n) > 0.05);
  double band = 0.0;
  for (int i = 0; i <= 10; ++i) band += hann_level(a, kOutRate, kRateHz * (0.3 + 0.05 * i), 0, n);
  band /= 11.0;
  CHECK(band > 0.01);
  for (int k = 1; k <= 3; ++k) {
    INFO("harmonic " << k);
    CHECK(hann_level(a, kOutRate, k * kRateHz, 0, n) < 0.01);
  }

  // A zero rate runs on to a target of 0 and rests there.
  sonare::midi::synth::Sf2Lfo lfo;
  lfo.start(kOutRate, 0.0f, kRateHz, LfoShape::kRandom, 11);
  for (int i = 0; i < 1000; ++i) lfo.next();
  lfo.set_frequency(kOutRate, 0.0f);
  for (int i = 0; i < static_cast<int>(2.0 / kRateHz * kOutRate) + 2; ++i) lfo.next();
  REQUIRE(lfo.next() == 0.0f);
  REQUIRE(lfo.next() == 0.0f);
}

TEST_CASE("NativeSynth random LFO shape reaches the voice and repeats per seed",
          "[midi][synth][lfo_shape]") {
  using sonare::midi::synth::LfoShape;
  auto render_shape = [](LfoShape shape, uint8_t note) {
    NativeSynthConfig cfg;
    cfg.patch.waveform = VaWaveform::kSine;
    cfg.patch.lfo_rate_hz = 6.0f;
    cfg.patch.lfo_to_pitch_cents = 50.0f;
    cfg.patch.lfo_shape = shape;
    NativeSynth synth(cfg);
    synth.prepare(kOutRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, 100)));
    return render(synth, 24000).left;
  };
  const auto random = render_shape(LfoShape::kRandom, 69);
  REQUIRE(render_shape(LfoShape::kRandom, 69) == random);
  REQUIRE(render_shape(LfoShape::kTriangle, 69) != random);
  REQUIRE(render_shape(LfoShape::kSine, 69) != random);
}

TEST_CASE("NativeSynth subtractive section: osc2 and noise are absent at level 0",
          "[midi][synth][va]") {
  REQUIRE(NativeSynthPatch{}.va.osc2_level == 0.0f);
  REQUIRE(NativeSynthPatch{}.va.noise_level == 0.0f);
  REQUIRE(NativeSynthPatch{}.va.noise_am == 0.0f);
  REQUIRE(NativeSynthPatch{}.va.band_noise_level == 0.0f);
  auto render_patch = [](const NativeSynthPatch& patch) {
    NativeSynthConfig cfg;
    cfg.patch = patch;
    NativeSynth synth(cfg);
    synth.prepare(kOutRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 100)));
    return render(synth, 9600).left;
  };
  NativeSynthPatch base;
  base.unison = 3;
  base.detune_cents = 12.0f;
  NativeSynthPatch interval_only = base;
  interval_only.va.osc2_interval_cents = 702.0f;
  REQUIRE(render_patch(interval_only) == render_patch(base));
  NativeSynthPatch corners_only = base;
  corners_only.va.band_noise_low_hz = 800.0f;
  corners_only.va.band_noise_low_key_track = 1.0f;
  corners_only.va.band_noise_high_hz = 900.0f;
  corners_only.va.band_noise_high_key_track = 1.0f;
  REQUIRE(render_patch(corners_only) == render_patch(base));
  NativeSynthPatch with_osc2 = base;
  with_osc2.va.osc2_level = 0.5f;
  REQUIRE(render_patch(with_osc2) != render_patch(base));
}

TEST_CASE("NativeSynth subtractive section: osc2 sits at its interval, noise fills the gaps",
          "[midi][synth][va]") {
  using va_section_detail::hann_level;
  constexpr double kF0 = 440.0;  // note 69
  auto render_patch = [](const NativeSynthPatch& patch) {
    NativeSynthConfig cfg;
    cfg.patch = patch;
    NativeSynth synth(cfg);
    synth.prepare(kOutRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 127)));
    return render(synth, static_cast<int>(kOutRate)).left;
  };
  NativeSynthPatch base;
  base.waveform = VaWaveform::kSine;
  base.cutoff_hz = 20000.0f;
  base.amp_env.attack_ms = 1.0f;
  base.amp_env.sustain = 1.0f;
  const size_t from = static_cast<size_t>(0.2 * kOutRate);
  const size_t len = static_cast<size_t>(0.5 * kOutRate);

  NativeSynthPatch fifth = base;
  fifth.va.osc2_interval_cents = 702.0f;
  fifth.va.osc2_level = 1.0f;
  const auto dry = render_patch(base);
  const auto wet = render_patch(fifth);
  const double osc2_hz = kF0 * std::exp2(702.0 / sonare::constants::kCentsPerOctave);
  const double dry_line = hann_level(dry, kOutRate, osc2_hz, from, len);
  const double wet_line = hann_level(wet, kOutRate, osc2_hz, from, len);
  const double wet_f0 = hann_level(wet, kOutRate, kF0, from, len);
  // Equal levels: the fifth stands as tall as the fundamental, where the sine alone has nothing.
  CHECK(wet_line == Catch::Approx(wet_f0).epsilon(0.05));
  CHECK(dry_line < 1e-3 * wet_f0);

  NativeSynthPatch noisy = base;
  noisy.va.noise_level = 0.2f;
  const auto hiss = render_patch(noisy);
  // Mean level over bins between the fundamental and its octave.
  const auto floor = [&](const std::vector<float>& buf) {
    double sum = 0.0;
    for (int i = 0; i < 16; ++i) sum += hann_level(buf, kOutRate, 1.2 * kF0 + 8.0 * i, from, len);
    return sum / 16.0;
  };
  const double dry_floor = floor(dry);
  const double hiss_floor = floor(hiss);
  CHECK(hiss_floor > 100.0 * std::max(dry_floor, 1e-9));
  // The fundamental is still there under it.
  CHECK(hann_level(hiss, kOutRate, kF0, from, len) ==
        Catch::Approx(hann_level(dry, kOutRate, kF0, from, len)).epsilon(0.05));
}

TEST_CASE("NativeSynth subtractive section: amplitude noise skirts each partial at its own level",
          "[midi][synth][va]") {
  using va_section_detail::hann_level;
  constexpr double kF0 = 440.0;  // note 69
  auto render_patch = [](const NativeSynthPatch& patch) {
    NativeSynthConfig cfg;
    cfg.patch = patch;
    NativeSynth synth(cfg);
    synth.prepare(kOutRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 127)));
    return render(synth, static_cast<int>(kOutRate)).left;
  };
  NativeSynthPatch base;
  base.waveform = VaWaveform::kSine;
  base.cutoff_hz = 20000.0f;
  base.amp_env.attack_ms = 1.0f;
  base.amp_env.sustain = 1.0f;
  base.va.osc2_interval_cents = 1200.0f;
  base.va.osc2_level = 0.25f;
  NativeSynthPatch breathy = base;
  breathy.va.noise_am = 0.2f;
  const auto dry = render_patch(base);
  const auto wet = render_patch(breathy);
  const size_t from = static_cast<size_t>(0.2 * kOutRate);
  const size_t len = static_cast<size_t>(0.5 * kOutRate);
  const auto skirt = [&](const std::vector<float>& buf, double line_hz) {
    return hann_level(buf, kOutRate, line_hz + 8.0, from, len) /
           hann_level(buf, kOutRate, line_hz, from, len);
  };
  // Amplitude, not pitch: the octave's skirt is no wider relative to its line than f0's.
  CHECK(skirt(wet, kF0) > 10.0 * skirt(dry, kF0));
  CHECK(skirt(wet, 2.0 * kF0) == Catch::Approx(skirt(wet, kF0)).epsilon(0.5));
  // The gap between the partials stays empty.
  CHECK(hann_level(wet, kOutRate, 1.5 * kF0, from, len) <
        1e-3 * hann_level(wet, kOutRate, kF0, from, len));
}

TEST_CASE("NativeSynth subtractive section: band noise passes the closed filter, inside its band",
          "[midi][synth][va]") {
  using va_section_detail::hann_level;
  constexpr double kF0 = 440.0;  // note 69
  auto render_patch = [](const NativeSynthPatch& patch) {
    NativeSynthConfig cfg;
    cfg.patch = patch;
    NativeSynth synth(cfg);
    synth.prepare(kOutRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 127)));
    return render(synth, static_cast<int>(kOutRate)).left;
  };
  NativeSynthPatch base;
  base.waveform = VaWaveform::kSine;
  base.cutoff_hz = 20000.0f;
  base.amp_env.attack_ms = 1.0f;
  base.amp_env.sustain = 1.0f;
  NativeSynthPatch closed = base;
  closed.cutoff_hz = 60.0f;
  closed.key_track = 0.0f;
  NativeSynthPatch filtered_noise = closed;
  filtered_noise.va.noise_level = 0.2f;
  NativeSynthPatch banded = closed;
  banded.va.band_noise_level = 0.2f;
  banded.va.band_noise_low_hz = 300.0f;
  banded.va.band_noise_high_hz = 3000.0f;
  const size_t from = static_cast<size_t>(0.2 * kOutRate);
  const size_t len = static_cast<size_t>(0.5 * kOutRate);
  const auto band = [&](const std::vector<float>& buf, double lo_hz) {
    double sum = 0.0;
    for (int i = 0; i < 16; ++i) sum += hann_level(buf, kOutRate, lo_hz + 8.0 * i, from, len);
    return sum / 16.0;
  };
  const auto filtered = render_patch(filtered_noise);
  const auto wet = render_patch(banded);
  // Between the partials, where the closed filter leaves the same noise ahead of it little.
  const double gap = band(wet, 1.2 * kF0);
  CHECK(gap > 30.0 * std::max(band(filtered, 1.2 * kF0), 1e-9));
  // Below the highpass corner and above the lowpass corner the band falls away.
  CHECK(band(wet, 30.0) < 0.1 * gap);
  CHECK(band(wet, 12000.0) < 0.1 * gap);
  // Tracking the key moves the lowpass corner up an octave over the octave above middle C.
  NativeSynthPatch fixed_corner = banded;
  fixed_corner.va.band_noise_high_hz = 1000.0f;
  NativeSynthPatch tracked_corner = fixed_corner;
  tracked_corner.va.band_noise_high_key_track = 1.0f;
  CHECK(band(render_patch(tracked_corner), 1500.0) >
        2.0 * band(render_patch(fixed_corner), 1500.0));
  // Tracking the key moves the highpass corner up too, out of the band beneath it.
  NativeSynthPatch tracked_low = banded;
  tracked_low.va.band_noise_low_key_track = 1.0f;
  CHECK(band(render_patch(tracked_low), 150.0) < 0.6 * band(wet, 150.0));
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

TEST_CASE("A clean bowed-string render leaves the discard count at zero",
          "[midi][synth][non-finite]") {
  // Same engine and elasto-plastic path as the poisoned case below, differing
  // only in the stribeck value, so the two cases isolate that one field.
  NativeSynthConfig cfg;
  cfg.patch.mode = SynthEngineMode::kBowedString;
  cfg.patch.bowed_string.elasto_plastic = true;
  cfg.patch.bowed_string.stribeck = 0.7f;
  NativeSynth synth(cfg);
  synth.prepare(kOutRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 57, 110)));
  const StereoRender out = render(synth, 8192);
  REQUIRE(peak(out.left) > 0.0f);  // non-vacuity: a count of 0 has to mean something
  REQUIRE(synth.non_finite_discard_count() == 0u);
}

TEST_CASE(
    "A NaN stribeck value is replaced by its default before it can reach the elasto-plastic "
    "divisor",
    "[midi][synth][non-finite]") {
  // bowed_string.stribeck used to reach the render loop without ever passing
  // through clamp_synth_patch, so a non-finite value corrupted
  // BowedStringVoiceCore's bristle state on the very first sample
  // (elasto_plastic_injection() divides by it). clamp_synth_patch now
  // sanitizes it, so the constructor is where this is stopped: the corrupted
  // value never reaches the voice at all.
  NativeSynthConfig cfg;
  cfg.patch.mode = SynthEngineMode::kBowedString;
  cfg.patch.bowed_string.elasto_plastic = true;
  cfg.patch.bowed_string.stribeck = std::numeric_limits<float>::quiet_NaN();
  NativeSynth synth(cfg);
  REQUIRE(synth.patch().bowed_string.stribeck == 0.5f);  // bowed_string_voice.h's declared default

  synth.prepare(kOutRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 57, 110)));
  const StereoRender out = render(synth, 8192);
  for (float v : out.left) REQUIRE(std::isfinite(v));
  for (float v : out.right) REQUIRE(std::isfinite(v));
  REQUIRE(synth.non_finite_discard_count() == 0u);
}

TEST_CASE("clamp_synth_patch sanitizes every field a bare std::clamp leaves NaN-transparent",
          "[midi][synth][non-finite]") {
  // std::clamp(NaN, lo, hi) returns NaN (both its comparisons are false), so a
  // field clamped without patch_clamp_detail::sanitize first passes a NaN
  // through untouched. Fields with no clamp at all are equally open. Covers
  // both shapes: unclamped fields fall back to their declared struct default,
  // clamped fields fall back to their default and then land inside the
  // existing bounds.
  using sonare::midi::synth::clamp_synth_patch;
  using sonare::midi::synth::NativeSynthPatch;

  const float nan = std::numeric_limits<float>::quiet_NaN();
  NativeSynthPatch p;
  // Unclamped (Group A): no std::clamp at all guarded these before this fix.
  p.ks.body_coupling = nan;
  p.percussion.noise_burst_interval_ms = nan;
  p.pipe_organ.keytrack = nan;
  // Clamped but NaN-transparent (Group B): std::clamp alone let NaN through.
  p.amp_env.delay_ms = nan;
  p.filter_env.sustain = nan;
  p.piano.strike_position = nan;
  p.brass.lip_aperture = nan;
  p.brass.bell_cutoff_hz = nan;
  p.brass.bore_nonlinearity = nan;

  const NativeSynthPatch clamped = clamp_synth_patch(p);

  REQUIRE(clamped.ks.body_coupling == 0.0f);
  REQUIRE(clamped.percussion.noise_burst_interval_ms == 10.0f);
  REQUIRE(clamped.pipe_organ.keytrack == 0.0f);
  REQUIRE(clamped.amp_env.delay_ms == 0.0f);    // default 0, inside [0, 5000]
  REQUIRE(clamped.filter_env.sustain == 0.7f);  // default 0.7, inside [0, 1]
  // A fallback is the field's own struct default, read from the struct rather
  // than spelled again here: a second copy of the value is what lets the two
  // drift, and on a strike point the drift is audible.
  REQUIRE(clamped.piano.strike_position == sonare::midi::synth::PianoPatchParams{}.strike_position);
  // Same reading for the gated brass physics, where the default is also the
  // switch: a fallback that drifted off 0 would turn a mechanism on for a patch
  // that never asked for one, and only on the non-finite path, so nothing else
  // in the suite would be looking.
  const sonare::midi::synth::BrassPatchParams brass_defaults{};
  REQUIRE(clamped.brass.lip_aperture == brass_defaults.lip_aperture);
  REQUIRE(clamped.brass.bell_cutoff_hz == brass_defaults.bell_cutoff_hz);
  REQUIRE(clamped.brass.bore_nonlinearity == brass_defaults.bore_nonlinearity);
  REQUIRE(brass_defaults.lip_aperture == 0.0f);
  REQUIRE(brass_defaults.bell_cutoff_hz == 0.0f);
  REQUIRE(brass_defaults.bore_nonlinearity == 0.0f);
}

TEST_CASE("physical-model GM programs route to their waveguide engines", "[midi][synth]") {
  using sonare::midi::synth::gm_fallback_patch;
  // Harpsichord (GM 6) voices its own jack-and-plectrum engine; the clavinet (7)
  // is a tangent-struck string read by a pickup, on the plucked-string loop.
  REQUIRE(gm_fallback_patch(0, 6).mode == SynthEngineMode::kHarpsichord);    // Harpsichord
  REQUIRE(gm_fallback_patch(0, 7).mode == SynthEngineMode::kKarplusStrong);  // Clavi
  // Bowed string family (GM 40-43) now voices the friction waveguide.
  REQUIRE(gm_fallback_patch(0, 40).mode == SynthEngineMode::kBowedString);  // Violin
  REQUIRE(gm_fallback_patch(0, 43).mode == SynthEngineMode::kBowedString);  // Contrabass
  // Brass family (GM 56-61); SynthBrass (62-63) stays FM.
  REQUIRE(gm_fallback_patch(0, 56).mode == SynthEngineMode::kBrass);  // Trumpet
  REQUIRE(gm_fallback_patch(0, 60).mode == SynthEngineMode::kBrass);  // French Horn
  REQUIRE(gm_fallback_patch(0, 61).mode == SynthEngineMode::kBrass);  // Brass Section
  REQUIRE(gm_fallback_patch(0, 62).mode == SynthEngineMode::kFm);     // Synth Brass 1
  // String Ensemble 1/2 (GM 48-49) are the bowed waveguide in section; the two
  // Synth Strings above them are subtractive stacks, each with its own patch.
  REQUIRE(gm_fallback_patch(0, 48).mode == SynthEngineMode::kBowedString);
  REQUIRE(gm_fallback_patch(0, 49).mode == SynthEngineMode::kBowedString);
  REQUIRE(gm_fallback_patch(0, 50).mode == SynthEngineMode::kSubtractive);
  REQUIRE(gm_fallback_patch(0, 51).mode == SynthEngineMode::kSubtractive);
  // The pair is voiced apart: the wider stack is the second of them.
  REQUIRE(gm_fallback_patch(0, 51).detune_cents > gm_fallback_patch(0, 50).detune_cents);
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
      0,   1,   2,   3,   6,   7,              // piano + harpsichord + clavi
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

TEST_CASE("NativeSynth preserves the bank-1 harpsichord's undamped 4' tail",
          "[midi][synth][harpsichord]") {
  NativeSynthConfig cfg;
  cfg.use_gm_programs = true;
  cfg.gain = 1.0f;
  cfg.dc_block = false;
  NativeSynth synth(cfg);
  synth.prepare(kOutRate, 256);

  // GM2/GS bank 1, program 6 is the octave registration. At note 89 its 4'
  // strings are above the damper break and must continue after key-off.
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 0, 121)));
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 32, 1)));
  synth.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 6)));
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 89, 110)));
  REQUIRE(peak(render(synth, 12000).left) > 1.0e-4f);
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 89, 0)));
  const StereoRender tail = render(synth, 72000);
  const float late_peak = peak(tail.left, 48000);
  INFO("late undamped 4' peak " << late_peak);
  REQUIRE(late_peak > 1.0e-5f);
  REQUIRE(synth.active_voice_count() > 0);
}

TEST_CASE("harpsichord tail helper bounds native and GM metadata", "[midi][synth][harpsichord]") {
  NativeSynthPatch custom;
  custom.mode = SynthEngineMode::kHarpsichord;
  custom.harpsichord.decay_s = 0.05f;
  custom.harpsichord.decay_stretch = 0.0f;
  custom.harpsichord.eight_a = false;
  custom.harpsichord.four = true;
  custom.harpsichord.undamped_from_note = 84;
  custom.harpsichord.rear_segment_mm = 0.0f;
  custom.harpsichord.board_diffuse_db = -120.0f;

  const double custom_seconds =
      sonare::midi::synth::harpsichord_max_release_tail_seconds(custom.harpsichord);
  const int64_t custom_helper_samples = static_cast<int64_t>(std::ceil(custom_seconds * kOutRate));
  const int64_t custom_bound =
      sonare::midi::synth::native_patch_tail_samples(custom, kOutRate, {}, nullptr);
  INFO("custom helper " << custom_helper_samples << ", native bound " << custom_bound);
  REQUIRE(std::isfinite(custom_seconds));
  REQUIRE(custom_bound < std::numeric_limits<int64_t>::max());
  REQUIRE(custom_bound >= custom_helper_samples);

  const NativeSynthPatch& gm_octave = sonare::midi::synth::gm_fallback_patch(1, 6);
  REQUIRE(gm_octave.mode == SynthEngineMode::kHarpsichord);
  const double gm_seconds =
      sonare::midi::synth::harpsichord_max_release_tail_seconds(gm_octave.harpsichord);
  const int64_t gm_helper_samples = static_cast<int64_t>(std::ceil(gm_seconds * kOutRate));
  const int64_t gm_bound =
      sonare::midi::synth::gm_fallback_max_tail_samples(kOutRate, 1.0f, 1.0f, 1.0f);
  INFO("GM helper " << gm_helper_samples << ", GM bound " << gm_bound);
  REQUIRE(std::isfinite(gm_seconds));
  REQUIRE(gm_bound >= gm_helper_samples);
}

TEST_CASE("NativeSynthVoice choke_fast overrides an idle harpsichord tail",
          "[midi][synth][harpsichord]") {
  NativeSynthPatch patch;
  patch.mode = SynthEngineMode::kHarpsichord;
  patch.amp_env.sustain = 1.0f;
  patch.amp_env.release_ms = 1.0f;
  patch.harpsichord.decay_s = 0.05f;
  patch.harpsichord.decay_stretch = 0.0f;
  patch.harpsichord.eight_a = false;
  patch.harpsichord.four = true;
  patch.harpsichord.undamped_from_note = 84;
  patch.harpsichord.rear_segment_mm = 0.0f;
  patch.harpsichord.board_diffuse_db = -120.0f;

  NativeSynthVoice voice;
  voice.active = true;
  voice.note = 89;
  voice.channel = 0;
  std::vector<float> slab(static_cast<size_t>(harpsichord_slab_capacity(kOutRate)), 0.0f);
  voice.harpsichord.attach(slab.data(), harpsichord_buffer_capacity(kOutRate));
  voice.start(patch, kOutRate, sonare::midi::Velocity16::from7(110), 0);

  Sf2ChannelMod mod;
  for (int i = 0; i < 2048; ++i) static_cast<void>(voice.render(mod));
  voice.release();
  // The ordinary amp envelope is already idle, while the undamped 4' string
  // still has a live tail that the voice must keep rendering.
  for (int i = 0; i < 2048; ++i) static_cast<void>(voice.render(mod));
  REQUIRE(voice.active);

  voice.choke_fast(kOutRate);
  // A late choke must fade from the physical tail's current level, even
  // though the ordinary amp envelope is idle. Cutting its latch in one
  // sample would click instead of applying the replacement fade.
  float fade_peak = 0.0f;
  for (int i = 0; i < 128; ++i) {
    fade_peak = std::max(fade_peak, std::abs(voice.render(mod)));
  }
  REQUIRE(fade_peak > 1.0e-6f);
  for (int i = 0; i < static_cast<int>(0.02 * kOutRate) && voice.active; ++i) {
    static_cast<void>(voice.render(mod));
  }
  REQUIRE_FALSE(voice.active);
}

TEST_CASE("NativeSynth harpsichord short tails end after the reported bound",
          "[midi][synth][harpsichord]") {
  NativeSynthConfig cfg;
  cfg.gain = 1.0f;
  cfg.dc_block = false;
  cfg.patch.mode = SynthEngineMode::kHarpsichord;
  cfg.patch.amp_env.sustain = 1.0f;
  cfg.patch.amp_env.release_ms = 1.0f;
  cfg.patch.harpsichord.decay_s = 0.05f;
  cfg.patch.harpsichord.decay_stretch = 0.0f;
  cfg.patch.harpsichord.eight_a = false;
  cfg.patch.harpsichord.four = true;
  cfg.patch.harpsichord.undamped_from_note = 84;
  cfg.patch.harpsichord.rear_segment_mm = 0.0f;
  cfg.patch.harpsichord.board_diffuse_db = -120.0f;

  NativeSynth synth(cfg);
  synth.prepare(kOutRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 89, 110)));
  REQUIRE(peak(render(synth, 4096).left) > 1.0e-4f);
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 89, 0)));

  const int tail = synth.tail_samples();
  REQUIRE(tail > 0);
  render(synth, tail + 256);
  REQUIRE(synth.active_voice_count() == 0);
}

TEST_CASE("NativeSynth harpsichord kill and mono choke end tails early",
          "[midi][synth][harpsichord]") {
  NativeSynthConfig cfg;
  cfg.use_gm_programs = true;
  cfg.gain = 1.0f;
  cfg.dc_block = false;
  NativeSynth synth(cfg);
  synth.prepare(kOutRate, 256);

  auto select_harpsichord = [&](uint8_t bank_lsb) {
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 0, 121)));
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 32, bank_lsb)));
    synth.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 6)));
  };

  select_harpsichord(0);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 89, 110)));
  REQUIRE(peak(render(synth, 2048).left) > 1.0e-4f);
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 120, 0)));
  REQUIRE(synth.active_voice_count() == 0);

  synth.set_articulation(0, sonare::midi::ArticulationMode::kMonoLegato);
  select_harpsichord(0);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 89, 110)));
  render(synth, 2048);
  select_harpsichord(1);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 89, 110)));
  REQUIRE(synth.active_voice_count() == 2);
  render(synth, static_cast<int>(0.02 * kOutRate));
  // choke_fast() is a five-ms replacement fade, even when the harpsichord's
  // ordinary undamped tail is much longer.
  REQUIRE(synth.active_voice_count() == 1);
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
  // Four sustained wind parts (flute / clarinet / trumpet / oboe). `only`
  // sounds one of them and silences the rest, because the residual has to be
  // read per part as well as on the mix: the four post-blocker residuals partly
  // cancel, so a ratio taken on the sum reports that cancellation rather than
  // the blocker's work. Measured on the mix, silencing the trumpet moves the
  // residual 2.2e-05 -> 9.8e-05 — the sum is smaller than its own terms, and it
  // is the quietest part that was holding the number down.
  const uint8_t programs[4] = {73, 71, 56, 68};
  auto mean_offset = [&](const bool* block, int only) {
    Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
    if (block != nullptr) cfg.dc_block = *block;
    Sf2Player player(cfg);  // no set_soundfont -> fallback floor
    player.prepare(kOutRate, 256);
    for (uint8_t part = 0; part < 4; ++part) {
      if (only >= 0 && part != only) continue;
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
  const double unblocked = mean_offset(&off, -1);
  const double blocked = mean_offset(&on, -1);
  const double by_default = mean_offset(nullptr, -1);
  INFO("dc offset unblocked " << unblocked << " blocked " << blocked << " default " << by_default);
  REQUIRE(unblocked > 1.0e-3);  // the offset the blocker exists to remove
  REQUIRE(blocked < 1.0e-3);
  REQUIRE(by_default == blocked);  // the blocker is on unless a host opts out

  // Per part, which is the statement a sum cannot make. The bound is absolute
  // rather than a fraction of the part's own offset: only the two reed voices
  // carry an offset worth a ratio (clarinet 3.8e-03, oboe over 1e-03), while
  // the flute leaves 9.1e-06 and the trumpet 6.7e-05 before the blocker runs,
  // so a ratio on those two would be scored on the blocker's own settling.
  for (int part = 0; part < 4; ++part) {
    const double one = mean_offset(&on, part);
    INFO("program " << static_cast<int>(programs[part]) << " alone leaves " << one);
    REQUIRE(one < 1.0e-3);
  }
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

TEST_CASE("re-engaging the voice filter after a skip does not replay stale state",
          "[midi][synth]") {
  using sonare::midi::synth::ModDestination;
  using sonare::midi::synth::ModSource;
  using sonare::midi::synth::NativeSynthParamId;
  constexpr int kBlock = 256;
  constexpr int kBlocks = 40;
  constexpr float kControlCutoff = 17999.0f;  // just under the skip threshold

  // Renders a held sine A4 block by block; `act` runs before each block.
  const auto held = [&](float cutoff, bool wheel_route, auto&& act) {
    NativeSynthConfig cfg;
    cfg.patch.waveform = VaWaveform::kSine;
    cfg.patch.cutoff_hz = cutoff;
    cfg.patch.gain = 0.8f;
    cfg.patch.amp_env.attack_ms = 1.0f;
    cfg.patch.amp_env.sustain = 1.0f;
    if (wheel_route) {
      cfg.patch.mod_matrix.routes[0] = {ModSource::kModWheel, ModDestination::kCutoffCents,
                                        -3600.0f};
    }
    NativeSynth synth(cfg);
    synth.prepare(kOutRate, kBlock);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 100)));
    std::vector<float> out;
    for (int b = 0; b < kBlocks; ++b) {
      act(synth, b);
      const StereoRender blk = render(synth, kBlock);
      out.insert(out.end(), blk.left.begin(), blk.left.end());
    }
    return out;
  };
  // Largest adjacent-sample step once the attack and DC blocker have settled.
  const auto max_step = [](const std::vector<float>& x) {
    float m = 0.0f;
    for (size_t i = 2049; i < x.size(); ++i) m = std::max(m, std::fabs(x[i] - x[i - 1]));
    return m;
  };
  const auto cutoff_id = static_cast<unsigned int>(NativeSynthParamId::kCutoffHz);

  // Cutoff automation 12 kHz -> 20 kHz (skipped) -> 12 kHz.
  const auto sweep = [&](float base) {
    return held(base, false, [&](NativeSynth& s, int b) {
      if (b == 0) s.apply_parameter(cutoff_id, 12000.0f);
      if (b == 14) s.apply_parameter(cutoff_id, 20000.0f);
      if (b == 28) s.apply_parameter(cutoff_id, 12000.0f);
    });
  };
  const float control_sweep = max_step(held(kControlCutoff, false, [&](NativeSynth& s, int b) {
    if (b == 0) s.apply_parameter(cutoff_id, 12000.0f);
    if (b == 14) s.apply_parameter(cutoff_id, kControlCutoff);
    if (b == 28) s.apply_parameter(cutoff_id, 12000.0f);
  }));
  REQUIRE(control_sweep > 0.0f);
  REQUIRE(max_step(sweep(20000.0f)) <= 1.5f * control_sweep);

  // Mod wheel routed to cutoff: 127 -> 0 (cutoff 20 kHz, skipped) -> 127.
  const auto wheel = [&](float base) {
    return max_step(held(base, true, [&](NativeSynth& s, int b) {
      const int v = (b >= 14 && b < 28) ? 0 : 127;
      if (b == 0 || b == 14 || b == 28) {
        s.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 1, v)));
      }
    }));
  };
  const float control_wheel = wheel(kControlCutoff);
  REQUIRE(control_wheel > 0.0f);
  REQUIRE(wheel(20000.0f) <= 1.5f * control_wheel);
}

TEST_CASE("NativeSynth GM church organ keeps the configured wind response",
          "[midi][synth][organ]") {
  using sonare::midi::synth::gm_fallback_patch;
  const NativeSynthPatch& church_organ = gm_fallback_patch(0, 19);
  REQUIRE(church_organ.mode == SynthEngineMode::kPipeOrgan);

  const auto play = [&](bool gm) {
    NativeSynthConfig cfg;
    cfg.gain = 1.0f;
    cfg.dc_block = false;
    cfg.use_gm_programs = gm;
    if (!gm) cfg.patch = church_organ;
    NativeSynth synth(cfg);
    synth.prepare(kOutRate, 256);
    if (gm) {
      synth.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 19)));
    }
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 110)));
    return render(synth, 24000);
  };

  const StereoRender gm = play(true);
  const StereoRender configured = play(false);
  REQUIRE(rms(gm.left, 4096) > 0.001f);
  REQUIRE(rms(configured.left, 4096) > 0.001f);
  REQUIRE(gm.left == configured.left);
  REQUIRE(gm.right == configured.right);
}

TEST_CASE("NativeSynth organ swell is isolated per channel", "[midi][synth][organ]") {
  NativeSynthConfig cfg;
  cfg.gain = 1.0f;
  cfg.dc_block = false;
  cfg.patch.mode = SynthEngineMode::kPipeOrgan;
  cfg.patch.pipe_organ.swell = 0.7f;
  cfg.patch.pipe_organ.tremulant_rate_hz = 0.0f;
  cfg.patch.pipe_organ.wind_sag = 0.0f;
  cfg.patch.amp_env.attack_ms = 2.0f;
  cfg.patch.amp_env.sustain = 1.0f;

  const auto render_parts = [&](uint8_t other_expression) {
    NativeSynth synth(cfg);
    synth.prepare(kOutRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 11, 127)));
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 1, 11, other_expression)));
    MidiEvent first = event(sonare::midi::make_midi1_note_on(0, 0, 60, 110));
    MidiEvent second = event(sonare::midi::make_midi1_note_on(0, 1, 67, 110));
    first.source_track_id = 1;
    second.source_track_id = 2;
    synth.on_event(0, first);
    synth.on_event(0, second);
    std::vector<float> fallback_l(8192, 0.0f), fallback_r(8192, 0.0f);
    std::vector<float> first_l(8192, 0.0f), first_r(8192, 0.0f);
    std::vector<float> second_l(8192, 0.0f), second_r(8192, 0.0f);
    float* fallback[] = {fallback_l.data(), fallback_r.data()};
    float* first_lane[] = {first_l.data(), first_r.data()};
    float* second_lane[] = {second_l.data(), second_r.data()};
    const MidiInstrumentSourceOutput outputs[] = {{0, fallback}, {1, first_lane}, {2, second_lane}};
    REQUIRE(synth.process_source_tracks(outputs, 3, 2, 8192));
    return std::pair<std::vector<float>, std::vector<float>>{std::move(first_l),
                                                             std::move(first_r)};
  };

  const auto open = render_parts(127);
  const auto unrelated_closed = render_parts(0);
  REQUIRE(open.first == unrelated_closed.first);
  REQUIRE(open.second == unrelated_closed.second);
}

TEST_CASE("NativeSynth organ pressure recovers while its part is silent",
          "[midi][synth][organ][gs-physical-review]") {
  NativeSynthConfig cfg;
  cfg.gain = 1.0f;
  cfg.dc_block = false;
  cfg.patch = sonare::midi::synth::gm_fallback_patch(0, 19);
  cfg.patch.retrigger = sonare::midi::synth::SynthRetrigger::kNote;
  cfg.patch.amp_env.release_ms = 1.0f;
  cfg.patch.pipe_organ.wind_sag = 0.9f;
  cfg.patch.pipe_organ.tremulant_rate_hz = 0.0f;
  const auto onset = [&](bool chord, int pause) {
    NativeSynth synth(cfg);
    synth.prepare(kOutRate, 256);
    if (chord) {
      for (const uint8_t note : {uint8_t{48}, uint8_t{52}, uint8_t{55}, uint8_t{60}}) {
        synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, 110)));
      }
      render(synth, 4096);
      synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 123, 0)));
      for (int i = 0; i < 4096 && synth.active_voice_count() != 0; ++i) render(synth, 256);
      REQUIRE(synth.active_voice_count() == 0);
      render(synth, pause);
    }
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 67, 110)));
    return render(synth, 2048);
  };
  const auto fresh = onset(false, 0);
  const auto immediate = onset(true, 0);
  const auto recovered = onset(true, 96000);
  REQUIRE(rms(fresh.left, 0) > 1e-6f);
  double immediate_error = 0.0, recovered_error = 0.0;
  for (size_t i = 0; i < fresh.left.size(); ++i) {
    const double a = immediate.left[i] - fresh.left[i];
    const double b = recovered.left[i] - fresh.left[i];
    immediate_error += a * a;
    recovered_error += b * b;
  }
  CAPTURE(immediate_error, recovered_error);
  REQUIRE(immediate_error > 1e-8);
  CHECK(recovered_error < immediate_error * 0.1);
}

TEST_CASE("NativeSynth wind sag follows sounding rank count, not voice count",
          "[midi][synth][organ]") {
  const auto make_patch = [](int rank_count, float wind_sag, bool sparse) {
    NativeSynthPatch patch;
    patch.mode = SynthEngineMode::kPipeOrgan;
    patch.gain = 1.0f;
    patch.cutoff_hz = 20000.0f;
    patch.amp_env.attack_ms = 1.0f;
    patch.amp_env.sustain = 1.0f;
    patch.amp_env.release_ms = 100.0f;
    patch.pipe_organ.breath = 0.9f;
    patch.pipe_organ.chiff = 0.0f;
    patch.pipe_organ.tone_decay_s = 8.0f;
    patch.pipe_organ.wind_sag = wind_sag;
    patch.pipe_organ.rank_count = rank_count;
    for (int r = 0; r < rank_count; ++r) {
      patch.pipe_organ.ranks[static_cast<size_t>(r)] = {
          1.0f, false, 0.6f, sparse && r > 0 ? 0.0f : 1.0f, 0.0f, 0.0f};
    }
    return patch;
  };
  const auto settled_rms = [](const NativeSynthPatch& patch) {
    NativeSynthConfig cfg;
    cfg.patch = patch;
    cfg.gain = 1.0f;
    cfg.dc_block = false;
    NativeSynth synth(cfg);
    synth.prepare(kOutRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 110)));
    return rms(render(synth, 48000).left, 24000);
  };

  const float one_dry = settled_rms(make_patch(1, 0.0f, false));
  const float one_sag = settled_rms(make_patch(1, 0.9f, false));
  const float six_dry = settled_rms(make_patch(6, 0.0f, false));
  const float six_sag = settled_rms(make_patch(6, 0.9f, false));
  const float sparse_sag = settled_rms(make_patch(6, 0.9f, true));
  REQUIRE(one_dry > 1.0e-4f);
  REQUIRE(six_dry > 1.0e-4f);
  const float one_ratio = one_sag / one_dry;
  const float six_ratio = six_sag / six_dry;
  const float sparse_ratio = sparse_sag / one_dry;
  INFO("one ratio " << one_ratio << ", six ratio " << six_ratio << ", sparse ratio "
                    << sparse_ratio);

  // Six drawn ranks consume substantially more wind than one rank. The zero
  // level slots in the same registration are silent pipes and must not add a
  // second load of their own.
  REQUIRE(six_ratio < one_ratio - 0.15f);
  CHECK(std::fabs(sparse_ratio - one_ratio) < 0.08f);

  // A regulated supply restores the set pressure, so the same six-rank draw
  // settles at the dry level.
  NativeSynthPatch regulated = make_patch(6, 0.9f, false);
  regulated.pipe_organ.wind_regulated = true;
  const float regulated_ratio = settled_rms(regulated) / six_dry;
  INFO("regulated ratio " << regulated_ratio);
  CHECK(std::fabs(regulated_ratio - 1.0f) < 0.01f);
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

TEST_CASE("custom sustained one-shot tails are bounded by what ends the voice",
          "[midi][synth][tail-physical]") {
  auto check_sustained = [](NativeSynthConfig cfg) {
    NativeSynth synth(cfg);
    synth.prepare(kOutRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 110)));
    const StereoRender onset = render(synth, 32768);
    CHECK(peak(onset.left) > 1.0e-4f);
    REQUIRE(synth.active_voice_count() == 1);
    synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
    const StereoRender after_note_off = render(synth, 256);
    CHECK(peak(after_note_off.left) > 1.0e-4f);
    CHECK(synth.active_voice_count() == 1);
    return static_cast<int64_t>(synth.tail_samples());
  };

  SECTION("bowed string") {
    NativeSynthConfig cfg;
    cfg.patch = sonare::midi::synth::gm_fallback_patch(0, 40);
    cfg.patch.one_shot = true;
    cfg.patch.amp_env.attack_ms = 1.0f;
    cfg.patch.amp_env.sustain = 1.0f;
    cfg.patch.amp_env.release_ms = 1.0f;
    // Nothing in a bowed string ends a held envelope.
    CHECK(check_sustained(cfg) == std::numeric_limits<int>::max());
  }

  SECTION("percussion") {
    NativeSynthConfig cfg;
    cfg.patch.mode = SynthEngineMode::kPercussion;
    cfg.patch.one_shot = true;
    cfg.patch.amp_env = {0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f};
    cfg.patch.percussion.gm_kit = false;
    cfg.patch.percussion.num_modes = 1;
    cfg.patch.percussion.base_freq_hz = 220.0f;
    cfg.patch.percussion.mode_decay_s = 30.0f;
    cfg.patch.percussion.tone_gain = 1.0f;
    // The piece ends when it falls silent: a 30 s t60 reaches the -100 dB floor at 50 s.
    const int64_t tail = check_sustained(cfg);
    CHECK(tail < std::numeric_limits<int>::max());
    CHECK(tail >= static_cast<int64_t>(kOutRate * 30.0 * 100.0 / 60.0));
  }
}

TEST_CASE("custom FM and GM fallback physical tails remain finite",
          "[midi][synth][tail-physical]") {
  NativeSynthConfig fm_cfg;
  fm_cfg.patch.mode = SynthEngineMode::kFm;
  fm_cfg.patch.one_shot = true;
  fm_cfg.patch.amp_env.sustain = 1.0f;
  fm_cfg.patch.amp_env.release_ms = 1.0f;
  NativeSynth fm(fm_cfg);
  fm.prepare(kOutRate, 256);
  CHECK(fm.tail_samples() < std::numeric_limits<int>::max());

  NativeSynthConfig gm_cfg;
  gm_cfg.use_gm_programs = true;
  gm_cfg.patch.mode = SynthEngineMode::kBowedString;
  gm_cfg.patch.one_shot = true;
  gm_cfg.patch.amp_env.sustain = 1.0f;
  gm_cfg.patch.amp_env.release_ms = 1.0f;
  NativeSynth gm(gm_cfg);
  gm.prepare(kOutRate, 256);
  CHECK(gm.tail_samples() < std::numeric_limits<int>::max());
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

TEST_CASE("the tuning field table reaches every gated brass physics field",
          "[midi][synth][tuning]") {
  // Each of these is off at 0 and carries a whole mechanism behind it, so one
  // missing from the table leaves its mechanism at the default forever: the fit
  // cannot address it and reports nothing about the restriction. Named one by
  // one rather than counted, so adding a field cannot quietly satisfy this.
  using sonare::midi::synth::NativeSynthPatch;
  using sonare::midi::synth::patch_tuning_field_paths;

  NativeSynthPatch p;
  p.mode = SynthEngineMode::kBrass;
  const std::vector<std::string> paths = patch_tuning_field_paths(p);
  const auto has = [&paths](const char* path) {
    return std::find(paths.begin(), paths.end(), path) != paths.end();
  };
  REQUIRE(has("brass.lip_aperture"));
  REQUIRE(has("brass.bell_cutoff_hz"));
  REQUIRE(has("brass.bore_nonlinearity"));
  REQUIRE(has("brass.bell_radiation_hz"));
  REQUIRE(has("brass.brassiness"));
  REQUIRE(has("brass.cuivre_dynamics"));
  REQUIRE(has("brass.mute"));
  REQUIRE(has("brass.half_valve"));
  REQUIRE(has("brass.dynamic_lip"));
  // Negative control: without one, a lookup that answered yes to everything
  // would satisfy every line above and read exactly like a registered table.
  REQUIRE_FALSE(has("brass.not_a_field"));
}

TEST_CASE("the tuning field table reaches the switch beside every field it gates",
          "[midi][synth][tuning]") {
  // A switch left out of the table makes every field it gates read inert, and
  // a 2n+1 probe cannot see the pair because it holds the switch fixed. The
  // church organ's `pipe_organ.brightness` is dead at every value because the
  // flat voicing fields are read only at rank_count 0, and the harpsichord's
  // `velocity_droop_db` because the droop shapes the response past
  // `peak_velocity`, which ships at the top of the range with nothing past it.
  using sonare::midi::synth::NativeSynthPatch;
  using sonare::midi::synth::patch_tuning_field_paths;

  struct Gate {
    SynthEngineMode mode;
    const char* gate;
    const char* gated;
  };
  static const Gate kGates[] = {
      {SynthEngineMode::kPipeOrgan, "pipe_organ.rank_count", "pipe_organ.brightness"},
      {SynthEngineMode::kPipeOrgan, "pipe_organ.ranks0.stopped", "pipe_organ.ranks0.brightness"},
      {SynthEngineMode::kHarpsichord, "harpsichord.peak_velocity", "harpsichord.velocity_droop_db"},
      {SynthEngineMode::kHarpsichord, "harpsichord.four", "harpsichord.pluck_4"},
      {SynthEngineMode::kPiano, "piano.strings", "piano.detune_cents"},
      {SynthEngineMode::kBowedString, "bowed_string.elasto_plastic", "bowed_string.stribeck"},
      {SynthEngineMode::kBowedString, "bowed_string.hyperbolic_friction",
       "bowed_string.string_tension_n"},
      {SynthEngineMode::kBowedString, "bowed_string.hyperbolic_friction",
       "bowed_string.force_ratio"},
      {SynthEngineMode::kReed, "reed.dynamic_reed", "reed.closing_pressure"},
      {SynthEngineMode::kReed, "reed.conical", "reed.cone_growth"},
      {SynthEngineMode::kBrass, "brass.conical", "brass.brightness"},
      {SynthEngineMode::kFlute, "flute.jet_drive", "flute.jet_offset"},
      {SynthEngineMode::kVocal, "vocal.vowel", "vocal.brightness"},
      {SynthEngineMode::kPercussion, "percussion.gm_kit", "percussion.num_modes"},
      {SynthEngineMode::kFm, "fm.algorithm", "fm.ops1.level"},
      // The two every engine carries: the filter's output tap and whether a
      // note-off is heard at all.
      {SynthEngineMode::kSubtractive, "filter_output", "cutoff_hz"},
      {SynthEngineMode::kSubtractive, "one_shot", "amp_env.release_ms"},
  };
  for (const Gate& g : kGates) {
    NativeSynthPatch p;
    p.mode = g.mode;
    const std::vector<std::string> paths = patch_tuning_field_paths(p);
    const auto reaches = [&paths](const char* path) {
      return std::find(paths.begin(), paths.end(), path) != paths.end();
    };
    INFO(g.gate << " gating " << g.gated);
    REQUIRE(reaches(g.gate));
    REQUIRE(reaches(g.gated));
  }
}

#endif  // SONARE_TUNING

namespace {

using sonare::midi::synth::apply_engine_params;
using sonare::midi::synth::engine_param_descriptors;
using sonare::midi::synth::EngineParam;
using sonare::midi::synth::EngineParamDescriptor;
using EngineUnit = sonare::mastering::api::detail::Unit;

// A default patch on each engine mode, in ordinal order.
std::vector<NativeSynthPatch> one_patch_per_engine_mode() {
  std::vector<NativeSynthPatch> out;
  for (int m = 0; m <= sonare::midi::synth::kSynthEngineModeMax; ++m) {
    NativeSynthPatch p;
    p.mode = static_cast<SynthEngineMode>(m);
    out.push_back(sonare::midi::synth::clamp_synth_patch(p));
  }
  return out;
}

// The message's number spelling: whole numbers exactly, the rest in %g.
std::string printed(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), value == std::floor(value) ? "%.0f" : "%g", value);
  return buffer;
}

// The bytes of @p patch, padding included, for a bit-identity comparison.
std::vector<unsigned char> patch_bytes(const NativeSynthPatch& patch) {
  std::vector<unsigned char> bytes(sizeof(NativeSynthPatch));
  std::memcpy(bytes.data(), &patch, sizeof(NativeSynthPatch));
  return bytes;
}

const EngineParamDescriptor* descriptor_named(const std::vector<EngineParamDescriptor>& ds,
                                              const std::string& key) {
  for (const EngineParamDescriptor& d : ds) {
    if (d.key == key) return &d;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("engine params reach every engine-section field and read back through the walker",
          "[midi][synth]") {
  for (const NativeSynthPatch& base : one_patch_per_engine_mode()) {
    INFO("engine mode " << static_cast<int>(base.mode));
    const std::vector<EngineParamDescriptor> before = engine_param_descriptors(base);
    if (base.mode == SynthEngineMode::kSample) {
      REQUIRE(before.empty());
      continue;
    }
    REQUIRE_FALSE(before.empty());

    // A distinct in-bound value per field, different from the field's current one,
    // so a key landing on the wrong member reads back wrong.
    std::vector<double> wanted;
    for (size_t i = 0; i < before.size(); ++i) {
      const EngineParamDescriptor& d = before[i];
      double v = 0.0;
      if (d.boolean) {
        v = d.value == 0.0f ? 1.0 : 0.0;
      } else if (d.integer) {
        v = !d.bounded || d.value + 1.0f <= d.hi ? d.value + 1.0 : d.value - 1.0;
      } else if (d.bounded) {
        double t = 0.1 + 0.8 * std::fmod(0.6180339887 * static_cast<double>(i + 1), 1.0);
        v = d.lo + (d.hi - d.lo) * t;
        if (static_cast<float>(v) == d.value)
          v = d.lo + (d.hi - d.lo) * (t < 0.5 ? t + 0.25 : t - 0.25);
      } else {
        v = d.value + 0.25 + 0.001 * static_cast<double>(i);
      }
      wanted.push_back(v);
    }
    std::vector<EngineParam> params;
    for (size_t i = 0; i < before.size(); ++i) params.push_back({before[i].key.c_str(), wanted[i]});

    NativeSynthPatch patch = base;
    std::string error;
    const bool applied = apply_engine_params(patch, params.data(), params.size(), &error);
    INFO(error);
    REQUIRE(applied);
    REQUIRE(error.empty());

    const std::vector<EngineParamDescriptor> after = engine_param_descriptors(patch);
    // In bounds means the clamp a render applies leaves the value where it was set.
    const std::vector<EngineParamDescriptor> clamped =
        engine_param_descriptors(sonare::midi::synth::clamp_synth_patch(patch));
    REQUIRE(after.size() == before.size());
    REQUIRE(clamped.size() == before.size());
    for (size_t i = 0; i < before.size(); ++i) {
      INFO(before[i].key << " = " << wanted[i]);
      REQUIRE(after[i].key == before[i].key);
      REQUIRE(after[i].value == static_cast<float>(wanted[i]));
      REQUIRE(after[i].value != before[i].value);
      REQUIRE(clamped[i].value == after[i].value);
    }
  }
}

TEST_CASE("engine param keys drop the section and camelCase each segment", "[midi][synth]") {
  for (const NativeSynthPatch& base : one_patch_per_engine_mode()) {
    INFO("engine mode " << static_cast<int>(base.mode));
    std::set<std::string> keys;
    const std::vector<EngineParamDescriptor> ds = engine_param_descriptors(base);
    for (const EngineParamDescriptor& d : ds) {
      INFO(d.key);
      REQUIRE_FALSE(d.key.empty());
      REQUIRE(d.key.find('_') == std::string::npos);
      REQUIRE(keys.insert(d.key).second);
    }
  }

  const auto keys_of = [](SynthEngineMode mode) {
    NativeSynthPatch p;
    p.mode = mode;
    std::set<std::string> keys;
    for (const EngineParamDescriptor& d : engine_param_descriptors(p)) keys.insert(d.key);
    return keys;
  };
  // Karplus-Strong's section is `ks`, not the mode's name, and is stripped all the same.
  const std::set<std::string> ks = keys_of(SynthEngineMode::kKarplusStrong);
  REQUIRE(ks.count("decayS") == 1);
  REQUIRE(ks.count("hfDecayS") == 1);
  for (const std::string& key : ks) REQUIRE(key.rfind("ks", 0) != 0);
  REQUIRE(keys_of(SynthEngineMode::kBowedString).count("bowForce") == 1);
  REQUIRE(keys_of(SynthEngineMode::kPipeOrgan).count("ranks2.level") == 1);
  REQUIRE(keys_of(SynthEngineMode::kFm).count("ops1.level") == 1);
  REQUIRE(keys_of(SynthEngineMode::kFm).count("ops0.env.attackMs") == 1);
  REQUIRE(keys_of(SynthEngineMode::kHarpsichord).count("pluck4") == 1);
  REQUIRE(keys_of(SynthEngineMode::kHarpsichord).count("eightA") == 1);
  REQUIRE(keys_of(SynthEngineMode::kPercussion).count("shellFreqHz3") == 1);
  REQUIRE(keys_of(SynthEngineMode::kPercussion).count("shellT60S1") == 1);
  REQUIRE(keys_of(SynthEngineMode::kAdditive).count("drawbarsB3") == 1);

  // Each key lands on the member its path names.
  struct Probe {
    SynthEngineMode mode;
    const char* key;
    double value;
    float (*member)(const NativeSynthPatch&);
  };
  static const Probe kProbes[] = {
      {SynthEngineMode::kBowedString, "bowForce", 0.33,
       [](const NativeSynthPatch& p) { return p.bowed_string.bow_force; }},
      {SynthEngineMode::kKarplusStrong, "decayS", 7.5,
       [](const NativeSynthPatch& p) { return p.ks.decay_s; }},
      {SynthEngineMode::kPipeOrgan, "ranks2.level", 0.41,
       [](const NativeSynthPatch& p) { return p.pipe_organ.ranks[2].level; }},
      {SynthEngineMode::kFm, "ops1.env.attackMs", 37.0,
       [](const NativeSynthPatch& p) { return p.fm.ops[1].env.attack_ms; }},
      {SynthEngineMode::kHarpsichord, "pluck4", 0.21,
       [](const NativeSynthPatch& p) { return p.harpsichord.pluck_4; }},
      {SynthEngineMode::kPercussion, "shellFreqHz3", 432.0,
       [](const NativeSynthPatch& p) { return p.percussion.shell_freq_hz[3]; }},
      {SynthEngineMode::kAdditive, "drawbarsB3", 5.0,
       [](const NativeSynthPatch& p) { return p.additive.drawbars_b[3]; }},
  };
  for (const Probe& probe : kProbes) {
    INFO(probe.key);
    NativeSynthPatch p;
    p.mode = probe.mode;
    const EngineParam param{probe.key, probe.value};
    std::string error;
    REQUIRE(apply_engine_params(p, &param, 1, &error));
    REQUIRE(probe.member(p) == static_cast<float>(probe.value));
  }
}

TEST_CASE("the FM algorithm is an engine param over the whole enum", "[midi][synth]") {
  using sonare::midi::synth::FmAlgorithm;
  NativeSynthPatch fm;
  fm.mode = SynthEngineMode::kFm;
  const std::vector<EngineParamDescriptor> ds = engine_param_descriptors(fm);
  const EngineParamDescriptor* algorithm = descriptor_named(ds, "algorithm");
  REQUIRE(algorithm != nullptr);
  REQUIRE(algorithm->integer);
  REQUIRE_FALSE(algorithm->boolean);
  REQUIRE(algorithm->bounded);
  REQUIRE(algorithm->lo == static_cast<float>(FmAlgorithm::kStack2));
  REQUIRE(algorithm->hi == static_cast<float>(FmAlgorithm::kAdd2));
  REQUIRE(algorithm->unit == EngineUnit::None);
  REQUIRE(algorithm->value == static_cast<float>(fm.fm.algorithm));

  const EngineParam pair{"algorithm", static_cast<double>(FmAlgorithm::kPair2x2)};
  std::string error;
  REQUIRE(apply_engine_params(fm, &pair, 1, &error));
  REQUIRE(fm.fm.algorithm == FmAlgorithm::kPair2x2);
  const EngineParam past{"algorithm", static_cast<double>(FmAlgorithm::kAdd2) + 1.0};
  REQUIRE_FALSE(apply_engine_params(fm, &past, 1, &error));
  REQUIRE(error == "engine param 'algorithm': out of range [0, 5]");
  REQUIRE(fm.fm.algorithm == FmAlgorithm::kPair2x2);

  // No other engine's section carries the key.
  NativeSynthPatch reed;
  reed.mode = SynthEngineMode::kReed;
  REQUIRE(descriptor_named(engine_param_descriptors(reed), "algorithm") == nullptr);
}

TEST_CASE("engine params refuse what they cannot set and leave the patch untouched",
          "[midi][synth]") {
  NativeSynthPatch piano;
  piano.mode = SynthEngineMode::kPiano;
  piano = sonare::midi::synth::clamp_synth_patch(piano);
  const std::vector<EngineParamDescriptor> ds = engine_param_descriptors(piano);
  const EngineParamDescriptor* brightness = descriptor_named(ds, "brightness");
  const EngineParamDescriptor* strings = descriptor_named(ds, "strings");
  REQUIRE(brightness != nullptr);
  REQUIRE(brightness->bounded);
  REQUIRE(strings != nullptr);
  REQUIRE(strings->integer);
  REQUIRE(strings->bounded);
  const std::string brightness_range =
      " [" + printed(brightness->lo) + ", " + printed(brightness->hi) + "]";
  const std::string strings_range = " [" + printed(strings->lo) + ", " + printed(strings->hi) + "]";

  const auto refused = [](NativeSynthPatch patch, std::vector<EngineParam> params,
                          const std::string& message) {
    INFO(message);
    const std::vector<unsigned char> before = patch_bytes(patch);
    std::string error;
    REQUIRE_FALSE(apply_engine_params(patch, params.data(), params.size(), &error));
    REQUIRE(error == message);
    REQUIRE(patch_bytes(patch) == before);
  };
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();

  refused(piano, {{"notAField", 0.5}}, "engine param 'notAField': not a field of the piano engine");
  // Another engine's field and a common-section field are both outside the section.
  refused(piano, {{"bowForce", 0.5}}, "engine param 'bowForce': not a field of the piano engine");
  refused(piano, {{"cutoffHz", 1000.0}},
          "engine param 'cutoffHz': not a field of the piano engine");
  // The tuning path spelling is not a public key: the mapping runs forward only.
  refused(piano, {{"piano.brightness", 0.5}},
          "engine param 'piano.brightness': not a field of the piano engine");
  refused(piano, {{"brightness", nan}},
          "engine param 'brightness': not a finite number" + brightness_range);
  refused(piano, {{"brightness", inf}},
          "engine param 'brightness': not a finite number" + brightness_range);
  refused(piano, {{"brightness", brightness->hi + 0.5}},
          "engine param 'brightness': out of range" + brightness_range);
  refused(piano, {{"brightness", brightness->lo - 0.5}},
          "engine param 'brightness': out of range" + brightness_range);
  refused(piano, {{"strings", 2.5}}, "engine param 'strings': not an integer" + strings_range);
  refused(piano, {{"strings", strings->hi + 1.0}},
          "engine param 'strings': out of range" + strings_range);
  refused(piano, {{"brightness", 0.5}, {"brightness", 0.6}},
          "engine param 'brightness': given more than once");
  refused(piano, {{nullptr, 0.5}}, "engine param '': key is null");
  // All or nothing: a valid field before a refused one is not written either.
  refused(piano, {{"brightness", 0.5}, {"nope", 1.0}},
          "engine param 'nope': not a field of the piano engine");
  refused(piano, {{"brightness", 0.5}, {"strings", 0.5}},
          "engine param 'strings': not an integer" + strings_range);

  NativeSynthPatch harpsichord;
  harpsichord.mode = SynthEngineMode::kHarpsichord;
  refused(harpsichord, {{"eightA", 2.0}}, "engine param 'eightA': out of range [0, 1]");
  refused(harpsichord, {{"eightA", 0.5}}, "engine param 'eightA': not an integer [0, 1]");

  // An unbounded field still refuses what a float cannot hold, and an integer
  // what lies past its range, however far.
  NativeSynthPatch ks;
  ks.mode = SynthEngineMode::kKarplusStrong;
  const std::vector<EngineParamDescriptor> ks_descriptors = engine_param_descriptors(ks);
  const EngineParamDescriptor* coupling = descriptor_named(ks_descriptors, "bodyCoupling");
  REQUIRE(coupling != nullptr);
  REQUIRE_FALSE(coupling->bounded);
  refused(ks, {{"bodyCoupling", 1e300}},
          "engine param 'bodyCoupling': outside the 32-bit float range");
  NativeSynthPatch vocal;
  vocal.mode = SynthEngineMode::kVocal;
  const std::vector<EngineParamDescriptor> vocal_descriptors = engine_param_descriptors(vocal);
  const EngineParamDescriptor* vowel = descriptor_named(vocal_descriptors, "vowel");
  REQUIRE(vowel != nullptr);
  REQUIRE(vowel->integer);
  REQUIRE(vowel->bounded);
  refused(vocal, {{"vowel", 1e12}},
          "engine param 'vowel': out of range [" + printed(vowel->lo) + ", " + printed(vowel->hi) +
              "]");

  NativeSynthPatch subtractive;
  refused(subtractive, {{"brightness", 0.5}},
          "engine param 'brightness': not a field of the subtractive engine");

  // The control: an accepted value is written, with no message.
  NativeSynthPatch accepted = piano;
  const EngineParam ok{"brightness", (brightness->lo + brightness->hi) / 2.0};
  std::string error;
  REQUIRE(apply_engine_params(accepted, &ok, 1, &error));
  REQUIRE(error.empty());
  REQUIRE(accepted.piano.brightness == static_cast<float>(ok.value));
}

TEST_CASE("engine params accept each published float bound as its shortest decimal spelling",
          "[midi][synth]") {
  // The catalogue spells a float bound as the shortest decimal that reads back to it, so a
  // caller handing that spelling back gets a double just off the float (0.05 against 0.05f).
  const auto shortest = [](float value) {
    double read = 0.0;
    for (int precision = 1; precision <= 9; ++precision) {
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "%.*g", precision, static_cast<double>(value));
      read = std::strtod(buffer, nullptr);
      if (static_cast<float>(read) == value) break;
    }
    return read;
  };
  int checked = 0;
  for (const NativeSynthPatch& base : one_patch_per_engine_mode()) {
    for (const EngineParamDescriptor& d : engine_param_descriptors(base)) {
      if (!d.bounded || d.integer || d.boolean) continue;
      for (const float bound : {d.lo, d.hi}) {
        INFO(d.key << " = " << shortest(bound));
        NativeSynthPatch patch = base;
        const EngineParam param{d.key.c_str(), shortest(bound)};
        std::string error;
        REQUIRE(apply_engine_params(patch, &param, 1, &error));
        ++checked;
      }
      // One float step past the bound is still refused.
      NativeSynthPatch patch = base;
      const EngineParam past{d.key.c_str(), static_cast<double>(std::nextafter(
                                                d.hi, std::numeric_limits<float>::infinity()))};
      std::string error;
      REQUIRE_FALSE(apply_engine_params(patch, &past, 1, &error));
    }
  }
  REQUIRE(checked > 0);
}

TEST_CASE("engine params with none given leave every preset bit-identical", "[midi][synth]") {
  for (size_t i = 0; i < sonare::midi::synth::synth_preset_count(); ++i) {
    const auto* preset = sonare::midi::synth::synth_preset_at(i);
    INFO(preset->name);
    NativeSynthPatch patch = preset->config.patch;
    const std::vector<unsigned char> before = patch_bytes(patch);
    std::string error;
    REQUIRE(apply_engine_params(patch, nullptr, 0, &error));
    REQUIRE(apply_engine_params(patch, nullptr, 0, nullptr));
    REQUIRE(error.empty());
    REQUIRE(patch_bytes(patch) == before);
  }
}

TEST_CASE("every engine-section field carries a unit its member name agrees with",
          "[midi][synth]") {
  using sonare::midi::synth::patch_tuning_detail::engine_field_sites;
  const auto ends_with = [](const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
  };
  struct SuffixUnit {
    const char* suffix;
    EngineUnit unit;
  };
  static const SuffixUnit kSuffixes[] = {
      {"_hz", EngineUnit::Hz}, {"_ms", EngineUnit::Ms},       {"_s", EngineUnit::Seconds},
      {"_db", EngineUnit::Db}, {"_cents", EngineUnit::Cents}, {"_mm", EngineUnit::Millimeters},
  };
  size_t suffixed = 0;
  for (const NativeSynthPatch& base : one_patch_per_engine_mode()) {
    const auto sites = engine_field_sites(base);
    const auto ds = engine_param_descriptors(base);
    REQUIRE(sites.size() == ds.size());
    for (size_t i = 0; i < sites.size(); ++i) {
      const auto& site = sites[i];
      INFO(site.path << " (member path " << site.member_path << ")");
      REQUIRE(site.has_unit);
      REQUIRE(sonare::mastering::api::detail::unit_name(ds[i].unit) != nullptr);
      // Only a site the walker marked has an index removed.
      REQUIRE(site.indexed == (site.member_path != site.path));
      const std::string member = site.member_path.substr(site.member_path.rfind('.') + 1);
      for (const SuffixUnit& rule : kSuffixes) {
        if (ends_with(member, rule.suffix)) {
          ++suffixed;
          REQUIRE(ds[i].unit == rule.unit);
          break;
        }
      }
    }
  }
  REQUIRE(suffixed > 0);

  // A member whose own name ends in a digit keeps it; an indexed one loses only the index.
  NativeSynthPatch harpsichord;
  harpsichord.mode = SynthEngineMode::kHarpsichord;
  NativeSynthPatch percussion;
  percussion.mode = SynthEngineMode::kPercussion;
  const auto site_at = [](const NativeSynthPatch& p, const char* path) {
    for (const auto& site : engine_field_sites(p)) {
      if (site.path == path) return site;
    }
    FAIL("no site " << path);
    return engine_field_sites(p).front();
  };
  const auto pluck = site_at(harpsichord, "harpsichord.pluck_4");
  REQUIRE_FALSE(pluck.indexed);
  REQUIRE(pluck.member_path == "harpsichord.pluck_4");
  const auto shell = site_at(percussion, "percussion.shell_freq_hz3");
  REQUIRE(shell.indexed);
  REQUIRE(shell.member_path == "percussion.shell_freq_hz");
  const auto t60 = site_at(percussion, "percussion.shell_t60_s2");
  REQUIRE(t60.member_path == "percussion.shell_t60_s");
  const std::vector<EngineParamDescriptor> harpsichord_descriptors =
      engine_param_descriptors(harpsichord);
  const EngineParamDescriptor* rear = descriptor_named(harpsichord_descriptors, "rearSegmentMm");
  REQUIRE(rear != nullptr);
  REQUIRE(std::string(sonare::mastering::api::detail::unit_name(rear->unit)) == "mm");
}

TEST_CASE("every engine with a section has a base preset of that engine", "[midi][synth]") {
  using sonare::midi::synth::base_preset_name;
  using sonare::midi::synth::find_synth_preset;
  for (const NativeSynthPatch& p : one_patch_per_engine_mode()) {
    INFO("engine mode " << static_cast<int>(p.mode));
    const bool has_section = !engine_param_descriptors(p).empty();
    const char* name = base_preset_name(p.mode);
    REQUIRE((name != nullptr) == has_section);
    if (name == nullptr) continue;
    INFO(name);
    const auto* preset = find_synth_preset(name);
    REQUIRE(preset != nullptr);
    REQUIRE(preset->config.patch.mode == p.mode);
  }
}

TEST_CASE("gm_fallback_max_tail_samples bounds every fallback patch table", "[midi][synth]") {
  const int64_t bound =
      sonare::midi::synth::gm_fallback_max_tail_samples(kOutRate, 1.0f, 1.0f, 1.0f);
  const auto covered = [bound](const sonare::midi::synth::NativeSynthPatch& p) {
    return bound >= sonare::midi::synth::native_patch_tail_samples(p, kOutRate, {}, nullptr);
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

// ---------------------------------------------------------------------------
// Shared bus residual split across source targets (SourceResidualSplitter)
// ---------------------------------------------------------------------------

namespace {

/// NativeSynth config with a bus-level body (soundboard + pedal-gated
/// resonance), so a single-note render actually exercises a nonzero
/// bus-wide residual rather than just per-voice dry audio.
NativeSynthConfig residual_test_config() {
  NativeSynthConfig cfg;
  cfg.patch.mode = SynthEngineMode::kPiano;
  return cfg;
}

}  // namespace

TEST_CASE(
    "NativeSynth and Sf2Player source-track residual: one source is bit-identical to a "
    "slot-0-only render",
    "[midi][synth]") {
  // Longer than kResidualChunk (256) so the render exercises both the
  // flush-at-chunk-boundary path and the flush-at-block-end path.
  constexpr int kSamples = 600;
  constexpr uint32_t kTrack = 77;

  const auto exercise = [](auto& solo, auto& laned) {
    solo.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
    std::vector<float> solo_l(static_cast<size_t>(kSamples), 0.0f);
    std::vector<float> solo_r(static_cast<size_t>(kSamples), 0.0f);
    float* solo_target[] = {solo_l.data(), solo_r.data()};
    const MidiInstrumentSourceOutput solo_outputs[] = {{0, solo_target}};
    REQUIRE(solo.process_source_tracks(solo_outputs, std::size(solo_outputs), 2, kSamples));

    MidiEvent on = event(sonare::midi::make_midi1_note_on(0, 0, 60, 100));
    on.source_track_id = kTrack;
    laned.on_event(0, on);
    std::vector<float> fallback_l(static_cast<size_t>(kSamples), 0.0f);
    std::vector<float> fallback_r(static_cast<size_t>(kSamples), 0.0f);
    std::vector<float> lane_l(static_cast<size_t>(kSamples), 0.0f);
    std::vector<float> lane_r(static_cast<size_t>(kSamples), 0.0f);
    float* fallback_target[] = {fallback_l.data(), fallback_r.data()};
    float* lane_target[] = {lane_l.data(), lane_r.data()};
    const MidiInstrumentSourceOutput laned_outputs[] = {{0, fallback_target},
                                                        {kTrack, lane_target}};
    {
      AllocationGuard guard;
      REQUIRE(laned.process_source_tracks(laned_outputs, std::size(laned_outputs), 2, kSamples));
      REQUIRE(guard.count() == 0);
    }

    REQUIRE(peak(solo_l) > 0.0f);  // non-vacuity: there is something to match
    for (size_t i = 0; i < static_cast<size_t>(kSamples); ++i) {
      REQUIRE(lane_l[i] == solo_l[i]);
      REQUIRE(lane_r[i] == solo_r[i]);
      REQUIRE(fallback_l[i] == 0.0f);
      REQUIRE(fallback_r[i] == 0.0f);
    }
  };

  SECTION("NativeSynth") {
    NativeSynth solo(residual_test_config());
    solo.prepare(kOutRate, 256);
    NativeSynth laned(residual_test_config());
    laned.prepare(kOutRate, 256);
    exercise(solo, laned);
  }
  SECTION("Sf2Player") {
    Sf2Player solo = make_fallback_player();
    Sf2Player laned = make_fallback_player();
    exercise(solo, laned);
  }
}

TEST_CASE("NativeSynth source-track render fans a mono fold-down to every channel past two",
          "[midi][synth]") {
  // Longer than kResidualChunk (256) so both residual flush paths reach the
  // channels past two.
  constexpr int kSamples = 600;
  constexpr int kChannels = 4;
  const auto render = [&](bool source_render) {
    NativeSynth synth(residual_test_config());
    synth.prepare(kOutRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
    std::vector<std::vector<float>> bufs(kChannels,
                                         std::vector<float>(static_cast<size_t>(kSamples), 0.0f));
    float* target[kChannels];
    for (int ch = 0; ch < kChannels; ++ch) target[ch] = bufs[static_cast<size_t>(ch)].data();
    if (source_render) {
      const MidiInstrumentSourceOutput outputs[] = {{0, target}};
      REQUIRE(synth.process_source_tracks(outputs, std::size(outputs), kChannels, kSamples));
    } else {
      synth.process(target, kChannels, kSamples);
    }
    return bufs;
  };
  const std::vector<std::vector<float>> laned = render(true);
  const std::vector<std::vector<float>> direct = render(false);

  const float scale = peak(direct[0]);
  REQUIRE(scale > 0.0f);  // non-vacuity: there is something to fan out
  const float tol = 1.0e-5f * scale;
  for (size_t ch = 2; ch < static_cast<size_t>(kChannels); ++ch) {
    for (size_t i = 0; i < static_cast<size_t>(kSamples); ++i) {
      // Each contribution reaches the extra channel exactly once, as the fold-down.
      const float fold = sonare::constants::kInvSqrt2 * (laned[0][i] + laned[1][i]);
      REQUIRE(std::fabs(laned[ch][i] - fold) <= tol);
      REQUIRE(std::fabs(laned[ch][i] - direct[ch][i]) <= tol);
    }
  }
}

TEST_CASE(
    "NativeSynth and Sf2Player source-track residual: muting a single lane attenuates the "
    "whole render by 90 dB",
    "[midi][synth]") {
  constexpr int kSamples = 512;
  constexpr uint32_t kTrack = 55;
  const float lane_gain_muted = db_to_linear(-96.0f);

  const auto exercise = [&](auto& synth) {
    MidiEvent on = event(sonare::midi::make_midi1_note_on(0, 0, 60, 100));
    on.source_track_id = kTrack;
    synth.on_event(0, on);

    std::vector<float> fallback_l(static_cast<size_t>(kSamples), 0.0f);
    std::vector<float> fallback_r(static_cast<size_t>(kSamples), 0.0f);
    std::vector<float> lane_l(static_cast<size_t>(kSamples), 0.0f);
    std::vector<float> lane_r(static_cast<size_t>(kSamples), 0.0f);
    float* fallback_target[] = {fallback_l.data(), fallback_r.data()};
    float* lane_target[] = {lane_l.data(), lane_r.data()};
    const MidiInstrumentSourceOutput outputs[] = {{0, fallback_target}, {kTrack, lane_target}};
    REQUIRE(synth.process_source_tracks(outputs, std::size(outputs), 2, kSamples));

    // A real engine sums slot 0 (unfaded) and the lane target (faded by the
    // lane's own fader) into the master bus, so this reproduces what a lane
    // fader at 0 dB vs -96 dB would deliver downstream.
    std::vector<float> unmuted(static_cast<size_t>(kSamples) * 2);
    std::vector<float> muted(static_cast<size_t>(kSamples) * 2);
    for (size_t i = 0; i < static_cast<size_t>(kSamples); ++i) {
      unmuted[2 * i] = fallback_l[i] + lane_l[i];
      unmuted[2 * i + 1] = fallback_r[i] + lane_r[i];
      muted[2 * i] = fallback_l[i] + lane_l[i] * lane_gain_muted;
      muted[2 * i + 1] = fallback_r[i] + lane_r[i] * lane_gain_muted;
    }
    const float rms_unmuted = rms(unmuted);
    const float rms_muted = rms(muted);
    REQUIRE(rms_unmuted > 1.0e-4f);  // non-vacuity
    const double attenuation_db =
        20.0 * std::log10(static_cast<double>(rms_unmuted) /
                          std::max(static_cast<double>(rms_muted), 1e-12));
    INFO("lane-mute attenuation (dB): " << attenuation_db);
    REQUIRE(attenuation_db >= 90.0);
  };

  SECTION("NativeSynth") {
    NativeSynth synth(residual_test_config());
    synth.prepare(kOutRate, 256);
    exercise(synth);
  }
  SECTION("Sf2Player") {
    Sf2Player synth = make_fallback_player();
    exercise(synth);
  }
}

TEST_CASE("NativeSynth and Sf2Player source-track residual sums to process() within tolerance",
          "[midi][synth]") {
  constexpr int kSamples = 600;
  constexpr uint32_t kTrack = 33;

  const auto exercise = [](auto& reference, auto& laned) {
    reference.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
    const StereoRender ref = render(reference, kSamples);

    MidiEvent on = event(sonare::midi::make_midi1_note_on(0, 0, 60, 100));
    on.source_track_id = kTrack;
    laned.on_event(0, on);
    std::vector<float> fallback_l(static_cast<size_t>(kSamples), 0.0f);
    std::vector<float> fallback_r(static_cast<size_t>(kSamples), 0.0f);
    std::vector<float> lane_l(static_cast<size_t>(kSamples), 0.0f);
    std::vector<float> lane_r(static_cast<size_t>(kSamples), 0.0f);
    float* fallback_target[] = {fallback_l.data(), fallback_r.data()};
    float* lane_target[] = {lane_l.data(), lane_r.data()};
    const MidiInstrumentSourceOutput outputs[] = {{0, fallback_target}, {kTrack, lane_target}};
    REQUIRE(laned.process_source_tracks(outputs, std::size(outputs), 2, kSamples));

    const float block_peak = std::max(peak(ref.left), peak(ref.right));
    REQUIRE(block_peak > 0.0f);
    float max_diff = 0.0f;
    for (size_t i = 0; i < static_cast<size_t>(kSamples); ++i) {
      max_diff = std::max(max_diff, std::fabs(fallback_l[i] + lane_l[i] - ref.left[i]));
      max_diff = std::max(max_diff, std::fabs(fallback_r[i] + lane_r[i] - ref.right[i]));
    }
    INFO("max_diff/block_peak: " << (max_diff / block_peak));
    REQUIRE(max_diff <= 1.0e-6f * block_peak);
  };

  SECTION("NativeSynth") {
    NativeSynthConfig cfg = residual_test_config();
    cfg.dc_block = true;
    NativeSynth reference(cfg);
    reference.prepare(kOutRate, 256);
    NativeSynth laned(cfg);
    laned.prepare(kOutRate, 256);
    exercise(reference, laned);
  }
  SECTION("Sf2Player") {
    Sf2Player reference = make_fallback_player();
    Sf2Player laned = make_fallback_player();
    exercise(reference, laned);
  }
}

TEST_CASE(
    "NativeSynth and Sf2Player source-track residual: two sources split and isolate "
    "correctly",
    "[midi][synth]") {
  constexpr int kSamples = 600;
  constexpr uint32_t kTrackA = 11;
  constexpr uint32_t kTrackB = 22;
  const float lane_gain_muted = db_to_linear(-96.0f);

  // A patch whose bus-wide residual dominates the signal (e.g. the piano
  // board, which the source comments say carries "most of the note") breaks
  // this correlation by construction: two same-duration, comparable-energy
  // notes earn comparable dry-energy weight, so each gets roughly half of a
  // residual that is most of the loudness, and muting one lane afterward
  // cannot hand its foregone half back to the other. A plain (non-piano,
  // non-GM) patch keeps the shared residual to what config_.dc_block alone
  // produces -- small next to the dry signal -- which is the regime this
  // correlation claim is about; setup() picks the equivalent program for
  // Sf2Player's fallback voices (leaving program 0's piano body out of it).
  // Under the engines' default retrigger (SynthRetrigger::kFree), a voice's
  // start phase/jitter seed derives from its voice-pool allocation index and
  // allocation count -- so B must be the FIRST note allocated in every
  // instance below, or its own dry audio silently stops being comparable
  // across instances (a determinism property of the voice engines, not of
  // the splitter under test). B is sent before A for exactly that reason.
  const auto exercise = [&](auto& reference, auto& two_source, auto& b_only, auto setup) {
    setup(reference);
    reference.on_event(0, event(sonare::midi::make_midi1_note_on(0, 1, 67, 100)));
    reference.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
    const StereoRender ref = render(reference, kSamples);
    const float block_peak = std::max(peak(ref.left), peak(ref.right));
    REQUIRE(block_peak > 0.0f);

    setup(two_source);
    MidiEvent b = event(sonare::midi::make_midi1_note_on(0, 1, 67, 100));
    b.source_track_id = kTrackB;
    MidiEvent a = event(sonare::midi::make_midi1_note_on(0, 0, 60, 100));
    a.source_track_id = kTrackA;
    two_source.on_event(0, b);
    two_source.on_event(0, a);
    std::vector<float> fallback_l(static_cast<size_t>(kSamples), 0.0f);
    std::vector<float> fallback_r(static_cast<size_t>(kSamples), 0.0f);
    std::vector<float> a_l(static_cast<size_t>(kSamples), 0.0f);
    std::vector<float> a_r(static_cast<size_t>(kSamples), 0.0f);
    std::vector<float> b_l(static_cast<size_t>(kSamples), 0.0f);
    std::vector<float> b_r(static_cast<size_t>(kSamples), 0.0f);
    float* fallback_target[] = {fallback_l.data(), fallback_r.data()};
    float* a_target[] = {a_l.data(), a_r.data()};
    float* b_target[] = {b_l.data(), b_r.data()};
    const MidiInstrumentSourceOutput outputs[] = {
        {0, fallback_target}, {kTrackA, a_target}, {kTrackB, b_target}};
    REQUIRE(two_source.process_source_tracks(outputs, std::size(outputs), 2, kSamples));

    float max_diff = 0.0f;
    for (size_t i = 0; i < static_cast<size_t>(kSamples); ++i) {
      const float sum_l = fallback_l[i] + a_l[i] + b_l[i];
      const float sum_r = fallback_r[i] + a_r[i] + b_r[i];
      max_diff = std::max(max_diff, std::fabs(sum_l - ref.left[i]));
      max_diff = std::max(max_diff, std::fabs(sum_r - ref.right[i]));
    }
    INFO("max_diff/block_peak: " << (max_diff / block_peak));
    REQUIRE(max_diff <= 1.0e-5f * block_peak);

    setup(b_only);
    b_only.on_event(0, event(sonare::midi::make_midi1_note_on(0, 1, 67, 100)));
    const StereoRender b_only_render = render(b_only, kSamples);

    std::vector<float> muted_mix(static_cast<size_t>(kSamples) * 2);
    std::vector<float> b_only_mix(static_cast<size_t>(kSamples) * 2);
    for (size_t i = 0; i < static_cast<size_t>(kSamples); ++i) {
      muted_mix[2 * i] = fallback_l[i] + a_l[i] * lane_gain_muted + b_l[i];
      muted_mix[2 * i + 1] = fallback_r[i] + a_r[i] * lane_gain_muted + b_r[i];
      b_only_mix[2 * i] = b_only_render.left[i];
      b_only_mix[2 * i + 1] = b_only_render.right[i];
    }
    const float correlation =
        pearson_correlation(muted_mix.data(), b_only_mix.data(), muted_mix.size());
    INFO("muted-lane-A vs B-only correlation: " << correlation);
    REQUIRE(correlation >= 0.99f);
  };

  SECTION("NativeSynth") {
    NativeSynthConfig cfg;  // default (non-piano) patch: dc_block is the only residual
    NativeSynth reference(cfg);
    reference.prepare(kOutRate, 256);
    NativeSynth two_source(cfg);
    two_source.prepare(kOutRate, 256);
    NativeSynth b_only(cfg);
    b_only.prepare(kOutRate, 256);
    exercise(reference, two_source, b_only, [](NativeSynth&) {});
  }
  SECTION("Sf2Player") {
    Sf2Player reference = make_fallback_player();
    Sf2Player two_source = make_fallback_player();
    Sf2Player b_only = make_fallback_player();
    // Program 80 (Lead 1, square) has no body resonator, unlike the default
    // program 0 (piano).
    const auto pick_lead = [](Sf2Player& p) {
      p.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 80)));
      p.on_event(0, event(sonare::midi::make_midi1_program_change(0, 1, 80)));
    };
    exercise(reference, two_source, b_only, pick_lead);
  }
}

#if defined(SONARE_MIDI_WITH_FX)
TEST_CASE("Sf2Player source-track residual: reverb leakage on a send-0 lane stays under -40 dB",
          "[midi][synth]") {
  // The effect return is split by send energy, so a part sending nothing to
  // reverb (CC91=0) carries none of the other part's tail on its lane.
  // dc_block is off: the DC blocker's share is split by total dry energy and
  // measures about -36 dB on its own, which is not what this case gates.
  constexpr int kSamples = 4800;
  constexpr uint32_t kTrackWet = 1;
  constexpr uint32_t kTrackDry = 2;

  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.dc_block = false;
  Sf2Player two_parts(cfg);
  two_parts.prepare(kOutRate, 256);
  two_parts.on_event(
      0, event(sonare::midi::make_midi1_control_change(0, 1, 91, 0)));  // part 2: send 0
  MidiEvent wet = event(sonare::midi::make_midi1_note_on(0, 0, 60, 100));
  wet.source_track_id = kTrackWet;
  MidiEvent dry = event(sonare::midi::make_midi1_note_on(0, 1, 67, 100));
  dry.source_track_id = kTrackDry;
  // Dry note first: a voice's retrigger jitter seed follows its allocation
  // index, so it must match the solo render's or the seed reads as leakage.
  two_parts.on_event(0, dry);
  two_parts.on_event(0, wet);

  std::vector<float> fallback_l(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> fallback_r(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> wet_l(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> wet_r(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> dry_l(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> dry_r(static_cast<size_t>(kSamples), 0.0f);
  float* fallback_target[] = {fallback_l.data(), fallback_r.data()};
  float* wet_target[] = {wet_l.data(), wet_r.data()};
  float* dry_target[] = {dry_l.data(), dry_r.data()};
  const MidiInstrumentSourceOutput outputs[] = {
      {0, fallback_target}, {kTrackWet, wet_target}, {kTrackDry, dry_target}};
  REQUIRE(two_parts.process_source_tracks(outputs, std::size(outputs), 2, kSamples));

  // Solo render of the send-0 part alone: nothing else ever sounds, so its
  // lane carries only its own dry contribution (plus a negligible non-reverb
  // residual share).
  Sf2Player solo_dry(cfg);
  solo_dry.prepare(kOutRate, 256);
  solo_dry.on_event(0, event(sonare::midi::make_midi1_control_change(0, 1, 91, 0)));
  solo_dry.on_event(0, event(sonare::midi::make_midi1_note_on(0, 1, 67, 100)));
  const StereoRender solo = render(solo_dry, kSamples);

  const float dry_level = std::max(rms(solo.left), 1e-12f);
  std::vector<float> diff_l(static_cast<size_t>(kSamples));
  std::vector<float> diff_r(static_cast<size_t>(kSamples));
  for (size_t i = 0; i < static_cast<size_t>(kSamples); ++i) {
    diff_l[i] = dry_l[i] - solo.left[i];
    diff_r[i] = dry_r[i] - solo.right[i];
  }
  const float leakage_level = std::max(rms(diff_l), rms(diff_r));
  const double leakage_db =
      20.0 * std::log10(static_cast<double>(leakage_level) / static_cast<double>(dry_level));
  INFO("SF2 send-0 lane reverb leakage relative to its own dry (dB): " << leakage_db);
  REQUIRE(dry_level > 1.0e-6f);  // non-vacuity: there is a dry reference to compare against
  REQUIRE(leakage_db <= -40.0);
}

#if defined(SONARE_WITH_MASTERING)
TEST_CASE(
    "Sf2Player source-track residual: a bussed insert part's lane attenuates its own "
    "contribution by 90 dB",
    "[midi][synth]") {
  // Part 0 carries a drive insert (bussed); part 1 stays plain. Muting part
  // 0's lane has to remove its whole post-insert bus output, leaving a
  // part-1-only render. dc_block and both reverb sends are off: those shares
  // are only approximately separable between two simultaneous sources, and
  // this case gates the bussed part's own output, which is exact.
  constexpr int kSamples = 2400;
  constexpr uint32_t kTrackA = 3;
  constexpr uint32_t kTrackB = 4;
  const float lane_gain_muted = db_to_linear(-96.0f);

  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.part_rigs[0].mode = sonare::midi::PartRigMode::kChain;
  cfg.part_rigs[0].stages = {{"saturation.softClipper", "{}"}};
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  cfg.dc_block = false;

  Sf2Player two_source(cfg);
  two_source.prepare(kOutRate, 256);
#if defined(SONARE_MIDI_WITH_FX)
  two_source.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 91, 0)));
  two_source.on_event(0, event(sonare::midi::make_midi1_control_change(0, 1, 91, 0)));
#endif
  MidiEvent a = event(sonare::midi::make_midi1_note_on(0, 0, 60, 100));
  a.source_track_id = kTrackA;
  MidiEvent b = event(sonare::midi::make_midi1_note_on(0, 1, 67, 100));
  b.source_track_id = kTrackB;
  // b first: matches the allocation order (and so the retrigger jitter seed)
  // of the b_only render below, the same reason the two-source residual case
  // above sends B before A.
  two_source.on_event(0, b);
  two_source.on_event(0, a);

  std::vector<float> fallback_l(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> fallback_r(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> a_l(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> a_r(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> b_l(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> b_r(static_cast<size_t>(kSamples), 0.0f);
  float* fallback_target[] = {fallback_l.data(), fallback_r.data()};
  float* a_target[] = {a_l.data(), a_r.data()};
  float* b_target[] = {b_l.data(), b_r.data()};
  const MidiInstrumentSourceOutput outputs[] = {
      {0, fallback_target}, {kTrackA, a_target}, {kTrackB, b_target}};
  REQUIRE(two_source.process_source_tracks(outputs, std::size(outputs), 2, kSamples));

  Sf2Player b_only(cfg);
  b_only.prepare(kOutRate, 256);
#if defined(SONARE_MIDI_WITH_FX)
  b_only.on_event(0, event(sonare::midi::make_midi1_control_change(0, 1, 91, 0)));
#endif
  b_only.on_event(0, event(sonare::midi::make_midi1_note_on(0, 1, 67, 100)));
  const StereoRender b_render = render(b_only, kSamples);

  // Both diffs cancel part B's own render against the B-only baseline, so
  // what remains is exactly what part A (dry plus its whole bussed insert
  // output) is responsible for, at full level and at -96 dB.
  std::vector<float> unmuted_diff(static_cast<size_t>(kSamples));
  std::vector<float> muted_diff(static_cast<size_t>(kSamples));
  for (size_t i = 0; i < static_cast<size_t>(kSamples); ++i) {
    const float unmuted_l = fallback_l[i] + a_l[i] + b_l[i];
    const float muted_l = fallback_l[i] + a_l[i] * lane_gain_muted + b_l[i];
    unmuted_diff[i] = unmuted_l - b_render.left[i];
    muted_diff[i] = muted_l - b_render.left[i];
  }
  const float unmuted_level = rms(unmuted_diff);
  const float muted_level = std::max(rms(muted_diff), 1e-12f);
  REQUIRE(unmuted_level > 1.0e-4f);  // non-vacuity: A actually contributes something
  const double attenuation_db =
      20.0 * std::log10(static_cast<double>(unmuted_level) / static_cast<double>(muted_level));
  INFO("bussed insert part-A lane-mute attenuation vs B-only baseline (dB): " << attenuation_db);
  REQUIRE(attenuation_db >= 90.0);
}

TEST_CASE("Sf2Player source-track lanes sum to the plain render at a non-unity output gain",
          "[midi][synth]") {
  // Every residual component (bussed insert, piano board, reverb return,
  // remainder) has to leave the player through the same output gain as the
  // dry voices, or the lanes stop summing to what process() renders.
  constexpr int kSamples = 1200;
  constexpr uint32_t kTrackA = 7;
  constexpr uint32_t kTrackB = 8;
  Sf2PlayerConfig cfg;
  cfg.gain = 0.5f;
  cfg.part_rigs[0].mode = sonare::midi::PartRigMode::kChain;
  cfg.part_rigs[0].stages = {{"saturation.softClipper", "{}"}};
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };

  Sf2Player reference(cfg);
  reference.prepare(kOutRate, 256);
  reference.on_event(0, event(sonare::midi::make_midi1_note_on(0, 1, 67, 100)));
  reference.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  const StereoRender ref = render(reference, kSamples);
  const float block_peak = std::max(peak(ref.left), peak(ref.right));
  REQUIRE(block_peak > 0.0f);

  Sf2Player laned(cfg);
  laned.prepare(kOutRate, 256);
  MidiEvent b = event(sonare::midi::make_midi1_note_on(0, 1, 67, 100));
  b.source_track_id = kTrackB;
  MidiEvent a = event(sonare::midi::make_midi1_note_on(0, 0, 60, 100));
  a.source_track_id = kTrackA;
  laned.on_event(0, b);
  laned.on_event(0, a);
  std::vector<float> fallback_l(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> fallback_r(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> a_l(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> a_r(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> b_l(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> b_r(static_cast<size_t>(kSamples), 0.0f);
  float* fallback_target[] = {fallback_l.data(), fallback_r.data()};
  float* a_target[] = {a_l.data(), a_r.data()};
  float* b_target[] = {b_l.data(), b_r.data()};
  const MidiInstrumentSourceOutput outputs[] = {
      {0, fallback_target}, {kTrackA, a_target}, {kTrackB, b_target}};
  REQUIRE(laned.process_source_tracks(outputs, std::size(outputs), 2, kSamples));

  float max_diff = 0.0f;
  for (size_t i = 0; i < static_cast<size_t>(kSamples); ++i) {
    max_diff = std::max(max_diff, std::fabs(fallback_l[i] + a_l[i] + b_l[i] - ref.left[i]));
    max_diff = std::max(max_diff, std::fabs(fallback_r[i] + a_r[i] + b_r[i] - ref.right[i]));
  }
  INFO("max_diff/block_peak: " << (max_diff / block_peak));
  REQUIRE(max_diff <= 1.0e-5f * block_peak);
}
#endif  // SONARE_WITH_MASTERING
#endif

TEST_CASE(
    "NativeSynth source-track residual: muting the piano lane leaves the saw lane's own render",
    "[midi][synth]") {
  // Piano on lane A drives the shared soundboard/sympathetic bank; a saw on
  // lane B drives none of it. Muting A has to remove the body's contribution along with the
  // dry piano, leaving a render that correlates with a saw-only render.
  constexpr int kSamples = 2400;
  constexpr uint32_t kTrackPiano = 5;
  constexpr uint32_t kTrackSaw = 6;
  const float lane_gain_muted = db_to_linear(-96.0f);

  // GM mode so program 0 (piano, the default) and a sawtooth lead can sound
  // together on one instrument -- a fixed single-patch config plays every
  // channel through the same engine, which could not tell the two apart.
  NativeSynthConfig cfg;
  cfg.use_gm_programs = true;

  NativeSynth two_source(cfg);
  two_source.prepare(kOutRate, 256);
  two_source.on_event(
      0, event(sonare::midi::make_midi1_program_change(0, 1, 81)));  // Lead 2 (sawtooth)
  MidiEvent piano = event(sonare::midi::make_midi1_note_on(0, 0, 60, 100));
  piano.source_track_id = kTrackPiano;
  MidiEvent saw = event(sonare::midi::make_midi1_note_on(0, 1, 67, 100));
  saw.source_track_id = kTrackSaw;
  // saw first: matches the allocation order (and so the retrigger jitter
  // seed -- a bare oscillator's start phase) of the saw-only render below,
  // the same reason the two-source residual cases above order their notes.
  two_source.on_event(0, saw);
  two_source.on_event(0, piano);

  std::vector<float> fallback_l(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> fallback_r(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> piano_l(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> piano_r(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> saw_l(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> saw_r(static_cast<size_t>(kSamples), 0.0f);
  float* fallback_target[] = {fallback_l.data(), fallback_r.data()};
  float* piano_target[] = {piano_l.data(), piano_r.data()};
  float* saw_target[] = {saw_l.data(), saw_r.data()};
  const MidiInstrumentSourceOutput outputs[] = {
      {0, fallback_target}, {kTrackPiano, piano_target}, {kTrackSaw, saw_target}};
  REQUIRE(two_source.process_source_tracks(outputs, std::size(outputs), 2, kSamples));

  NativeSynth saw_only(cfg);
  saw_only.prepare(kOutRate, 256);
  saw_only.on_event(0, event(sonare::midi::make_midi1_program_change(0, 1, 81)));
  saw_only.on_event(0, event(sonare::midi::make_midi1_note_on(0, 1, 67, 100)));
  const StereoRender saw_render = render(saw_only, kSamples);

  std::vector<float> muted_mix(static_cast<size_t>(kSamples) * 2);
  std::vector<float> saw_only_mix(static_cast<size_t>(kSamples) * 2);
  for (size_t i = 0; i < static_cast<size_t>(kSamples); ++i) {
    muted_mix[2 * i] = fallback_l[i] + piano_l[i] * lane_gain_muted + saw_l[i];
    muted_mix[2 * i + 1] = fallback_r[i] + piano_r[i] * lane_gain_muted + saw_r[i];
    saw_only_mix[2 * i] = saw_render.left[i];
    saw_only_mix[2 * i + 1] = saw_render.right[i];
  }
  const float correlation =
      pearson_correlation(muted_mix.data(), saw_only_mix.data(), muted_mix.size());
  INFO("muted-piano-lane vs saw-only correlation: " << correlation);
  REQUIRE(correlation >= 0.99f);
}

namespace {

/// Held note from a GM fallback program, left channel.
std::vector<float> render_patch(const sonare::midi::synth::NativeSynthPatch& patch, uint8_t note,
                                int num_samples) {
  NativeSynthConfig cfg;
  cfg.patch = patch;
  NativeSynth synth(cfg);
  synth.prepare(kOutRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, 100)));
  return render(synth, num_samples).left;
}

std::vector<float> render_program(uint8_t program, uint8_t note, int num_samples) {
  return render_patch(sonare::midi::synth::gm_fallback_patch(0, program), note, num_samples);
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

TEST_CASE("the subtractive and FM velocity exponents scale the voice amplifier alone",
          "[midi][synth]") {
  // The oscillators and filter render the same at one velocity, so two exponents differ by
  // exactly (velocity/127)^(e1 - e2).
  for (const bool fm : {false, true}) {
    auto render_at = [fm](float exponent, uint8_t velocity) {
      NativeSynthConfig cfg;
      cfg.patch.cutoff_hz = 3000.0f;
      if (fm) {
        cfg.patch.mode = SynthEngineMode::kFm;
        cfg.patch.fm.ops[0].level = 1.0f;
        cfg.patch.fm.velocity_exponent = exponent;
      } else {
        cfg.patch.waveform = sonare::midi::synth::VaWaveform::kSaw;
        cfg.patch.va.velocity_exponent = exponent;
      }
      NativeSynth synth(cfg);
      synth.prepare(kOutRate, 256);
      synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, velocity)));
      return render(synth, 24000).left;
    };
    for (uint8_t velocity : {40, 120}) {
      const std::vector<float> linear = render_at(1.0f, velocity);
      const std::vector<float> square = render_at(2.0f, velocity);
      INFO((fm ? "fm" : "subtractive") << ", velocity " << int(velocity));
      REQUIRE(rms(square, 4800) > 1.0e-3f);
      const float ratio_db = 20.0f * std::log10(rms(linear, 4800) / rms(square, 4800));
      CHECK(std::abs(ratio_db + 20.0f * std::log10(velocity / 127.0f)) < 0.01f);
    }
  }
}

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

TEST_CASE("the FX and SFX octets have dedicated voices", "[midi][synth]") {
  using sonare::midi::synth::gm_fallback_patch;
  using sonare::midi::synth::detail::family_patches;
  static constexpr std::array<uint8_t, 16> kPrograms = {
      96, 97, 98, 99, 100, 101, 102, 103, 120, 121, 122, 123, 124, 125, 126, 127,
  };

  std::vector<std::vector<float>> tones;
  tones.reserve(kPrograms.size());
  for (const uint8_t program : kPrograms) {
    const auto& patch = gm_fallback_patch(0, program);
    INFO("GM program " << int(program));
    // The old family patch remains available as a default, but no member of
    // either octet should resolve to it after receiving a named voice.
    REQUIRE(&patch != &family_patches()[program >> 3]);
    tones.push_back(render_program(program, 60, 96000));
    REQUIRE(peak(tones.back()) > 0.001f);
  }

  // Pointer inequality proves table dispatch; rendered inequality proves the
  // new patches are not merely names wrapped around the former family defaults.
  for (size_t i = 0; i < kPrograms.size(); ++i) {
    const auto& family = family_patches()[kPrograms[i] >> 3];
    REQUIRE(tones[i] != render_patch(family, 60, 96000));
    for (size_t j = i + 1; j < kPrograms.size(); ++j) REQUIRE(tones[i] != tones[j]);
  }
}

TEST_CASE("the bird tweet uses an amplitude-envelope pitch sweep", "[midi][synth]") {
  using sonare::midi::synth::evaluate_mod_matrix;
  using sonare::midi::synth::gm_fallback_patch;
  using sonare::midi::synth::ModDestination;
  using sonare::midi::synth::ModSource;
  using sonare::midi::synth::ModSourceValues;

  const auto& bird = gm_fallback_patch(0, 123);
  const auto& route = bird.mod_matrix.routes[0];
  REQUIRE(route.source == ModSource::kAmpEnv);
  REQUIRE(route.destination == ModDestination::kPitchCents);
  REQUIRE(route.depth > 600.0f);

  ModSourceValues quiet;
  quiet.amp_env = 0.0f;
  ModSourceValues open;
  open.amp_env = 1.0f;
  REQUIRE(evaluate_mod_matrix(bird.mod_matrix, quiet).pitch_cents == 0.0f);
  REQUIRE(evaluate_mod_matrix(bird.mod_matrix, open).pitch_cents > 600.0f);

  // Remove the route from an otherwise identical patch. The rendered change
  // guards against a vacuous declaration that never reaches the oscillator.
  auto without_sweep = bird;
  without_sweep.mod_matrix.routes[0] = {};
  const std::vector<float> swept = render_patch(bird, 60, 24000);
  const std::vector<float> static_pitch = render_patch(without_sweep, 60, 24000);
  REQUIRE(swept != static_pitch);
  float max_difference = 0.0f;
  for (size_t i = 0; i < swept.size(); ++i) {
    max_difference = std::max(max_difference, std::fabs(swept[i] - static_pitch[i]));
  }
  REQUIRE(max_difference > 0.01f);
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

TEST_CASE("the metallic pad's top is a layer the filter does not shape", "[midi][synth]") {
  // The pad is one narrow pulse twice over: through a lowpass on the
  // fundamental for the low partials, and unfiltered for the flat top above
  // them, which no lowpass setting gives. Filtering that copy too loses the top.
  const auto& metallic = sonare::midi::synth::gm_fallback_patch(0, 93);
  auto filtered = metallic;
  filtered.va.osc2_dry = 0.0f;
  const double top = high_fraction(render_patch(metallic, 60, 96000), 40000, 4000.0, 4096);
  const double lowpassed = high_fraction(render_patch(filtered, 60, 96000), 40000, 4000.0, 4096);
  // Measured 45x above 4 kHz at middle C.
  REQUIRE(top > 10.0 * lowpassed);
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

TEST_CASE("the banjo is a steel string drained by a head", "[midi][synth]") {
  // 105 was the last program in its octet still answering the family patch. It
  // derives from the steel guitar, and what makes it a banjo rather than a
  // bright guitar is the membrane: it radiates so efficiently that the string
  // is emptied in a fraction of the time.
  const std::vector<float> banjo = render_program(105, 55, 240000);
  const std::vector<float> guitar = render_program(25, 55, 240000);
  const std::vector<float> sitar = render_program(104, 55, 240000);

  // Time to fall 30 dB from the peak, in 10 ms steps. Measured 360 ms against
  // the guitar's 1830.
  auto fall_ms = [](const std::vector<float>& buf) {
    const double floor_level = 0.0316 * double(peak(buf));
    for (int w = 0; w < 480; ++w) {
      double sum = 0.0;
      for (size_t i = size_t(w) * 480; i < size_t(w + 1) * 480; ++i)
        sum += double(buf[i]) * double(buf[i]);
      if (std::sqrt(sum / 480.0) < floor_level) return w * 10;
    }
    return 4800;
  };
  REQUIRE(fall_ms(banjo) * 3 < fall_ms(guitar));

  // Brighter than the guitar because of the fingerpick and the bridge-side
  // pluck, and nowhere near the sitar, whose buzz is a different mechanism.
  const double bright_banjo = high_fraction(banjo, 4800, 2000.0, 2048);
  REQUIRE(bright_banjo > 2.0 * high_fraction(guitar, 4800, 2000.0, 2048));
  REQUIRE(bright_banjo < 0.5 * high_fraction(sitar, 4800, 2000.0, 2048));

  // Playable beside the guitar it is derived from.
  REQUIRE(peak(banjo) > 0.7f * peak(guitar));

  // The whole octet names its own patch now, so the family entry is a default
  // nothing reaches.
  for (uint8_t program = 104; program <= 111; ++program) {
    INFO("program " << int(program));
    REQUIRE(&sonare::midi::synth::gm_fallback_patch(0, program) !=
            &sonare::midi::synth::detail::family_patches()[13]);
  }
}

TEST_CASE("SourceResidualSplitter gives a source with no target its share on the default target",
          "[midi][synth]") {
  sonare::midi::SourceResidualSplitter splitter;
  splitter.configure(48000.0, 0.5f);
  std::array<float, 4> fallback_l{};
  std::array<float, 4> fallback_r{};
  std::array<float, 4> lane_l{};
  std::array<float, 4> lane_r{};
  float* fallback[] = {fallback_l.data(), fallback_r.data()};
  float* lane[] = {lane_l.data(), lane_r.data()};
  const MidiInstrumentSourceOutput outputs[] = {{0, fallback}, {101, lane}};
  // Equal dry energy from a lane source and from a source with no lane (a live
  // input, say), whose dry voices render onto the default target.
  splitter.accumulate(101, 1.0f, 1.0f);
  splitter.accumulate(999, 1.0f, 1.0f);
  const std::array<float, 4> residual{1.0f, 1.0f, 1.0f, 1.0f};
  splitter.flush(outputs, std::size(outputs), 4, residual.data(), residual.data(), 0,
                 [](float* const* target, int i, float l, float r) {
                   target[0][i] += l;
                   target[1][i] += r;
                 });
  for (size_t i = 0; i < 4; ++i) {
    REQUIRE(fallback_l[i] == 0.5f);
    REQUIRE(lane_l[i] == 0.5f);
  }
}

TEST_CASE("SourceResidualSplitter carries energy shares across a routing boundary",
          "[midi][synth][gs-physical-review]") {
  sonare::midi::SourceResidualSplitter owners, downstream;
  owners.configure(48000.0, 0.5f);
  downstream.configure(48000.0, 0.5f);
  float fallback_l = 0.0f, fallback_r = 0.0f;
  float first_l = 0.0f, first_r = 0.0f;
  float second_l = 0.0f, second_r = 0.0f;
  float* fallback[] = {&fallback_l, &fallback_r};
  float* first[] = {&first_l, &first_r};
  float* second[] = {&second_l, &second_r};
  const MidiInstrumentSourceOutput outputs[] = {{0, fallback}, {1, first}, {2, second}};
  const float zero = 0.0f, one = 1.0f;
  owners.accumulate(1, 2.0f, 0.0f);
  owners.accumulate(2, 1.0f, 0.0f);
  owners.flush(outputs, 3, 1, &zero, &zero, 0, [](float* const*, int, float, float) {});
  downstream.accumulate_residual_energy(owners, 10.0f);
  downstream.flush(outputs, 3, 1, &one, &one, 0, [](float* const* target, int, float l, float r) {
    target[0][0] += l;
    target[1][0] += r;
  });
  REQUIRE(std::fabs(first_l - 0.8f) < 1e-6f);
  REQUIRE(std::fabs(first_r - 0.8f) < 1e-6f);
  REQUIRE(std::fabs(second_l - 0.2f) < 1e-6f);
  REQUIRE(std::fabs(second_r - 0.2f) < 1e-6f);
  REQUIRE(fallback_l == 0.0f);
}

TEST_CASE("SourceResidualSplitter reuses decayed slots across many track ids", "[midi][synth]") {
  sonare::midi::SourceResidualSplitter splitter;
  // A tau far below one chunk decays each source to nothing by the next flush.
  splitter.configure(48000.0, 1e-6f);
  std::array<float, 4> fallback_l{};
  std::array<float, 4> fallback_r{};
  std::array<float, 4> lane_l{};
  std::array<float, 4> lane_r{};
  float* fallback[] = {fallback_l.data(), fallback_r.data()};
  float* lane[] = {lane_l.data(), lane_r.data()};
  const std::array<float, 4> residual{1.0f, 1.0f, 1.0f, 1.0f};
  const auto add = [](float* const* target, int i, float l, float r) {
    target[0][i] += l;
    target[1][i] += r;
  };
  for (uint32_t id = 1; id <= 3 * sonare::midi::kMaxResidualSources; ++id) {
    const MidiInstrumentSourceOutput outputs[] = {{0, fallback}, {id, lane}};
    fallback_l.fill(0.0f);
    lane_l.fill(0.0f);
    splitter.accumulate(id, 1.0f, 1.0f);
    splitter.flush(outputs, std::size(outputs), 4, residual.data(), residual.data(), 0, add);
    splitter.flush(outputs, std::size(outputs), 4, residual.data(), residual.data(), 0, add);
    INFO("track id " << id);
    // The first flush lands on the fresh source's own lane; the second, after
    // its weight decayed to nothing, on the default target.
    REQUIRE(lane_l[0] == 1.0f);
    REQUIRE(fallback_l[0] == 1.0f);
  }
}

TEST_CASE("NativeSynth and Sf2Player keep a ringing piano board when a note asks for another mix",
          "[midi][synth][piano]") {
  // A held GM 0 note, then a barely audible note from a piano program with a
  // different board mix: the board the held note rings through must not be
  // cleared under it. Program 0 is the control, whose mix matches. The return
  // level does follow the newer mix, so GM 2's x0.06 board still costs the held
  // note its modal colour: measured 0.57-0.88 of the level kept, against
  // 0.23-0.49 when the board was cleared.
  constexpr int kHeld = 24000;
  constexpr int kWindow = 960;  // 20 ms after the second note-on
  const auto exercise = [](auto make_host) {
    const auto render = [&](int other_program, bool strike_other) {
      auto host = make_host();
      host->on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 0)));
      host->on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
      std::vector<float> out = sonare::test::render_left(*host, kHeld);
      if (strike_other) {
        host->on_event(0, event(sonare::midi::make_midi1_program_change(
                              0, 0, static_cast<uint8_t>(other_program))));
        host->on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 96, 1)));
      }
      const std::vector<float> tail = sonare::test::render_left(*host, kWindow);
      out.insert(out.end(), tail.begin(), tail.end());
      return out;
    };
    const std::vector<float> alone = render(0, false);
    const float alone_rms = rms(std::vector<float>(alone.begin() + kHeld, alone.end()));
    REQUIRE(alone_rms > 1.0e-3f);  // non-vacuity: the held note is ringing
    for (const int other : {0, 2, 3}) {
      const std::vector<float> both = render(other, true);
      const float both_rms = rms(std::vector<float>(both.begin() + kHeld, both.end()));
      INFO("other program " << other << " alone rms " << alone_rms << " with the new note "
                            << both_rms);
      CHECK(both_rms > 0.53f * alone_rms);
    }
  };
  SECTION("NativeSynth") {
    exercise([] {
      NativeSynthConfig cfg;
      cfg.use_gm_programs = true;
      auto synth = std::make_unique<NativeSynth>(cfg);
      synth->prepare(kOutRate, 256);
      return synth;
    });
  }
  SECTION("Sf2Player") {
    exercise([] {
      Sf2PlayerConfig cfg;
      cfg.gain = 1.0f;
      auto player = std::make_unique<Sf2Player>(cfg);
      player->prepare(kOutRate, 256);
      return player;
    });
  }
}

TEST_CASE("Sf2Player takes an RPN sent as a MIDI 2.0 Registered Controller", "[midi][synth]") {
  // RPN 00 02 Master Coarse Tuning, data MSB 64 + 5: five semitones up.
  Sf2Player player = make_fallback_player();
  player.on_event(
      0, event(sonare::midi::make_midi2_registered_controller(0, 3, 0, 2, uint32_t{69} << 25)));
  REQUIRE(player.pitch_coarse_tune(3) == 5);
}

TEST_CASE("CC11 attenuates a bowed string exactly as it does every other engine", "[midi][synth]") {
  // Expression reaches every engine through one shared VCA; a second path
  // through the bow speed would make strings swell harder than winds.
  const auto level_db = [](uint8_t program, uint8_t note, uint8_t cc11) {
    NativeSynthConfig cfg;
    cfg.use_gm_programs = true;
    NativeSynth synth(cfg);
    synth.prepare(kOutRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, program)));
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 11, cc11)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, 100)));
    const std::vector<float> out = sonare::test::render_left(synth, 48000);
    return 20.0 * std::log10(std::max(1.0e-12, static_cast<double>(rms(out, 24000))));
  };
  const auto attenuation = [&](uint8_t program, uint8_t note, uint8_t cc11) {
    return level_db(program, note, cc11) - level_db(program, note, 127);
  };
  for (const uint8_t cc11 : {uint8_t{64}, uint8_t{32}}) {
    const double flute = attenuation(73, 72, cc11);
    for (const uint8_t program : {uint8_t{40}, uint8_t{42}}) {
      const double string = attenuation(program, program == 40 ? 67 : 48, cc11);
      INFO("CC11=" << int{cc11} << " program " << int{program} << " string " << string
                   << " dB, flute " << flute << " dB");
      CHECK(std::fabs(string - flute) < 1.0);
    }
  }
}

namespace {

using sonare::midi::Bend32;
using sonare::midi::Control32;
using sonare::midi::Velocity16;
using sonare::midi::synth::ModDestination;
using sonare::midi::synth::ModSource;

/// First 50 ms at the output rate.
constexpr int kOnsetSamples = 2400;

/// A sine through an open filter, so level and pitch read cleanly.
NativeSynthConfig resolution_config() {
  NativeSynthConfig cfg;
  cfg.patch.waveform = VaWaveform::kSine;
  cfg.patch.cutoff_hz = 20000.0f;
  cfg.patch.gain = 0.8f;
  return cfg;
}

/// Raw MIDI 2.0 value halfway between two neighbouring upscaled points.
uint32_t raw_midpoint(uint32_t lo, uint32_t hi) { return lo + (hi - lo) / 2u; }

/// True when @p mid lies strictly between @p lo and @p hi, whichever way they are ordered, and
/// clear of both by a tenth of the step, so rounding noise at either end cannot pass for it.
bool strictly_between(double lo, double mid, double hi) {
  const double margin = 0.1 * std::fabs(hi - lo);
  return lo != hi && (mid - lo) * (hi - mid) > 0.0 && std::fabs(mid - lo) > margin &&
         std::fabs(hi - mid) > margin;
}

/// Onset peak of note 69 after @p before is sent on channel 0.
template <typename Setup>
float onset_peak(const NativeSynthConfig& cfg, Setup before, uint16_t velocity16) {
  NativeSynth synth(cfg);
  synth.prepare(kOutRate, 256);
  before(synth);
  synth.on_event(0, event(sonare::midi::make_midi2_note_on(0, 0, 69, velocity16)));
  return peak(render(synth, kOnsetSamples).left);
}

/// Onset peak with @p controller at the MIDI 2.0 value @p raw.
float cc_onset_peak(const NativeSynthConfig& cfg, uint8_t controller, uint32_t raw) {
  return onset_peak(
      cfg,
      [&](NativeSynth& s) {
        s.on_event(0, event(sonare::midi::make_midi2_control_change(0, 0, controller, raw)));
      },
      Velocity16::from7(100).raw);
}

/// Sounding frequency of note 69 on @p channel bent to the MIDI 2.0 value @p raw, after
/// @p before has run.
template <typename Setup>
double bent_hz(Setup before, uint8_t channel, uint32_t raw) {
  NativeSynth synth(resolution_config());
  synth.prepare(kOutRate, 256);
  before(synth);
  synth.on_event(0, event(sonare::midi::make_midi2_pitch_bend(0, channel, raw)));
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, channel, 69, 100)));
  return estimate_frequency(render(synth, 48000).left, kOutRate, 1024);
}

}  // namespace

TEST_CASE("NativeSynth hears a MIDI 2.0 volume or expression between the 7-bit steps",
          "[midi][synth][midi2]") {
  const NativeSynthConfig cfg = resolution_config();
  for (const uint8_t controller : {uint8_t{7}, uint8_t{11}}) {
    const uint32_t lo = Control32::from7(100).raw;
    const uint32_t hi = Control32::from7(101).raw;
    const float p_lo = cc_onset_peak(cfg, controller, lo);
    const float p_mid = cc_onset_peak(cfg, controller, raw_midpoint(lo, hi));
    const float p_hi = cc_onset_peak(cfg, controller, hi);
    INFO("CC" << int{controller} << " " << p_lo << " / " << p_mid << " / " << p_hi);
    CHECK(strictly_between(p_lo, p_mid, p_hi));
  }
}

TEST_CASE("NativeSynth hears a MIDI 2.0 channel pressure between the 7-bit steps",
          "[midi][synth][midi2]") {
  NativeSynthConfig cfg = resolution_config();
  cfg.patch.mod_matrix.routes[0] = {ModSource::kAftertouch, ModDestination::kAmpGain, -0.5f};
  const auto pressure_peak = [&](uint32_t raw) {
    return onset_peak(
        cfg,
        [&](NativeSynth& s) {
          s.on_event(0, event(sonare::midi::make_midi2_channel_pressure(0, 0, raw)));
        },
        Velocity16::from7(100).raw);
  };
  const uint32_t lo = Control32::from7(64).raw;
  const uint32_t hi = Control32::from7(65).raw;
  const float p_lo = pressure_peak(lo);
  const float p_mid = pressure_peak(raw_midpoint(lo, hi));
  const float p_hi = pressure_peak(hi);
  INFO(p_lo << " / " << p_mid << " / " << p_hi);
  CHECK(strictly_between(p_lo, p_mid, p_hi));
}

TEST_CASE("NativeSynth hears a MIDI 2.0 velocity between the 7-bit steps", "[midi][synth][midi2]") {
  // The voice's own velocity source, routed to pitch so the level laws downstream of it
  // cannot answer for it.
  NativeSynthConfig cfg = resolution_config();
  cfg.patch.mod_matrix.routes[0] = {ModSource::kVelocity, ModDestination::kPitchCents, 1200.0f};
  const auto velocity_hz = [&](uint32_t raw) {
    NativeSynth synth(cfg);
    synth.prepare(kOutRate, 256);
    synth.on_event(0,
                   event(sonare::midi::make_midi2_note_on(0, 0, 69, static_cast<uint16_t>(raw))));
    return estimate_frequency(render(synth, 48000).left, kOutRate, 1024);
  };
  const uint32_t lo = Velocity16::from7(80).raw;
  const uint32_t hi = Velocity16::from7(81).raw;
  const double f_lo = velocity_hz(lo);
  const double f_mid = velocity_hz(raw_midpoint(lo, hi));
  const double f_hi = velocity_hz(hi);
  INFO(f_lo << " / " << f_mid << " / " << f_hi);
  CHECK(strictly_between(f_lo, f_mid, f_hi));
}

TEST_CASE("NativeSynth bends a MIDI 2.0 pitch bend between the 14-bit steps",
          "[midi][synth][midi2]") {
  const uint32_t lo = Bend32::from14(10000).raw;
  const uint32_t hi = Bend32::from14(10001).raw;
  const uint32_t mid = raw_midpoint(lo, hi);

  SECTION("an ordinary channel") {
    // A 48-semitone range widens one 14-bit step to about half a cent.
    const auto wide = [](NativeSynth& s) {
      s.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 101, 0)));
      s.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 100, 0)));
      s.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 6, 48)));
    };
    const double f_lo = bent_hz(wide, 0, lo);
    const double f_mid = bent_hz(wide, 0, mid);
    const double f_hi = bent_hz(wide, 0, hi);
    INFO(f_lo << " / " << f_mid << " / " << f_hi);
    CHECK(strictly_between(f_lo, f_mid, f_hi));
  }

  SECTION("an MPE member channel") {
    // Lower zone of fifteen members; a member bends over the zone's 48 semitones.
    const auto zone = [](NativeSynth& s) {
      s.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 101, 0)));
      s.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 100, 6)));
      s.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 6, 15)));
    };
    const double f_lo = bent_hz(zone, 1, lo);
    const double f_mid = bent_hz(zone, 1, mid);
    const double f_hi = bent_hz(zone, 1, hi);
    INFO(f_lo << " / " << f_mid << " / " << f_hi);
    CHECK(strictly_between(f_lo, f_mid, f_hi));
  }
}

TEST_CASE("NativeSynth takes a MIDI 2.0 Registered Controller 0/0 as the RPN 0/0 bend range",
          "[midi][synth][midi2]") {
  // 12 semitones and 50 cents: Data Entry MSB 12, LSB 50.
  constexpr uint16_t kRange14 = (12u << 7) | 50u;
  const auto render_bent = [](uint8_t channel, bool zoned, bool midi2) {
    NativeSynth synth(resolution_config());
    synth.prepare(kOutRate, 256);
    if (zoned) {
      synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 101, 0)));
      synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 100, 6)));
      synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 6, 15)));
    }
    if (midi2) {
      synth.on_event(0, event(sonare::midi::make_midi2_registered_controller(
                            0, channel, 0, 0, Control32::from14_zero_ext(kRange14).raw)));
    } else {
      synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, channel, 101, 0)));
      synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, channel, 100, 0)));
      synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, channel, 6, 12)));
      synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, channel, 38, 50)));
    }
    synth.on_event(0, event(sonare::midi::make_midi1_pitch_bend(0, channel, 12288)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, channel, 69, 100)));
    return render(synth, 16384).left;
  };

  SECTION("an ordinary channel") {
    const std::vector<float> rpn = render_bent(0, false, false);
    const std::vector<float> rc = render_bent(0, false, true);
    // Half of +12.5 semitones from A4, so the range really moved off its default.
    const double hz = estimate_frequency(rc, kOutRate, 1024);
    CHECK(hz > 440.0 * std::pow(2.0, 6.0 / 12.0));
    CHECK(rc == rpn);
  }

  SECTION("an MPE member channel") {
    const std::vector<float> rpn = render_bent(1, true, false);
    const std::vector<float> rc = render_bent(1, true, true);
    const double hz = estimate_frequency(rc, kOutRate, 1024);
    CHECK(hz > 440.0 * std::pow(2.0, 6.0 / 12.0));
    CHECK(rc == rpn);
  }
}

namespace {

constexpr int kPerNoteSamples = 16384;
constexpr uint16_t kPerNoteVelocity = 0xC000;
constexpr uint32_t kPerNoteBendUpOne = 0xC0000000u;  // half of the default 2 semitones
constexpr uint32_t kPerNoteBendFullUp = 0xFFFFFFFFu;

/// A sine with nothing on the mix bus, so a source track carries its own voice and nothing else,
/// seeded from the note alone, so a note renders the same whichever slot it lands in.
NativeSynthConfig per_note_config() {
  NativeSynthConfig cfg = resolution_config();
  cfg.dc_block = false;
  cfg.patch.retrigger = sonare::midi::synth::SynthRetrigger::kNote;
  return cfg;
}

NativeSynthConfig fm_absolute_pitch_config() {
  NativeSynthConfig cfg = resolution_config();
  cfg.dc_block = false;
  cfg.patch.mode = sonare::midi::synth::SynthEngineMode::kFm;
  cfg.patch.fm.algorithm = sonare::midi::synth::FmAlgorithm::kStack2;
  cfg.patch.fm.ops[0].ratio = 1.0f;
  cfg.patch.fm.ops[0].level = 1.0f;
  cfg.patch.fm.ops[0].env = {0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 100.0f};
  cfg.patch.fm.ops[1].level = 0.0f;
  return cfg;
}

NativeSynth prepared_per_note_synth() {
  NativeSynth synth(per_note_config());
  synth.prepare(kOutRate, 256);
  return synth;
}

MidiEvent on_track(const sonare::midi::Ump& ump, uint32_t track) {
  MidiEvent e = event(ump);
  e.source_track_id = track;
  return e;
}

/// Renders @p synth split by source track (ids 1 and 2) and returns both left legs.
std::array<std::vector<float>, 2> render_tracks(NativeSynth& synth, int num_samples) {
  const size_t n = static_cast<size_t>(num_samples);
  std::vector<float> fallback_l(n, 0.0f);
  std::vector<float> fallback_r(n, 0.0f);
  std::array<std::vector<float>, 2> left{std::vector<float>(n, 0.0f), std::vector<float>(n, 0.0f)};
  std::array<std::vector<float>, 2> right{std::vector<float>(n, 0.0f), std::vector<float>(n, 0.0f)};
  float* fallback[] = {fallback_l.data(), fallback_r.data()};
  float* one[] = {left[0].data(), right[0].data()};
  float* two[] = {left[1].data(), right[1].data()};
  const MidiInstrumentSourceOutput outputs[] = {{0, fallback}, {1, one}, {2, two}};
  REQUIRE(synth.process_source_tracks(outputs, std::size(outputs), 2, num_samples));
  return left;
}

/// Equal-tempered frequency of a (fractional) MIDI note.
double note_hz(double note) { return 440.0 * std::pow(2.0, (note - 69.0) / 12.0); }

bool near_hz(double measured, double expected) {
  return std::fabs(measured / expected - 1.0) < 0.002;
}

/// Sounding frequency of one MIDI 2.0 note after @p setup.
template <typename Setup>
double per_note_hz(Setup setup, uint8_t note, uint8_t attribute_type = 0,
                   uint16_t attribute_data = 0) {
  NativeSynth synth = prepared_per_note_synth();
  setup(synth);
  synth.on_event(0, event(sonare::midi::make_midi2_note_on(0, 0, note, kPerNoteVelocity,
                                                           attribute_type, attribute_data)));
  return estimate_frequency(render(synth, kPerNoteSamples).left, kOutRate, 1024);
}

}  // namespace

TEST_CASE("NativeSynth per-note pitch bend moves only its own note", "[midi][synth][midi2]") {
  NativeSynth pair = prepared_per_note_synth();
  pair.on_event(0, on_track(sonare::midi::make_midi2_note_on(0, 0, 60, kPerNoteVelocity), 1));
  pair.on_event(0, on_track(sonare::midi::make_midi2_note_on(0, 0, 67, kPerNoteVelocity), 2));
  pair.on_event(0,
                event(sonare::midi::make_midi2_per_note_pitch_bend(0, 0, 60, kPerNoteBendUpOne)));
  const auto both = render_tracks(pair, kPerNoteSamples);

  NativeSynth solo = prepared_per_note_synth();
  solo.on_event(0, on_track(sonare::midi::make_midi2_note_on(0, 0, 67, kPerNoteVelocity), 2));
  const auto alone = render_tracks(solo, kPerNoteSamples);

  const double bent = estimate_frequency(both[0], kOutRate, 1024);
  const double other = estimate_frequency(both[1], kOutRate, 1024);
  INFO(bent << " Hz / " << other << " Hz");
  CHECK(near_hz(bent, note_hz(61.0)));
  CHECK(near_hz(other, note_hz(67.0)));
  CHECK(peak(alone[1]) > 0.0f);
  CHECK(both[1] == alone[1]);
}

TEST_CASE("NativeSynth detaches a voice on Per-Note Management D=1", "[midi][synth][midi2]") {
  const auto render_60 = [](bool detach_then_rebend) {
    NativeSynth synth = prepared_per_note_synth();
    synth.on_event(0, on_track(sonare::midi::make_midi2_note_on(0, 0, 60, kPerNoteVelocity), 1));
    synth.on_event(
        0, event(sonare::midi::make_midi2_per_note_pitch_bend(0, 0, 60, kPerNoteBendUpOne)));
    if (detach_then_rebend) {
      synth.on_event(0, event(sonare::midi::make_midi2_per_note_management(0, 0, 60, true, false)));
      synth.on_event(
          0, event(sonare::midi::make_midi2_per_note_pitch_bend(0, 0, 60, kPerNoteBendFullUp)));
    }
    return render_tracks(synth, kPerNoteSamples)[0];
  };
  // The detached voice keeps the bend it had when it was detached.
  const std::vector<float> detached = render_60(true);
  CHECK(near_hz(estimate_frequency(detached, kOutRate, 1024), note_hz(61.0)));
  CHECK(detached == render_60(false));

  // The row itself took the new bend, so the next note on the key sounds it.
  const double next_note = per_note_hz(
      [](NativeSynth& s) {
        s.on_event(
            0, event(sonare::midi::make_midi2_per_note_pitch_bend(0, 0, 60, kPerNoteBendUpOne)));
        s.on_event(0, event(sonare::midi::make_midi2_per_note_management(0, 0, 60, true, false)));
        s.on_event(
            0, event(sonare::midi::make_midi2_per_note_pitch_bend(0, 0, 60, kPerNoteBendFullUp)));
      },
      60);
  INFO(next_note << " Hz");
  CHECK(near_hz(next_note, note_hz(62.0)));
}

TEST_CASE("NativeSynth takes a note's absolute pitch from RPNC #3 and attribute #3",
          "[midi][synth][midi2]") {
  // RPNC #3 Pitch 7.25 on key 60 makes it sound 72.5; attribute #3 Pitch 7.9 on the note-on
  // outranks it (M2-104-UM §7.4.15).
  constexpr uint32_t kPitch725 = (72u << 25) | (1u << 24);
  const auto rpnc = [](NativeSynth& s) {
    s.on_event(0, event(sonare::midi::make_midi2_per_note_controller(0, 0, 60, 3, kPitch725)));
  };
  const double from_rpnc = per_note_hz(rpnc, 60);
  const double from_attribute = per_note_hz(rpnc, 60, 3, 67 * 512 + 256);
  // Per-note bend offsets from the absolute pitch.
  const double bent = per_note_hz(
      [&](NativeSynth& s) {
        rpnc(s);
        s.on_event(
            0, event(sonare::midi::make_midi2_per_note_pitch_bend(0, 0, 60, kPerNoteBendUpOne)));
      },
      60);
  INFO(from_rpnc << " / " << from_attribute << " / " << bent << " Hz");
  CHECK(near_hz(from_rpnc, note_hz(72.5)));
  CHECK(near_hz(from_attribute, note_hz(67.5)));
  CHECK(near_hz(bent, note_hz(73.5)));
}

TEST_CASE("NativeSynth carries an absolute-pitch FM voice without clamping its zone key",
          "[midi][synth][midi2]") {
  const auto carried_frequency = [](uint8_t first, uint16_t first_pitch, uint8_t second) {
    NativeSynth synth(fm_absolute_pitch_config());
    synth.prepare(kOutRate, 256);
    synth.set_articulation(0, sonare::midi::ArticulationMode::kMonoLegato);
    synth.on_event(
        0, event(sonare::midi::make_midi2_note_on(0, 0, first, kPerNoteVelocity, 3, first_pitch)));
    render(synth, 4096);
    // The virtual reference reaches 132 after +12 and must remain outside the
    // ordinary MIDI key range while the sounding pitch becomes note 72.
    synth.on_event(0, event(sonare::midi::make_midi2_note_on(0, 0, second, kPerNoteVelocity)));
    return estimate_frequency(render(synth, kPerNoteSamples).left, kOutRate, 1024);
  };

  const double high = carried_frequency(60, static_cast<uint16_t>(120u * 512u), 72);
  const double low = carried_frequency(60, static_cast<uint16_t>(8u * 512u), 48);
  INFO("upper-bound carry " << high << " Hz, lower-bound carry " << low << " Hz");
  REQUIRE(near_hz(high, note_hz(72.0)));
  REQUIRE(near_hz(low, note_hz(48.0)));
}

TEST_CASE("NativeSynth restores a held key's absolute-pitch attribute after legato return",
          "[midi][synth][midi2]") {
  NativeSynth synth(fm_absolute_pitch_config());
  synth.prepare(kOutRate, 256);
  synth.set_articulation(0, sonare::midi::ArticulationMode::kMonoLegato);

  // Key 60 carries absolute pitch 67. The transient key 64 has no attribute;
  // releasing it must return to the held key's original absolute pitch rather
  // than replacing that attribute with zero.
  synth.on_event(0, event(sonare::midi::make_midi2_note_on(0, 0, 60, kPerNoteVelocity, 3,
                                                           static_cast<uint16_t>(67u * 512u))));
  render(synth, 4096);
  synth.on_event(0, event(sonare::midi::make_midi2_note_on(0, 0, 64, kPerNoteVelocity)));
  render(synth, 4096);
  synth.on_event(0, event(sonare::midi::make_midi2_note_off(0, 0, 64, 0)));
  const double sounding = estimate_frequency(render(synth, kPerNoteSamples).left, kOutRate, 1024);
  INFO("sounding " << sounding << " Hz");
  REQUIRE(near_hz(sounding, note_hz(67.0)));
}

TEST_CASE("NativeSynth scales per-note bend by RC 0/7, absolute and relative",
          "[midi][synth][midi2]") {
  const auto full_bend_after = [](sonare::midi::Ump rc) {
    return per_note_hz(
        [rc](NativeSynth& s) {
          s.on_event(0, event(rc));
          s.on_event(
              0, event(sonare::midi::make_midi2_per_note_pitch_bend(0, 0, 60, kPerNoteBendFullUp)));
        },
        60);
  };
  const double absolute =
      full_bend_after(sonare::midi::make_midi2_registered_controller(0, 0, 0, 7, 12u << 25));
  // Relative +10 semitones on the default 2.
  const double relative = full_bend_after(
      sonare::midi::make_midi2_relative_registered_controller(0, 0, 0, 7, 10u << 25));
  // A delta far below zero saturates at 0 semitones instead of wrapping.
  const double saturated = full_bend_after(
      sonare::midi::make_midi2_relative_registered_controller(0, 0, 0, 7, 0x80000000u));
  INFO(absolute << " / " << relative << " / " << saturated << " Hz");
  CHECK(near_hz(absolute, note_hz(72.0)));
  CHECK(near_hz(relative, note_hz(72.0)));
  CHECK(near_hz(saturated, note_hz(60.0)));
}

TEST_CASE("NativeSynth moves the channel bend range by a relative RC 0/0", "[midi][synth][midi2]") {
  // +10 semitones on the default 2, then a full channel bend up.
  const double hz = per_note_hz(
      [](NativeSynth& s) {
        s.on_event(0, event(sonare::midi::make_midi2_relative_registered_controller(0, 0, 0, 0,
                                                                                    10u << 25)));
        s.on_event(0, event(sonare::midi::make_midi2_pitch_bend(0, 0, 0xFFFFFFFFu)));
      },
      60);
  INFO(hz << " Hz");
  CHECK(near_hz(hz, note_hz(72.0)));
}

TEST_CASE("NativeSynth keeps per-note pitch across Reset All Controllers", "[midi][synth][midi2]") {
  const double hz = per_note_hz(
      [](NativeSynth& s) {
        s.on_event(
            0, event(sonare::midi::make_midi2_per_note_pitch_bend(0, 0, 60, kPerNoteBendUpOne)));
        s.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 121, 0)));
      },
      60);
  INFO(hz << " Hz");
  CHECK(near_hz(hz, note_hz(61.0)));
}

TEST_CASE("NativeSynth counts the per-note and relative messages it does not realise",
          "[midi][synth][midi2]") {
  NativeSynth synth = prepared_per_note_synth();
  REQUIRE(synth.skipped_event_count() == 0);
  // A per-note controller other than pitch, and an assignable one.
  synth.on_event(0, event(sonare::midi::make_midi2_per_note_controller(0, 0, 60, 7, 0x80000000u)));
  synth.on_event(0, event(sonare::midi::make_midi2_assignable_per_note_controller(0, 0, 60, 1, 0)));
  // Relative on a parameter this synth does not hold.
  synth.on_event(
      0, event(sonare::midi::make_midi2_relative_registered_controller(0, 0, 0, 1, 1u << 25)));
  synth.on_event(
      0, event(sonare::midi::make_midi2_relative_assignable_controller(0, 0, 3, 4, 1u << 25)));
  CHECK(synth.skipped_event_count() == 4);
  // Pitch per-note, management and RC 0/7 are realised, not counted.
  synth.on_event(0, event(sonare::midi::make_midi2_per_note_controller(0, 0, 60, 3, 60u << 25)));
  synth.on_event(0, event(sonare::midi::make_midi2_per_note_pitch_bend(0, 0, 60, 0x80000000u)));
  synth.on_event(0, event(sonare::midi::make_midi2_per_note_management(0, 0, 60, true, true)));
  synth.on_event(0, event(sonare::midi::make_midi2_registered_controller(0, 0, 0, 7, 2u << 25)));
  CHECK(synth.skipped_event_count() == 4);
  synth.reset();
  CHECK(synth.skipped_event_count() == 0);
}

TEST_CASE("NativeSynth channel-mode All Notes Off preserves sustain", "[midi][synth]") {
  for (const int controller : {123, 124, 125, 126, 127}) {
    NativeSynthConfig config;
    config.patch.amp_env.release_ms = 5.0f;
    NativeSynth synth(config);
    synth.prepare(kOutRate, 256);

    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, 127)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
    REQUIRE(peak(render(synth, 2048).left) > 0.0f);

    synth.on_event(0, event(sonare::midi::make_midi1_control_change(
                          0, 0, static_cast<uint8_t>(controller), 0)));
    render(synth, 2048);
    INFO("controller " << controller);
    REQUIRE(synth.active_voice_count() > 0);

    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, 0)));
    render(synth, synth.tail_samples() + 1024);
    REQUIRE(synth.active_voice_count() == 0);
  }
}

TEST_CASE("NativeSynth All Sound Off keeps sustain for a new note", "[midi][synth]") {
  NativeSynthConfig config;
  config.patch.amp_env.release_ms = 5.0f;
  NativeSynth synth(config);
  synth.prepare(kOutRate, 256);

  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, 127)));
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  REQUIRE(peak(render(synth, 2048).left) > 0.0f);
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 120, 0)));
  REQUIRE(synth.active_voice_count() == 0);
  REQUIRE(peak(render(synth, 512).left) == 0.0f);

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  REQUIRE(peak(render(synth, 2048).left) > 0.0f);
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
  render(synth, synth.tail_samples() + 1024);
  REQUIRE(synth.active_voice_count() > 0);

  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, 0)));
  render(synth, synth.tail_samples() + 1024);
  REQUIRE(synth.active_voice_count() == 0);
}

TEST_CASE("NativeSynth All Notes Off preserves a sostenuto capture", "[midi][synth]") {
  NativeSynthConfig config;
  config.patch.amp_env.release_ms = 5.0f;
  NativeSynth synth(config);
  synth.prepare(kOutRate, 256);

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  REQUIRE(peak(render(synth, 2048).left) > 0.0f);
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 66, 127)));
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 123, 0)));
  render(synth, synth.tail_samples() + 1024);
  REQUIRE(synth.active_voice_count() > 0);

  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 66, 0)));
  render(synth, synth.tail_samples() + 1024);
  REQUIRE(synth.active_voice_count() == 0);
}

namespace {

using sonare::midi::ControllerAxis;
using sonare::midi::ControllerInput;
using sonare::midi::ControllerProfile;
using sonare::midi::synth::NativeSynthPatch;

/// Tail centroid of note 60 after an optional CC @p cc and an optional CC121.
double excitation_tail_centroid(const NativeSynthPatch& patch, const ControllerProfile& profile,
                                uint8_t cc, uint8_t value, bool move, bool reset) {
  NativeSynthConfig cfg;
  cfg.patch = patch;
  NativeSynth synth(cfg);
  synth.set_controller_profile(profile);
  synth.prepare(kOutRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  render(synth, 12000);
  if (move) synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, cc, value)));
  render(synth, 12000);
  if (reset) synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 121, 0)));
  // Half a second for the excitation to settle before the window: a bowed string's stick-slip
  // regime re-locks over a few hundred milliseconds after its position jumps back, and that
  // transient is not the base the reset restores.
  render(synth, 24000);
  return sonare::test::spectral_centroid(render(synth, 24000).left, 12000);
}

ControllerProfile single_binding_profile(ControllerAxis axis) {
  ControllerProfile profile;
  REQUIRE(profile.bind({ControllerInput::kControlChange, 74, axis}));
  return profile;
}

}  // namespace

TEST_CASE("NativeSynth Reset All Controllers restores a sounding voice's excitation base",
          "[midi][synth]") {
  using sonare::midi::synth::gm_fallback_patch;
  NativeSynthPatch additive{};
  additive.mode = SynthEngineMode::kAdditive;
  additive.gain = 0.8f;
  additive.amp_env.sustain = 1.0f;
  additive.additive.drawbars_b = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 8.0f, 8.0f, 8.0f};
  struct Case {
    const char* label;
    NativeSynthPatch patch;
    ControllerAxis axis;
    uint8_t value;
  };
  const Case cases[] = {
      {"brass brightness", gm_fallback_patch(0, 56), ControllerAxis::kBrightness, 127},
      {"bowed position", gm_fallback_patch(0, 40), ControllerAxis::kPosition, 127},
      {"additive morph", additive, ControllerAxis::kMorph, 127},
  };
  for (const Case& c : cases) {
    INFO(c.label);
    const ControllerProfile profile = single_binding_profile(c.axis);
    const double fresh = excitation_tail_centroid(c.patch, profile, 74, c.value, false, false);
    const double moved = excitation_tail_centroid(c.patch, profile, 74, c.value, true, false);
    const double reset = excitation_tail_centroid(c.patch, profile, 74, c.value, true, true);
    INFO("centroids fresh=" << fresh << " moved=" << moved << " reset=" << reset);
    REQUIRE(std::fabs(moved - fresh) > 0.02 * fresh);
    CHECK(std::fabs(reset - fresh) < 0.2 * std::fabs(moved - fresh));
  }
}

TEST_CASE("NativeSynth channel control on one axis keeps a per-note value on another",
          "[midi][synth]") {
  // Poly pressure is a per-note brightness here; CC2 then moves only the channel's breath.
  ControllerProfile profile;
  REQUIRE(profile.bind({ControllerInput::kControlChange, 74, ControllerAxis::kBrightness}));
  REQUIRE(profile.bind({ControllerInput::kPolyPressure, 0, ControllerAxis::kBrightness}));
  REQUIRE(profile.bind({ControllerInput::kControlChange, 2, ControllerAxis::kExcitation}));
  const NativeSynthPatch patch = sonare::midi::synth::gm_fallback_patch(0, 56);
  const auto tail_centroid = [&](uint8_t channel_brightness, bool per_note_bright) {
    NativeSynthConfig cfg;
    cfg.patch = patch;
    NativeSynth synth(cfg);
    synth.set_controller_profile(profile);
    synth.prepare(kOutRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 74, channel_brightness)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
    render(synth, 4800);
    if (per_note_bright) {
      synth.on_event(0, event(sonare::midi::make_midi1_poly_pressure(0, 0, 60, 127)));
    }
    render(synth, 4800);
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 2, 100)));
    return sonare::test::spectral_centroid(render(synth, 24000).left, 12000);
  };
  const double dark = tail_centroid(0, false);
  const double bright = tail_centroid(127, false);
  const double per_note = tail_centroid(0, true);
  INFO("centroids dark=" << dark << " bright=" << bright << " per-note=" << per_note);
  REQUIRE(std::fabs(bright - dark) > 0.02 * dark);
  CHECK(std::fabs(per_note - bright) < 0.2 * std::fabs(bright - dark));
}

namespace {

using sonare::midi::ArticulationMode;
using sonare::midi::synth::BodyType;

MidiEvent source_event(const sonare::midi::Ump& ump, uint32_t source) {
  MidiEvent ev = event(ump);
  ev.source_track_id = source;
  return ev;
}

NativeSynthConfig legato_config() {
  NativeSynthConfig cfg = resolution_config();
  cfg.dc_block = false;
  cfg.patch.amp_env.attack_ms = 1.0f;
  cfg.patch.amp_env.sustain = 1.0f;
  cfg.patch.amp_env.release_ms = 20.0f;
  return cfg;
}

constexpr uint16_t absolute_pitch(uint8_t note) { return static_cast<uint16_t>(note * 512u); }

}  // namespace

TEST_CASE("a slur across two source tracks ends when both keys are released",
          "[midi][synth][articulation]") {
  for (const ArticulationMode mode :
       {ArticulationMode::kMonoLegato, ArticulationMode::kMonoRetrigger}) {
    NativeSynth synth(legato_config());
    synth.prepare(kOutRate, 256);
    REQUIRE(synth.set_articulation(0, mode));
    synth.on_event(0, source_event(sonare::midi::make_midi1_note_on(0, 0, 60, 100), 101));
    render(synth, 2048);
    synth.on_event(0, source_event(sonare::midi::make_midi1_note_on(0, 0, 64, 100), 202));
    render(synth, 4096);
    // One channel is one monophonic line whichever source pressed the key.
    CHECK(synth.active_voice_count() == 1);
    synth.on_event(0, source_event(sonare::midi::make_midi1_note_off(0, 0, 64, 0), 202));
    render(synth, 2048);
    synth.on_event(0, source_event(sonare::midi::make_midi1_note_off(0, 0, 60, 0), 101));
    render(synth, 24000);
    CAPTURE(static_cast<int>(mode));
    CHECK(synth.active_voice_count() == 0);
    CHECK(peak(render(synth, 2048).left) == 0.0f);
  }
}

TEST_CASE("a single source still slurs and returns to its own held key",
          "[midi][synth][articulation]") {
  NativeSynth synth(legato_config());
  synth.prepare(kOutRate, 256);
  synth.set_articulation(0, ArticulationMode::kMonoLegato);
  synth.on_event(0, source_event(sonare::midi::make_midi1_note_on(0, 0, 60, 100), 101));
  render(synth, 2048);
  synth.on_event(0, source_event(sonare::midi::make_midi1_note_on(0, 0, 67, 100), 101));
  render(synth, 2048);
  synth.on_event(0, source_event(sonare::midi::make_midi1_note_off(0, 0, 67, 0), 101));
  const double returned = estimate_frequency(render(synth, 8192).left, kOutRate, 1024);
  uint64_t fallbacks = 1;
  REQUIRE(synth.legato_fallback_count(&fallbacks));
  CHECK(fallbacks == 0);
  CHECK(synth.active_voice_count() == 1);
  CHECK(near_hz(returned, note_hz(60.0)));
}

TEST_CASE("legato reach is judged on the sounding pitch, not the binding key",
          "[midi][synth][midi2][articulation]") {
  const auto play = [](uint8_t low_key, uint8_t high_key) {
    NativeSynthConfig cfg;
    cfg.dc_block = false;
    cfg.patch.mode = SynthEngineMode::kPipeOrgan;
    cfg.patch.amp_env.sustain = 1.0f;
    NativeSynth synth(cfg);
    synth.prepare(kOutRate, 256);
    synth.set_articulation(0, ArticulationMode::kMonoLegato);
    synth.on_event(0, event(sonare::midi::make_midi2_note_on(0, 0, low_key, kPerNoteVelocity, 3,
                                                             absolute_pitch(60))));
    render(synth, 4096);
    synth.on_event(0, event(sonare::midi::make_midi2_note_on(0, 0, high_key, kPerNoteVelocity, 3,
                                                             absolute_pitch(72))));
    render(synth, 4096);
    synth.on_event(0, event(sonare::midi::make_midi2_note_off(0, 0, high_key, 0)));
    const float tail = peak(render(synth, 8192).left);
    uint64_t fallbacks = 0;
    REQUIRE(synth.legato_fallback_count(&fallbacks));
    return std::make_tuple(fallbacks, synth.active_voice_count(), tail);
  };
  // Binding keys 0/24 and 60/84 name the same sounding pitches 60 -> 72 -> 60.
  const auto low_binding = play(0, 24);
  const auto high_binding = play(60, 84);
  CHECK(std::get<0>(low_binding) == 0);
  CHECK(std::get<1>(low_binding) == 1);
  CHECK(std::get<2>(low_binding) > 0.0f);
  CHECK(std::get<0>(high_binding) == std::get<0>(low_binding));
  CHECK(std::get<1>(high_binding) == std::get<1>(low_binding));
}

TEST_CASE("a carried absolute-pitch voice tracks the filter and body of its sounding key",
          "[midi][synth][midi2][articulation]") {
  const auto tail_centroid = [](bool carried, NativeSynthConfig cfg) {
    cfg.dc_block = false;
    NativeSynth synth(cfg);
    synth.prepare(kOutRate, 256);
    synth.set_articulation(0, ArticulationMode::kMonoLegato);
    if (carried) {
      synth.on_event(0, event(sonare::midi::make_midi2_note_on(0, 0, 60, kPerNoteVelocity, 3,
                                                               absolute_pitch(60))));
      render(synth, 4096);
    }
    synth.on_event(0, event(sonare::midi::make_midi2_note_on(0, 0, 72, kPerNoteVelocity, 3,
                                                             absolute_pitch(60))));
    render(synth, 4096);
    return sonare::test::spectral_centroid(render(synth, 16384).left, 0);
  };

  SECTION("filter key tracking") {
    NativeSynthConfig cfg = legato_config();
    cfg.patch.waveform = VaWaveform::kSaw;
    cfg.patch.cutoff_hz = 1000.0f;
    cfg.patch.key_track = 1.0f;
    NativeSynthConfig doubled = cfg;
    doubled.patch.cutoff_hz = 2000.0f;
    const double fresh = tail_centroid(false, cfg);
    const double slurred = tail_centroid(true, cfg);
    const double octave_up = tail_centroid(false, doubled);
    INFO("fresh " << fresh << " slurred " << slurred << " cutoff an octave up " << octave_up);
    REQUIRE(std::fabs(octave_up - fresh) > 0.05 * fresh);
    CHECK(std::fabs(slurred - fresh) < 0.2 * std::fabs(octave_up - fresh));
  }
  SECTION("wood-tube body") {
    NativeSynthConfig cfg = legato_config();
    cfg.patch.waveform = VaWaveform::kSaw;
    cfg.patch.body = BodyType::kWoodTube;
    cfg.patch.body_mix = 1.0f;
    NativeSynthConfig dry = cfg;
    dry.patch.body_mix = 0.0f;
    const double fresh = tail_centroid(false, cfg);
    const double slurred = tail_centroid(true, cfg);
    const double without_body = tail_centroid(false, dry);
    INFO("fresh " << fresh << " slurred " << slurred << " no body " << without_body);
    REQUIRE(std::fabs(without_body - fresh) > 0.01 * fresh);
    CHECK(std::fabs(slurred - fresh) < 0.2 * std::fabs(without_body - fresh));
  }
}

TEST_CASE("a carried absolute-pitch voice keeps its pitch and its glide origin",
          "[midi][synth][midi2][articulation]") {
  NativeSynthConfig cfg = legato_config();
  cfg.patch.glide_ms = 500.0f;
  NativeSynth synth(cfg);
  synth.prepare(kOutRate, 256);
  synth.set_articulation(0, ArticulationMode::kMonoLegato);
  synth.on_event(0, event(sonare::midi::make_midi2_note_on(0, 0, 60, kPerNoteVelocity, 3,
                                                           absolute_pitch(60))));
  render(synth, 8192);
  synth.on_event(0, event(sonare::midi::make_midi2_note_on(0, 0, 72, kPerNoteVelocity, 3,
                                                           absolute_pitch(60))));
  const std::vector<float> carry = render(synth, 4096).left;
  CHECK(near_hz(estimate_frequency(carry, kOutRate, 256), note_hz(60.0)));

  synth.on_event(0, event(sonare::midi::make_midi2_note_off(0, 0, 72, 0)));
  synth.on_event(0, event(sonare::midi::make_midi2_note_off(0, 0, 60, 0)));
  render(synth, 24000);
  REQUIRE(synth.active_voice_count() == 0);
  // The glide origin of the next note is where the phrase sounded, not its binding key.
  synth.on_event(
      0, event(sonare::midi::make_midi2_note_on(0, 0, 0, kPerNoteVelocity, 3, absolute_pitch(60))));
  const std::vector<float> fresh = render(synth, 4096).left;
  CHECK(near_hz(estimate_frequency(fresh, kOutRate, 256), note_hz(60.0)));
}

namespace {

using sonare::midi::make_midi1_note_off;
using sonare::midi::make_midi1_note_on;
using sonare::midi::MidiEvent;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::SynthEngineMode;

struct TailMeasurement {
  int declared = 0;
  float peak_after_declared = 0.0f;
  int last_above_floor = -1;
  int nonfinite = 0;
};

TailMeasurement render_reed(bool dc_block, int sample_rate) {
  NativeSynthConfig config;
  config.dc_block = dc_block;
  config.gain = 1.0f;
  config.patch.mode = SynthEngineMode::kReed;
  config.patch.cutoff_hz = 20000.0f;
  config.patch.amp_env.attack_ms = 5.0f;
  config.patch.amp_env.sustain = 1.0f;
  config.patch.amp_env.release_ms = 10.0f;
  config.patch.reed.release_ms = 5000.0f;

  NativeSynth synth(config);
  synth.prepare(sample_rate, 256);
  MidiEvent event;
  event.ump = make_midi1_note_on(0, 0, 72, 100);
  synth.on_event(0, event);

  std::array<float, 256> left{};
  std::array<float, 256> right{};
  float* channels[2] = {left.data(), right.data()};
  for (int block = 0; block < 94; ++block) synth.process(channels, 2, 256);

  event.ump = make_midi1_note_off(0, 0, 72, 0);
  synth.on_event(0, event);
  TailMeasurement result;
  result.declared = synth.tail_samples();
  const int frame_count = result.declared + sample_rate / 2;
  for (int offset = 0; offset < frame_count; offset += 256) {
    const int count = std::min(256, frame_count - offset);
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    synth.process(channels, 2, count);
    for (int i = 0; i < count; ++i) {
      const float value = std::max(std::fabs(left[static_cast<size_t>(i)]),
                                   std::fabs(right[static_cast<size_t>(i)]));
      const int frame = offset + i;
      if (!std::isfinite(value)) ++result.nonfinite;
      if (frame >= result.declared) {
        result.peak_after_declared = std::max(result.peak_after_declared, value);
      }
      if (value > 1.0e-5f) result.last_above_floor = frame;
    }
  }
  return result;
}

}  // namespace

TEST_CASE("NativeSynth tail includes the mix-bus DC blocker decay", "[boundary]") {
  for (const int sample_rate : {48000, 96000}) {
    const TailMeasurement with_dc = render_reed(true, sample_rate);
    const TailMeasurement without_dc = render_reed(false, sample_rate);
    INFO("sample rate " << sample_rate << ", declared " << with_dc.declared << ", DC peak "
                        << with_dc.peak_after_declared << ", no-DC peak "
                        << without_dc.peak_after_declared);
    CHECK(with_dc.nonfinite == 0);
    CHECK(without_dc.nonfinite == 0);
    CHECK(with_dc.last_above_floor <= with_dc.declared);
    CHECK(without_dc.last_above_floor <= without_dc.declared);
  }
}

TEST_CASE("Sf2Player synth-fallback tail includes the mix-bus DC blocker decay", "[boundary]") {
  // Both hosts run the same blocker after everything else on the bus, so turning it on must
  // lengthen each bound by the same decay.
  for (const int sample_rate : {48000, 96000}) {
    const auto sf2_tail = [sample_rate](bool dc_block) {
      Sf2PlayerConfig config;
      config.dc_block = dc_block;
      Sf2Player player(config);  // no SoundFont: the synth fallback voices every program
      player.prepare(sample_rate, 256);
      return player.tail_samples();
    };
    const auto native_tail = [sample_rate](bool dc_block) {
      NativeSynthConfig config;
      config.dc_block = dc_block;
      config.use_gm_programs = true;
      NativeSynth synth(config);
      synth.prepare(sample_rate, 256);
      return synth.tail_samples();
    };
    const int sf2_on = sf2_tail(true);
    const int sf2_off = sf2_tail(false);
    const int native_on = native_tail(true);
    const int native_off = native_tail(false);
    INFO("sample rate " << sample_rate << ", Sf2Player " << sf2_off << " -> " << sf2_on
                        << ", NativeSynth " << native_off << " -> " << native_on);
    REQUIRE(sf2_on < std::numeric_limits<int>::max());
    REQUIRE(native_on < std::numeric_limits<int>::max());
    CHECK(native_on - native_off > 0);
    CHECK(sf2_on - sf2_off == native_on - native_off);
  }
}

TEST_CASE("GM fallback tail covers every source across supported rates", "[boundary]") {
  using sonare::midi::synth::EnvelopeTimeScales;
  using sonare::midi::synth::gm_drum_kit_max_tail_samples;
  using sonare::midi::synth::gm_fallback_max_tail_samples;
  using sonare::midi::synth::native_patch_tail_samples;

  for (const double sample_rate : {8000.0, 11025.0, 24000.0, 32000.0, 44100.0, 48000.0, 53777.0,
                                   88200.0, 96000.0, 176400.0, 192000.0, 384000.0}) {
    const int64_t bound = gm_fallback_max_tail_samples(sample_rate, 1.0f, 1.0f, 1.0f);
    const int64_t drum_bound = gm_drum_kit_max_tail_samples(sample_rate, 1.0f, 1.0f, 1.0f);
    INFO("sample rate " << sample_rate << ", bound " << bound);
    REQUIRE(bound > 0);
    REQUIRE(bound < std::numeric_limits<int64_t>::max());
    REQUIRE(drum_bound > 0);
    REQUIRE(drum_bound < std::numeric_limits<int64_t>::max());
    REQUIRE(drum_bound <= bound);
    const auto covered = [&](const sonare::midi::synth::NativeSynthPatch& patch) {
      return bound >= native_patch_tail_samples(patch, sample_rate, EnvelopeTimeScales{}, nullptr);
    };
    const auto drum_covered = [&](const sonare::midi::synth::NativeSynthPatch& patch) {
      return drum_bound >=
             native_patch_tail_samples(patch, sample_rate, EnvelopeTimeScales{}, nullptr);
    };
    for (const auto& patch : sonare::midi::synth::detail::family_patches()) {
      REQUIRE(covered(patch));
    }
    for (const auto& patch : sonare::midi::synth::detail::drum_note_table()) {
      REQUIRE(covered(patch));
      REQUIRE(drum_covered(patch));
    }
    for (size_t kit = 0; kit < sonare::midi::synth::kGsDrumKits.size(); ++kit) {
      for (int note = 0; note < 128; ++note) {
        auto patch = sonare::midi::synth::gm_fallback_drum_patch(static_cast<uint8_t>(note));
        sonare::midi::synth::apply_gs_drum_kit(
            patch.percussion, patch.amp_env, static_cast<uint8_t>(kit), static_cast<uint8_t>(note));
        REQUIRE(covered(patch));
        REQUIRE(drum_covered(patch));
      }
    }
    const auto& overrides = sonare::midi::synth::detail::program_overrides();
    const auto* patches = sonare::midi::synth::detail::program_override_patches(overrides);
    for (std::size_t i = 0; i < sonare::midi::synth::detail::kProgramOverrideCount; ++i) {
      REQUIRE(covered(patches[i]));
    }
  }
}

TEST_CASE("a released voice frees its slot once the source is silent, not at the envelope tail",
          "[midi][synth][release]") {
  using sonare::midi::synth::gm_fallback_patch;
  using sonare::midi::synth::NativeSynthConfig;
  // Electric grand, dulcimer, glockenspiel, vibraphone, accordion and synth voice all carry an
  // amplitude release of seconds over a source that falls silent far sooner.
  for (const uint8_t program : {2, 9, 11, 15, 21, 54}) {
    NativeSynthConfig cfg;
    cfg.patch = gm_fallback_patch(0, program);
    NativeSynth synth(cfg);
    synth.prepare(sonare::test::kRate, 256);
    synth.on_event(0, sonare::test::event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
    (void)sonare::test::render_left(synth, static_cast<int>(0.4 * sonare::test::kRate));
    REQUIRE(synth.active_voice_count() == 1);
    synth.on_event(0, sonare::test::event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
    int freed_at = -1;
    const int block = 256;
    const int limit = static_cast<int>(3.5 * sonare::test::kRate);
    for (int done = 0; done < limit && freed_at < 0; done += block) {
      (void)sonare::test::render_left(synth, block);
      if (synth.active_voice_count() == 0) freed_at = done;
    }
    INFO("program " << static_cast<int>(program));
    CHECK(freed_at >= 0);
  }
}

TEST_CASE("the accordion amplitude path rises into its held level without a step",
          "[midi][synth][accordion]") {
  using sonare::midi::synth::gm_fallback_patch;
  for (const uint8_t variation : {0, 8}) {
    const auto& patch = gm_fallback_patch(variation, 21, sonare::midi::synth::GsToneMap::kSc55);
    INFO("variation " << static_cast<int>(variation));
    REQUIRE(patch.mode == sonare::midi::synth::SynthEngineMode::kFreeReed);
    CHECK((patch.amp_env.sustain >= 1.0f || patch.amp_env.decay_ms > 0.0f));
  }
}

TEST_CASE(
    "a released voice is cut at the same note-relative sample whatever the onset or block size",
    "[midi][synth][release]") {
  using sonare::midi::synth::gm_fallback_patch;
  using sonare::midi::synth::NativeSynthConfig;
  using sonare::midi::synth::SynthRetrigger;
  // One note, released after half a second, rendered with the onset at two offsets and the host
  // blocks at two sizes. Events land on exact samples, so a block boundary falls differently
  // against the note each time.
  const auto render_note = [](uint8_t program, long onset, int block, int* active_at_end) {
    NativeSynthConfig cfg;
    cfg.patch = gm_fallback_patch(0, program);
    cfg.patch.retrigger = SynthRetrigger::kNote;
    NativeSynth synth(cfg);
    synth.prepare(sonare::test::kRate, block);
    // Reverb, chorus and delay sends off, so only the voice is compared.
    for (const uint8_t cc : {91, 93, 94}) {
      synth.on_event(0, sonare::test::event(sonare::midi::make_midi1_control_change(0, 0, cc, 0)));
    }
    const long release_at = onset + 24000;
    const long total = onset + 3 * 48000;
    std::vector<float> out;
    long pos = 0;
    bool noted = false;
    bool released = false;
    while (pos < total) {
      if (!noted && pos >= onset) {
        synth.on_event(0, sonare::test::event(sonare::midi::make_midi1_note_on(0, 0, 48, 100)));
        noted = true;
      }
      if (!released && pos >= release_at) {
        synth.on_event(0, sonare::test::event(sonare::midi::make_midi1_note_off(0, 0, 48, 0)));
        released = true;
      }
      long n = std::min<long>(block, total - pos);
      if (!noted) n = std::min(n, onset - pos);
      if (noted && !released) n = std::min(n, release_at - pos);
      std::vector<float> left(static_cast<size_t>(n));
      std::vector<float> right(static_cast<size_t>(n));
      float* chans[2] = {left.data(), right.data()};
      synth.process(chans, 2, static_cast<int>(n));
      out.insert(out.end(), left.begin(), left.end());
      pos += n;
    }
    *active_at_end = synth.active_voice_count();
    return std::vector<float>(out.begin() + onset, out.end());
  };
  // Karplus-Strong bass, Synth Voice, Warm Pad and Saw Lead: the cores whose output falls silent
  // after release, and the voices whose amplitude release outlasts them.
  for (const uint8_t program : {33, 54, 89, 81}) {
    int active_a = -1, active_b = -1, active_c = -1;
    const std::vector<float> reference = render_note(program, 0, 64, &active_a);
    const std::vector<float> shifted = render_note(program, 12345, 64, &active_b);
    const std::vector<float> reblocked = render_note(program, 40001, 777, &active_c);
    INFO("program " << static_cast<int>(program));
    REQUIRE(reference.size() == shifted.size());
    REQUIRE(reference.size() == reblocked.size());
    CHECK(reference == shifted);
    CHECK(reference == reblocked);
    // The slot is freed by the end of the three seconds in every case.
    CHECK(active_a == 0);
    CHECK(active_b == 0);
    CHECK(active_c == 0);
  }
}
