/// @file voice_polish_test.cpp
/// @brief P14 realism polish on NativeSynth: body/formant resonance (guitar
///        body knock, note-tracked wood tube), seeded per-voice stereo
///        spread, and the mix-bus glue (gain-neutral drive + DC blocker).

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
using sonare::midi::synth::BodyType;
using sonare::midi::synth::gm_fallback_patch;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::SynthEngineMode;
using sonare::midi::synth::VaWaveform;

constexpr double kRate = 48000.0;
constexpr int kFft = 8192;

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

struct StereoOut {
  std::vector<float> left;
  std::vector<float> right;
};

StereoOut render_notes(const NativeSynthConfig& cfg, std::initializer_list<uint8_t> notes,
                       int num_samples) {
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  for (uint8_t note : notes) {
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, 110)));
  }
  StereoOut out;
  out.left.assign(static_cast<size_t>(num_samples), 0.0f);
  out.right.assign(static_cast<size_t>(num_samples), 0.0f);
  float* chans[2] = {out.left.data(), out.right.data()};
  synth.process(chans, 2, num_samples);
  return out;
}

std::vector<float> render_note(const NativeSynthPatch& patch, uint8_t note, int num_samples) {
  NativeSynthConfig cfg;
  cfg.patch = patch;
  return render_notes(cfg, {note}, num_samples).left;
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

/// Power inside [freq_hz - half_width, freq_hz + half_width] as a fraction of
/// the total — measures a resonant boost independent of overall level.
double band_fraction(const std::vector<float>& buf, size_t from, double freq_hz,
                     double half_width_hz) {
  const std::vector<double> power = power_spectrum(buf, from);
  const int lo = static_cast<int>(std::lround((freq_hz - half_width_hz) / kRate * kFft));
  const int hi = static_cast<int>(std::lround((freq_hz + half_width_hz) / kRate * kFft));
  double band = 0.0;
  double total = 0.0;
  for (int b = 1; b < static_cast<int>(power.size()); ++b) {
    total += power[static_cast<size_t>(b)];
    if (b >= lo && b <= hi) band += power[static_cast<size_t>(b)];
  }
  return total > 0.0 ? band / total : 0.0;
}

/// A flat sustained white-noise patch (filter bypassed): an ideal probe for
/// the body resonator's frequency response.
NativeSynthPatch noise_patch() {
  NativeSynthPatch p;
  p.waveform = VaWaveform::kNoise;
  p.cutoff_hz = 20000.0f;
  p.amp_env.attack_ms = 1.0f;
  p.amp_env.sustain = 1.0f;
  return p;
}

}  // namespace

TEST_CASE("the guitar body adds its low modes deterministically", "[midi][synth][body]") {
  NativeSynthPatch dry = noise_patch();
  NativeSynthPatch body = dry;
  body.body = BodyType::kGuitar;
  body.body_mix = 0.6f;

  const std::vector<float> dry_tone = render_note(dry, 69, 16384);
  const std::vector<float> body_tone = render_note(body, 69, 16384);
  // The 100 Hz air mode must stick out of the flat noise floor.
  const double dry_band = band_fraction(dry_tone, 8192, 100.0, 12.0);
  const double body_band = band_fraction(body_tone, 8192, 100.0, 12.0);
  REQUIRE(body_band > 1.3 * dry_band);
  // Seeded noise + fixed resonator tables: bit-identical renders.
  REQUIRE(body_tone == render_note(body, 69, 16384));
}

TEST_CASE("the wood-tube body tracks the played note", "[midi][synth][body]") {
  NativeSynthPatch dry = noise_patch();
  NativeSynthPatch tube = dry;
  tube.body = BodyType::kWoodTube;
  tube.body_mix = 0.6f;

  // The tube is cut for its bar: its resonance must sit on each note's
  // fundamental (A4 = 440 Hz, A5 = 880 Hz), not at a fixed frequency.
  for (const auto& [note, f0] :
       {std::pair<uint8_t, double>{69, 440.0}, std::pair<uint8_t, double>{81, 880.0}}) {
    const std::vector<float> dry_tone = render_note(dry, note, 16384);
    const std::vector<float> tube_tone = render_note(tube, note, 16384);
    REQUIRE(band_fraction(tube_tone, 8192, f0, 15.0) >
            1.4 * band_fraction(dry_tone, 8192, f0, 15.0));
  }
}

