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
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/audio_fixtures.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::gm_fallback_drum_patch;
using sonare::midi::synth::gm_fallback_patch;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::midi::synth::SynthEngineMode;

using sonare::test::kFft;
using sonare::test::kRate;

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

template <typename Instrument>
std::vector<float> render_left(Instrument& instrument, int num_samples) {
  std::vector<float> left(static_cast<size_t>(num_samples), 0.0f);
  std::vector<float> right(static_cast<size_t>(num_samples), 0.0f);
  float* chans[2] = {left.data(), right.data()};
  instrument.process(chans, 2, num_samples);
  return left;
}

/// Sf2Player with no SoundFont: every note resolves through the GM fallback.
Sf2Player make_fallback_player() {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  Sf2Player player(cfg);
  player.prepare(kRate, 256);
  return player;
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

using sonare::test::power_spectrum;

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

TEST_CASE("drawbar percussion is switched off by the harmonic, not by its level",
          "[midi][synth][additive]") {
  // The off value has to reproduce the render exactly, or every value fitted
  // with the percussion in place is measured against a moved baseline.
  const NativeSynthPatch& organ = gm_fallback_patch(0, 16);
  REQUIRE(organ.additive.percussion_harmonic == 0);
  NativeSynthPatch loud = organ;
  loud.additive.percussion_level = 1.0f;
  loud.additive.percussion_decay_ms = 4000.0f;

  const std::vector<float> base = render_patch(organ, 57, 100, 24000);
  const std::vector<float> unchanged = render_patch(loud, 57, 100, 24000);
  REQUIRE(base == unchanged);
}

TEST_CASE("drawbar percussion is a decaying tone at the harmonic it names",
          "[midi][synth][additive]") {
  const NativeSynthPatch& organ = gm_fallback_patch(0, 16);
  const double f0 = 220.0;  // note 57
  const std::vector<float> plain = render_patch(organ, 57, 100, 24000);

  // Same seed, same registration, so subtracting the plain render leaves the
  // percussion alone — the registration's own energy at the same harmonic
  // cancels with it.
  auto shot = [&](int harmonic) {
    NativeSynthPatch p = organ;
    p.additive.percussion_harmonic = harmonic;
    p.additive.percussion_decay_ms = 100.0f;
    const std::vector<float> tone = render_patch(p, 57, 100, 24000);
    std::vector<float> diff(tone.size());
    for (size_t i = 0; i < tone.size(); ++i) diff[i] = tone[i] - plain[i];
    return diff;
  };

  const std::vector<float> second = shot(2);
  const std::vector<float> third = shot(3);
  const std::vector<double> second_power = power_spectrum(second, 0);
  const std::vector<double> third_power = power_spectrum(third, 0);
  REQUIRE(band_power(second_power, f0 * 2.0) > 100.0 * band_power(second_power, f0 * 3.0));
  REQUIRE(band_power(third_power, f0 * 3.0) > 100.0 * band_power(third_power, f0 * 2.0));

  // Four time constants of 100 ms is 35 dB, and the tonewheels under it do not
  // decay at all — so a shot that did not decay would read as no shot.
  REQUIRE(rms(second, 0, 2400) > 0.01f);
  REQUIRE(rms(second, 19200, 21600) < 0.05f * rms(second, 0, 2400));
}

TEST_CASE("the shipped percussive organ's ping is spent inside a beat", "[midi][synth][additive]") {
  // Every other case here sets its own decay, so none of them sees the bank's.
  // Reading the field as a time to inaudibility rather than as a time constant
  // stretches the ping sevenfold, and at that length it is still within 6 dB of
  // its peak 200 ms in, where the reference's is 33 dB down.
  const NativeSynthPatch shipped = gm_fallback_patch(0, 17);
  NativeSynthPatch plain = shipped;
  plain.additive.percussion_harmonic = 0;
  const int n = static_cast<int>(kRate) / 2;
  const std::vector<float> with = render_patch(shipped, 57, 100, n);
  const std::vector<float> without = render_patch(plain, 57, 100, n);
  std::vector<float> shot(with.size());
  for (size_t i = 0; i < with.size(); ++i) shot[i] = with[i] - without[i];

  const size_t window = static_cast<size_t>(kRate) / 100;  // 10 ms
  const size_t late = static_cast<size_t>(kRate) / 5;      // 200 ms in
  const float first = rms(shot, 0, window);
  REQUIRE(first > 0.01f);
  REQUIRE(rms(shot, late, late + window) < 0.05f * first);
}

TEST_CASE("drawbar percussion sounds once per phrase and recharges when the keys are up",
          "[midi][synth][additive]") {
  NativeSynthConfig cfg;
  cfg.patch = gm_fallback_patch(0, 16);
  cfg.patch.additive.percussion_harmonic = 2;
  cfg.patch.additive.percussion_decay_ms = 150.0f;
  cfg.patch.additive.percussion_level = 0.8f;
  const int hold = static_cast<int>(kRate) / 2;  // the second key arrives 0.5 s in

  // Two notes half a second apart, the second either struck under the first or
  // after it. The percussion is the difference against the same phrase with the
  // percussion off, which shares every seed with it.
  auto phrase = [&](bool legato, bool percussion) {
    NativeSynthConfig c = cfg;
    if (!percussion) c.patch.additive.percussion_harmonic = 0;
    NativeSynth synth(c);
    synth.prepare(kRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 57, 100)));
    std::vector<float> out = render_left(synth, hold);
    if (!legato) synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 57, 0)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 64, 100)));
    const std::vector<float> tail = render_left(synth, hold);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
  };
  auto shot = [&](bool legato) {
    const std::vector<float> with = phrase(legato, true);
    const std::vector<float> without = phrase(legato, false);
    std::vector<float> diff(with.size());
    for (size_t i = 0; i < with.size(); ++i) diff[i] = with[i] - without[i];
    return diff;
  };

  const size_t window = static_cast<size_t>(kRate) / 20;  // 50 ms after each key
  const std::vector<float> under = shot(true);
  const std::vector<float> after = shot(false);
  const float first = rms(under, 0, window);
  REQUIRE(first > 0.01f);
  REQUIRE(rms(after, 0, window) == first);  // the phrases share their first key

  // Held under the first key the charge is spent, so what is left at the second
  // key is only the first shot's tail — three time constants down. Released and
  // struck again it recharges, and the second key sounds like the first.
  const size_t second_key = static_cast<size_t>(hold);
  REQUIRE(rms(under, second_key, second_key + window) < 0.2f * first);
  REQUIRE(rms(after, second_key, second_key + window) > 0.8f * first);
}

