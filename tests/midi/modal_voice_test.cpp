/// @file modal_voice_test.cpp
/// @brief Modal / additive / percussion NativeSynth modes (midi/synth/
///        modal_voice, additive_voice, percussion_voice): physical
///        partial-ratio checks (glockenspiel vs marimba bars, drawbar
///        pitches, membrane Rayleigh modes), mallet velocity -> brightness,
///        organ key click, the descending drum pitch and one-shot drum
///        determinism through the GM fallback kit.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "core/fft.h"
#include "midi/midi_event.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::gm_fallback_drum_patch;
using sonare::midi::synth::gm_fallback_patch;
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
                                int num_samples, uint8_t channel = 0) {
  NativeSynthConfig cfg;
  cfg.patch = patch;
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, channel, note, velocity)));
  return render_left(synth, num_samples);
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

/// Hann-windowed power spectrum of buf[from, from+kFft).
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

/// Refines the strongest bin near @p freq_hz (+-4 bins) to a parabolic peak
/// frequency; returns 0 when the local peak carries no energy.
double refine_peak_hz(const std::vector<double>& power, double freq_hz) {
  const int centre = static_cast<int>(std::lround(freq_hz / kRate * kFft));
  int best = -1;
  double best_power = 0.0;
  for (int b = centre - 4; b <= centre + 4; ++b) {
    if (b > 1 && b < static_cast<int>(power.size()) - 1 &&
        power[static_cast<size_t>(b)] > best_power) {
      best_power = power[static_cast<size_t>(b)];
      best = b;
    }
  }
  if (best < 0 || best_power <= 0.0) return 0.0;
  // Parabolic interpolation on log power.
  const double l = std::log(power[static_cast<size_t>(best - 1)] + 1.0e-30);
  const double c = std::log(power[static_cast<size_t>(best)] + 1.0e-30);
  const double r = std::log(power[static_cast<size_t>(best + 1)] + 1.0e-30);
  const double denom = l - 2.0 * c + r;
  const double delta = denom != 0.0 ? 0.5 * (l - r) / denom : 0.0;
  return (static_cast<double>(best) + delta) * kRate / kFft;
}