TEST_CASE("GM fallbacks carry the P14 body/spread voicing", "[midi][synth][body]") {
  // Acoustic guitars resonate; solid-body electrics do not.
  REQUIRE(gm_fallback_patch(0, 25).body == BodyType::kGuitar);    // steel
  REQUIRE(gm_fallback_patch(0, 24).body == BodyType::kGuitar);    // nylon
  REQUIRE(gm_fallback_patch(0, 27).body == BodyType::kNone);      // electric
  REQUIRE(gm_fallback_patch(0, 46).body == BodyType::kGuitar);    // harp
  REQUIRE(gm_fallback_patch(0, 12).body == BodyType::kWoodTube);  // marimba
  REQUIRE(gm_fallback_patch(0, 13).body == BodyType::kWoodTube);  // xylophone
  // Section/pad families spread their voices; plucked solo patches stay put.
  REQUIRE(gm_fallback_patch(0, 89).stereo_spread > 0.0f);  // synth pad
  REQUIRE(gm_fallback_patch(0, 48).stereo_spread > 0.0f);  // string ensemble
  REQUIRE(gm_fallback_patch(0, 25).stereo_spread == 0.0f);
}

TEST_CASE("stereo spread scatters voices across the image", "[midi][synth][spread]") {
  NativeSynthPatch patch;
  patch.waveform = VaWaveform::kSaw;
  patch.amp_env.attack_ms = 1.0f;
  patch.amp_env.sustain = 1.0f;

  NativeSynthConfig centred;
  centred.patch = patch;
  NativeSynthConfig spread = centred;
  spread.patch.stereo_spread = 0.7f;

  // spread = 0: every voice stays centre-panned, L == R.
  const StereoOut mono = render_notes(centred, {48, 52, 55, 60}, 8192);
  std::vector<float> diff(mono.left.size());
  for (size_t i = 0; i < diff.size(); ++i) diff[i] = mono.left[i] - mono.right[i];
  REQUIRE(rms(diff, 0, diff.size()) < 1.0e-6f);

  // spread > 0: the seeded per-voice pans decorrelate the chord...
  const StereoOut wide = render_notes(spread, {48, 52, 55, 60}, 8192);
  for (size_t i = 0; i < diff.size(); ++i) diff[i] = wide.left[i] - wide.right[i];
  REQUIRE(rms(diff, 0, diff.size()) > 0.05f * rms(wide.left, 0, wide.left.size()));
  // ...deterministically (seeded by voice index/note, not RNG).
  const StereoOut again = render_notes(spread, {48, 52, 55, 60}, 8192);
  REQUIRE(wide.left == again.left);
  REQUIRE(wide.right == again.right);
}

TEST_CASE("bus drive glues the mix without leaving the gain-neutral bound", "[midi][synth][bus]") {
  NativeSynthPatch patch;
  patch.waveform = VaWaveform::kSaw;
  patch.unison = 5;
  patch.detune_cents = 14.0f;
  patch.amp_env.attack_ms = 1.0f;
  patch.amp_env.sustain = 1.0f;

  NativeSynthConfig clean;
  clean.patch = patch;
  NativeSynthConfig driven = clean;
  driven.bus_drive = 0.9f;

  const StereoOut clean_out = render_notes(clean, {48, 52, 55, 60}, 8192);
  const StereoOut driven_out = render_notes(driven, {48, 52, 55, 60}, 8192);
  REQUIRE(clean_out.left != driven_out.left);
  // tanh(g*x)/g caps each leg at 1/g = 1/(1 + 3*0.9).
  float peak = 0.0f;
  for (float s : driven_out.left) peak = std::max(peak, std::fabs(s));
  for (float s : driven_out.right) peak = std::max(peak, std::fabs(s));
  REQUIRE(peak > 0.01f);
  REQUIRE(peak < 0.3f);
  const StereoOut again = render_notes(driven, {48, 52, 55, 60}, 8192);
  REQUIRE(driven_out.left == again.left);
  REQUIRE(driven_out.right == again.right);
}

TEST_CASE("the bus DC blocker is wired and keeps the output centred", "[midi][synth][bus]") {
  // A kick-like percussion hit (low membrane modes + a noise burst).
  NativeSynthPatch patch;
  patch.mode = SynthEngineMode::kPercussion;
  patch.percussion.num_modes = 3;
  patch.percussion.base_freq_hz = 50.0f;
  patch.percussion.mode_decay_s = 0.4f;
  patch.percussion.pitch_drop = 1.5f;
  patch.percussion.noise_gain = 0.3f;
  patch.percussion.noise_decay_ms = 30.0f;
  NativeSynthConfig blocked;
  blocked.patch = patch;
  NativeSynthConfig raw = blocked;
  raw.dc_block = false;

  const StereoOut blocked_out = render_notes(blocked, {36}, 24000);
  const StereoOut raw_out = render_notes(raw, {36}, 24000);
  REQUIRE(blocked_out.left != raw_out.left);  // the blocker is in the path
  double mean = 0.0;
  for (float s : blocked_out.left) mean += s;
  mean /= static_cast<double>(blocked_out.left.size());
  REQUIRE(std::fabs(mean) < 1.0e-3);
}