TEST_CASE("the GM percussive organ carries its percussion through the fallback bank",
          "[midi][sf2][synth][additive]") {
  // The GM path runs through Sf2Player rather than NativeSynth, so the channel
  // bit exists on both hosts: wiring only NativeSynth leaves the percussion
  // silent for every MIDI file.
  const NativeSynthPatch& percussive = gm_fallback_patch(0, 17);
  REQUIRE(percussive.mode == SynthEngineMode::kAdditive);
  REQUIRE(percussive.additive.percussion_harmonic == 3);

  auto phrase = [](bool legato) {
    Sf2Player player = make_fallback_player();
    player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 17)));
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 57, 100)));
    std::vector<float> out = render_left(player, static_cast<int>(kRate) / 2);
    if (!legato) player.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 57, 0)));
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 64, 100)));
    const std::vector<float> tail = render_left(player, static_cast<int>(kRate) / 2);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
  };

  // The percussion is the only thing that differs between the two phrases at
  // the second key: same patch, same registration, same note.
  const std::vector<float> under = phrase(true);
  const std::vector<float> after = phrase(false);
  const size_t second_key = static_cast<size_t>(kRate) / 2;
  const size_t window = static_cast<size_t>(kRate) / 20;
  const double at_second_key = band_power(power_spectrum(after, second_key), 3.0 * 329.6);
  const double under_first = band_power(power_spectrum(under, second_key), 3.0 * 329.6);
  REQUIRE(rms(after, second_key, second_key + window) > 0.0f);
  REQUIRE(at_second_key > 4.0 * under_first);
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