/// Total power within +-3 bins of @p freq_hz.
double band_power(const std::vector<double>& power, double freq_hz) {
  const int centre = static_cast<int>(std::lround(freq_hz / kRate * kFft));
  double acc = 0.0;
  for (int b = centre - 3; b <= centre + 3; ++b) {
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

}  // namespace

TEST_CASE("glockenspiel and marimba bars ring at their physical mode ratios",
          "[midi][synth][modal]") {
  const double f0 = 440.0;

  // Uniform bar: 1 : 2.756 : 5.404.
  const NativeSynthPatch& glock = gm_fallback_patch(0, 9);
  REQUIRE(glock.mode == SynthEngineMode::kModal);
  const std::vector<float> glock_tone = render_patch(glock, 69, 120, 16384);
  const std::vector<double> glock_power = power_spectrum(glock_tone, 1024);
  for (const double ratio : {1.0, 2.756, 5.404}) {
    const double peak = refine_peak_hz(glock_power, f0 * ratio);
    REQUIRE(peak > 0.0);
    REQUIRE(std::fabs(peak / (f0 * ratio) - 1.0) < 0.01);
  }

  // Deep-arch tuned bar: 1 : 4 : 10.
  const NativeSynthPatch& marimba = gm_fallback_patch(0, 12);
  REQUIRE(marimba.mode == SynthEngineMode::kModal);
  const std::vector<float> mar_tone = render_patch(marimba, 57, 120, 16384);
  const std::vector<double> mar_power = power_spectrum(mar_tone, 512);
  const double mar_f0 = 220.0;
  for (const double ratio : {1.0, 4.0, 10.0}) {
    const double peak = refine_peak_hz(mar_power, mar_f0 * ratio);
    REQUIRE(peak > 0.0);
    REQUIRE(std::fabs(peak / (mar_f0 * ratio) - 1.0) < 0.01);
  }
  // The marimba bar must NOT carry the uniform-bar 2.756 partial.
  REQUIRE(band_power(mar_power, mar_f0 * 2.756) < 0.02 * band_power(mar_power, mar_f0));
}

TEST_CASE("mallet velocity controls strike brightness", "[midi][synth][modal]") {
  const NativeSynthPatch& vibes = gm_fallback_patch(0, 11);
  REQUIRE(vibes.mode == SynthEngineMode::kModal);
  const std::vector<float> hard = render_patch(vibes, 69, 127, 12000);
  const std::vector<float> soft = render_patch(vibes, 69, 25, 12000);
  // Upper-mode (>= 4*f0) energy share grows with velocity.
  const double hard_high = high_band_fraction(hard, 512, 440.0 * 3.0);
  const double soft_high = high_band_fraction(soft, 512, 440.0 * 3.0);
  REQUIRE(hard_high > 2.0 * soft_high);
}

TEST_CASE("modal rendering is deterministic and damps at note-off", "[midi][synth][modal]") {
  const NativeSynthPatch& glock = gm_fallback_patch(0, 9);
  const std::vector<float> first = render_patch(glock, 76, 100, 8192);
  const std::vector<float> second = render_patch(glock, 76, 100, 8192);
  float peak = 0.0f;
  for (float s : first) peak = std::max(peak, std::fabs(s));
  REQUIRE(peak > 0.005f);
  REQUIRE(first == second);

  // Held vs released at 0.25 s: the damp + release must kill the ring.
  NativeSynthConfig cfg;
  cfg.patch = glock;
  NativeSynth held_synth(cfg);
  held_synth.prepare(kRate, 256);
  held_synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 76, 100)));
  const std::vector<float> held = render_left(held_synth, 48000);

  NativeSynth damped_synth(cfg);
  damped_synth.prepare(kRate, 256);
  damped_synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 76, 100)));
  std::vector<float> head(12000, 0.0f);
  std::vector<float> head_r(12000, 0.0f);
  float* chans[2] = {head.data(), head_r.data()};
  damped_synth.process(chans, 2, 12000);
  damped_synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 76, 0)));
  const std::vector<float> tail = render_left(damped_synth, 36000);

  const float held_late = rms(held, 38400, 48000);
  const float damped_late = rms(tail, 26400, 36000);  // same absolute window
  REQUIRE(held_late > 0.0f);
  REQUIRE(damped_late < 0.1f * held_late);
}

TEST_CASE("the drawbar organ stacks its registration partials with a key click",
          "[midi][synth][additive]") {
  const NativeSynthPatch& organ = gm_fallback_patch(0, 16);
  REQUIRE(organ.mode == SynthEngineMode::kAdditive);
  const double f0 = 220.0;
  const std::vector<float> tone = render_patch(organ, 57, 100, 24000);

  // Registration pitches present (16' = 0.5, 8' = 1, 5-1/3' = 1.5, 4' = 2);
  // unpulled drawbars (2-2/3' = 3) carry no energy.
  const std::vector<double> power = power_spectrum(tone, 8192);
  REQUIRE(band_power(power, f0 * 0.5) > 0.01 * band_power(power, f0));
  REQUIRE(band_power(power, f0 * 1.5) > 0.01 * band_power(power, f0));
  REQUIRE(band_power(power, f0 * 2.0) > 0.005 * band_power(power, f0));
  REQUIRE(band_power(power, f0 * 3.0) < 0.002 * band_power(power, f0));

  // Tonewheels sustain: no decay between 0.2 s and 0.45 s.
  REQUIRE(rms(tone, 9600, 12000) > 0.7f * rms(tone, 4800, 7200));

  // Key click: rendering the same patch with key_click = 0 leaves identical
  // partials (same seed), so the difference isolates the click — a clear
  // transient in the first 10 ms that has died out by 50 ms.
  NativeSynthPatch no_click = organ;
  no_click.additive.key_click = 0.0f;
  const std::vector<float> clean = render_patch(no_click, 57, 100, 24000);
  std::vector<float> click(tone.size());
  for (size_t i = 0; i < tone.size(); ++i) click[i] = tone[i] - clean[i];
  REQUIRE(rms(click, 0, 480) > 0.002f);
  REQUIRE(rms(click, 0, 480) > 10.0f * rms(click, 2400, 2880));
}