TEST_CASE("the promoted chromatic-percussion programs voice their physical cores",
          "[midi][synth][modal]") {
  // Celesta (8), Music Box (10), Tubular Bells (14) resolve to the modal bank;
  // Dulcimer (15) is a hammered (Karplus-Strong) string.
  REQUIRE(gm_fallback_patch(0, 8).mode == SynthEngineMode::kModal);
  REQUIRE(gm_fallback_patch(0, 10).mode == SynthEngineMode::kModal);
  REQUIRE(gm_fallback_patch(0, 14).mode == SynthEngineMode::kModal);
  REQUIRE(gm_fallback_patch(0, 15).mode == SynthEngineMode::kKarplusStrong);

  // Each lands its fundamental on the played key and rings audibly.
  for (const uint8_t program : {uint8_t{8}, uint8_t{10}, uint8_t{14}, uint8_t{15}}) {
    const NativeSynthPatch& patch = gm_fallback_patch(0, program);
    const std::vector<float> tone = render_patch(patch, 69, 110, 24000);
    float peak = 0.0f;
    for (float s : tone) peak = std::max(peak, std::fabs(s));
    REQUIRE(peak > 0.01f);
    const std::vector<double> power = power_spectrum(tone, 1024);
    const double f0 = refine_peak_hz(power, 440.0);
    REQUIRE(f0 > 0.0);
    REQUIRE(std::fabs(f0 / 440.0 - 1.0) < 0.03);
    // Deterministic bounce.
    REQUIRE(render_patch(patch, 69, 110, 8192) == render_patch(patch, 69, 110, 8192));
  }
}

TEST_CASE("tubular bells ring on after note-off", "[midi][synth][modal]") {
  const NativeSynthPatch& bells = gm_fallback_patch(0, 14);
  NativeSynthConfig cfg;
  cfg.patch = bells;
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 110)));
  std::vector<float> head(6000, 0.0f);
  std::vector<float> head_r(6000, 0.0f);
  float* chans[2] = {head.data(), head_r.data()};
  synth.process(chans, 2, 6000);
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 69, 0)));
  // A struck bell keeps sounding well after the key is lifted (a glockenspiel's
  // short damper would kill it within ~0.1 s).
  const std::vector<float> tail = render_left(synth, 48000);
  REQUIRE(rms(tail, 24000, 48000) > 0.05f * rms(head, 0, 6000));
}

TEST_CASE("the promoted pitched-percussion programs track the key and honour note-off",
          "[midi][synth][percussion]") {
  // Tinkle Bell (112) .. Reverse Cymbal (119) all voice the percussion core as
  // melodic programs — key-tracked (except the unpitched reverse cymbal), never
  // one-shot.
  for (uint8_t program = 112; program <= 119; ++program) {
    const NativeSynthPatch& patch = gm_fallback_patch(0, program);
    REQUIRE(patch.mode == SynthEngineMode::kPercussion);
    REQUIRE_FALSE(patch.one_shot);
    const std::vector<float> tone = render_patch(patch, 69, 110, 24000);
    float peak = 0.0f;
    for (float s : tone) peak = std::max(peak, std::fabs(s));
    REQUIRE(peak > 0.01f);
    REQUIRE(render_patch(patch, 69, 110, 8192) == render_patch(patch, 69, 110, 8192));
  }

  // Steel Drums (114) is a melodic lead: its tone tracks the struck key.
  const NativeSynthPatch& steel = gm_fallback_patch(0, 114);
  const std::vector<float> low = render_patch(steel, 57, 110, 24000);   // A3 = 220 Hz
  const std::vector<float> high = render_patch(steel, 69, 110, 24000);  // A4 = 440 Hz
  const double low_f0 = refine_peak_hz(power_spectrum(low, 512), 220.0);
  const double high_f0 = refine_peak_hz(power_spectrum(high, 512), 440.0);
  REQUIRE(low_f0 > 0.0);
  REQUIRE(high_f0 > 0.0);
  REQUIRE(std::fabs(low_f0 / 220.0 - 1.0) < 0.05);
  REQUIRE(std::fabs(high_f0 / 440.0 - 1.0) < 0.05);
}