TEST_CASE("the GM kick pitch falls after the strike", "[midi][synth][percussion]") {
  const NativeSynthPatch& kick = gm_fallback_drum_patch(36);
  REQUIRE(kick.mode == SynthEngineMode::kPercussion);
  REQUIRE(kick.one_shot);
  const std::vector<float> hit = render_patch(kick, 36, 127, 16384, /*channel=*/9);
  float peak = 0.0f;
  for (float s : hit) peak = std::max(peak, std::fabs(s));
  REQUIRE(peak > 0.05f);

  // Compare the strongest low partial early vs late: the tension-release
  // envelope must land the late pitch noticeably below the early pitch.
  auto window_peak_hz = [&](size_t from) {
    std::vector<float> window(hit.begin() + static_cast<long>(from),
                              hit.begin() + static_cast<long>(from) + 4096);
    window.resize(kFft, 0.0f);
    const std::vector<double> power = power_spectrum(window, 0);
    double best_power = 0.0;
    int best = 0;
    // Search below 200 Hz.
    const int limit = static_cast<int>(std::lround(200.0 / kRate * kFft));
    for (int b = 2; b < limit; ++b) {
      if (power[static_cast<size_t>(b)] > best_power) {
        best_power = power[static_cast<size_t>(b)];
        best = b;
      }
    }
    return static_cast<double>(best) * kRate / kFft;
  };
  const double early = window_peak_hz(0);
  const double late = window_peak_hz(6000);
  REQUIRE(early > 1.15 * late);
}

TEST_CASE("the GM snare layers shell modes under the wire band", "[midi][synth][percussion]") {
  const NativeSynthPatch& snare = gm_fallback_drum_patch(38);
  REQUIRE(snare.mode == SynthEngineMode::kPercussion);
  const std::vector<float> hit = render_patch(snare, 38, 127, 16384, /*channel=*/9);
  const std::vector<double> power = power_spectrum(hit, 0);
  // Shell fundamental at the pinned 185 Hz shows up against the noise floor
  // (the pitch drop has settled within the analysis window)...
  REQUIRE(band_power(power, 185.0) > 4.0 * band_power(power, 120.0));
  // ...and the wire crack carries broadband energy around its band centre.
  REQUIRE(high_band_fraction(hit, 0, 1000.0) > 0.3);
}

TEST_CASE("GM drum strikes are one-shot and deterministic", "[midi][synth][percussion]") {
  for (const uint8_t note : {36, 38, 42, 49}) {
    const NativeSynthPatch& patch = gm_fallback_drum_patch(note);
    REQUIRE(patch.mode == SynthEngineMode::kPercussion);
    REQUIRE(patch.one_shot);
    const std::vector<float> first = render_patch(patch, note, 110, 8192, /*channel=*/9);
    const std::vector<float> second = render_patch(patch, note, 110, 8192, /*channel=*/9);
    REQUIRE(first == second);

    // One-shot: an immediate note-off must not choke the strike.
    NativeSynthConfig cfg;
    cfg.patch = patch;
    NativeSynth synth(cfg);
    synth.prepare(kRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 9, note, 110)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 9, note, 0)));
    const std::vector<float> choked = render_left(synth, 8192);
    REQUIRE(rms(choked, 0, 4096) > 0.8f * rms(first, 0, 4096));
  }
}
