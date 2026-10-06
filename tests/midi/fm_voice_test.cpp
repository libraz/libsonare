/// @file fm_voice_test.cpp
/// @brief FM operator stack (midi/synth/fm_voice): deterministic rendering,
///        spectral tolerance bands for the e-piano / bell / brass fallback
///        patches (harmonic 1:1 stacks vs inharmonic bell ratios), feedback
///        spectral enrichment, velocity -> modulation index brightness and
///        key-rate-scaled decay.

#include "midi/synth/fm_voice.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <limits>
#include <set>
#include <vector>

#include "core/fft.h"
#include "midi/midi_event.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"
#include "support/audio_fixtures.h"
#include "support/midi_render.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::FmAlgorithm;
using sonare::midi::synth::FmOperatorParams;
using sonare::midi::synth::gm_fallback_patch;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::SynthEngineMode;

using sonare::test::event;
using sonare::test::kFft;
using sonare::test::kRate;
using sonare::test::render_left;

float rms(const std::vector<float>& buf, size_t from, size_t to) {
  double acc = 0.0;
  size_t n = 0;
  for (size_t i = from; i < to && i < buf.size(); ++i) {
    acc += static_cast<double>(buf[i]) * buf[i];
    ++n;
  }
  return n > 0 ? static_cast<float>(std::sqrt(acc / static_cast<double>(n))) : 0.0f;
}

float absolute_peak(const std::vector<float>& buf) {
  float peak = 0.0f;
  for (float sample : buf) peak = std::max(peak, std::fabs(sample));
  return peak;
}

NativeSynthPatch fm_base_patch();

void set_fm_operator(FmOperatorParams& op, float level, float delay_ms, float decay_ms,
                     float sustain) {
  op.ratio = 1.0f;
  op.level = level;
  op.env = {delay_ms, 0.0f, 0.0f, decay_ms, sustain, 50.0f};
}

NativeSynthConfig fm_lifecycle_config(FmAlgorithm algorithm) {
  NativeSynthConfig cfg;
  cfg.patch = fm_base_patch();
  cfg.patch.one_shot = true;
  // Keep the wrapper VCA alive so the FM carriers, rather than the shared TVA,
  // determine when a one-shot voice can release its pool slot.
  cfg.patch.amp_env = {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 50.0f};
  cfg.patch.fm.algorithm = algorithm;
  for (FmOperatorParams& op : cfg.patch.fm.ops) set_fm_operator(op, 0.0f, 0.0f, 0.0f, 0.0f);
  return cfg;
}

using sonare::test::power_spectrum;

/// Fraction of spectral power OUTSIDE +-3 bins of the f0 harmonic grid
/// (skipping the lowest bins). ~0 for a harmonic tone, large for bells.
double inharmonicity(const std::vector<float>& buf, size_t from, double f0) {
  const std::vector<double> power = power_spectrum(buf, from);
  std::set<int> harmonic_bins;
  for (int k = 1; k * f0 < 0.5 * kRate; ++k) {
    const int centre = static_cast<int>(std::lround(k * f0 / kRate * kFft));
    for (int b = centre - 3; b <= centre + 3; ++b) harmonic_bins.insert(b);
  }
  double harmonic = 0.0;
  double other = 0.0;
  for (int b = 8; b < static_cast<int>(power.size()); ++b) {
    if (harmonic_bins.count(b) > 0) {
      harmonic += power[static_cast<size_t>(b)];
    } else {
      other += power[static_cast<size_t>(b)];
    }
  }
  const double total = harmonic + other;
  return total > 0.0 ? other / total : 1.0;
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

/// A bypass-filter FM test patch wrapper.
NativeSynthPatch fm_base_patch() {
  NativeSynthPatch p;
  p.mode = SynthEngineMode::kFm;
  p.cutoff_hz = 20000.0f;
  p.amp_env.attack_ms = 1.0f;
  p.amp_env.sustain = 1.0f;
  return p;
}

std::vector<float> render_patch(const NativeSynthPatch& patch, uint8_t note, uint8_t velocity,
                                int num_samples) {
  NativeSynthConfig cfg;
  cfg.patch = patch;
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, velocity)));
  return render_left(synth, num_samples);
}

}  // namespace

TEST_CASE("FM rendering is deterministic", "[midi][synth][fm]") {
  const NativeSynthPatch& patch = gm_fallback_patch(0, 4);  // FM e-piano
  REQUIRE(patch.mode == SynthEngineMode::kFm);
  const std::vector<float> first = render_patch(patch, 60, 100, 4096);
  const std::vector<float> second = render_patch(patch, 60, 100, 4096);
  float peak = 0.0f;
  for (float s : first) peak = std::max(peak, std::fabs(s));
  REQUIRE(peak > 0.01f);
  REQUIRE(first == second);
}

TEST_CASE("1:1 operator stacks stay harmonic, bell ratios go inharmonic", "[midi][synth][fm]") {
  // Harmonic: 2-op 1:1 stack at moderate index.
  NativeSynthPatch harmonic = fm_base_patch();
  harmonic.fm.algorithm = FmAlgorithm::kStack2;
  harmonic.fm.ops[0].ratio = 1.0f;
  harmonic.fm.ops[0].level = 1.0f;
  harmonic.fm.ops[0].env = {0.0f, 1.0f, 0.0f, 60.0f, 1.0f, 120.0f};
  harmonic.fm.ops[1].ratio = 1.0f;
  harmonic.fm.ops[1].level = 1.5f;
  harmonic.fm.ops[1].env = {0.0f, 1.0f, 0.0f, 60.0f, 1.0f, 120.0f};
  const std::vector<float> tone = render_patch(harmonic, 69, 127, 24000);
  REQUIRE(inharmonicity(tone, 12000, 440.0) < 0.02);

  // Inharmonic: an FM bell (3.5 ratio sidebands). The chromatic-percussion
  // programs voice the dedicated modal/KS cores now, so this exercises the FM
  // bell recipe directly rather than through a GM program.
  NativeSynthPatch bell = fm_base_patch();
  bell.amp_env = {0.0f, 1.0f, 0.0f, 2500.0f, 0.0f, 600.0f};
  bell.fm.algorithm = FmAlgorithm::kStack2;
  bell.fm.ops[0].ratio = 1.0f;
  bell.fm.ops[0].level = 1.0f;
  bell.fm.ops[0].env = {0.0f, 1.0f, 0.0f, 2500.0f, 0.0f, 600.0f};
  bell.fm.ops[1].ratio = 3.5f;  // inharmonic bell partials
  bell.fm.ops[1].level = 3.0f;
  bell.fm.ops[1].env = {0.0f, 1.0f, 0.0f, 900.0f, 0.0f, 400.0f};
  const std::vector<float> ring = render_patch(bell, 69, 127, 24000);
  REQUIRE(inharmonicity(ring, 4096, 440.0) > 0.3);
}

TEST_CASE("the feedback operator enriches the brass spectrum", "[midi][synth][fm]") {
  // Synth Brass 2, the FM brass patch whose fit kept the feedback operator.
  const NativeSynthPatch& brass = gm_fallback_patch(0, 63);
  REQUIRE(brass.mode == SynthEngineMode::kFm);
  REQUIRE(brass.fm.ops[2].feedback > 0.0f);

  NativeSynthPatch no_feedback = brass;
  no_feedback.fm.ops[2].feedback = 0.0f;

  const std::vector<float> with_fb = render_patch(brass, 57, 127, 24000);
  const std::vector<float> without_fb = render_patch(no_feedback, 57, 127, 24000);
  const double fb_high = high_band_fraction(with_fb, 12000, 1500.0);
  const double plain_high = high_band_fraction(without_fb, 12000, 1500.0);
  REQUIRE(fb_high > 1.2 * plain_high);
}

TEST_CASE("velocity scales the modulation index (brightness)", "[midi][synth][fm]") {
  const NativeSynthPatch& ep = gm_fallback_patch(0, 4);  // FM e-piano
  const std::vector<float> loud = render_patch(ep, 60, 127, 12000);
  const std::vector<float> soft = render_patch(ep, 60, 30, 12000);
  // Brightness, not just level: compare spectral balance above ~1.5 kHz.
  const double loud_high = high_band_fraction(loud, 2048, 1500.0);
  const double soft_high = high_band_fraction(soft, 2048, 1500.0);
  REQUIRE(loud_high > 2.0 * soft_high);
}

TEST_CASE("key-rate scaling shortens decay up the keyboard", "[midi][synth][fm]") {
  const NativeSynthPatch& ep = gm_fallback_patch(0, 4);  // FM e-piano, krs > 0
  auto decay_ratio = [](const std::vector<float>& buf) {
    // Level after 3 s relative to the initial strike window. The probe sits at
    // about one time constant of the patch's own decay, so the two notes have
    // separated; read at half a second neither has fallen far enough to compare.
    const float early = rms(buf, 480, 4800);
    const float late = rms(buf, 144000, 148800);
    return early > 0.0f ? late / early : 0.0f;
  };
  const std::vector<float> low_note = render_patch(ep, 36, 110, 148800);
  const std::vector<float> high_note = render_patch(ep, 96, 110, 148800);
  // The high note must have decayed appreciably further by then.
  REQUIRE(decay_ratio(high_note) < 0.6f * decay_ratio(low_note));
}

TEST_CASE("FM one-shots start audibly, ignore note-off, and retire after the carrier decays",
          "[midi][synth][fm]") {
  // Stack2 deliberately leaves its modulator alive at sustain. It is not a
  // carrier, so it must not keep the one-shot slot open after op0 has ended.
  NativeSynthConfig cfg = fm_lifecycle_config(FmAlgorithm::kStack2);
  set_fm_operator(cfg.patch.fm.ops[0], 1.0f, 0.0f, 20.0f, 0.0f);
  // An incorrectly forwarded note-off would replace the short natural decay
  // with this long release and leave the carrier active at the final check.
  cfg.patch.fm.ops[0].env.release_ms = 2000.0f;
  set_fm_operator(cfg.patch.fm.ops[1], 1.0f, 0.0f, 0.0f, 1.0f);

  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const std::vector<float> onset = render_left(synth, 256);
  REQUIRE(absolute_peak(onset) > 0.01f);
  REQUIRE(synth.active_voice_count() == 1);

  // One-shot release ignores note-off. The carrier is still in its decay here,
  // so the voice must remain active after the event.
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
  render_left(synth, 256);
  REQUIRE(synth.active_voice_count() == 1);

  render_left(synth, static_cast<int>(kRate));
  CHECK(synth.active_voice_count() == 0);
}

TEST_CASE("FM one-shot lifetime follows both carriers in additive algorithms",
          "[midi][synth][fm]") {
  auto check_algorithm = [](FmAlgorithm algorithm, int delayed_carrier) {
    NativeSynthConfig cfg = fm_lifecycle_config(algorithm);
    set_fm_operator(cfg.patch.fm.ops[0], 1.0f, 0.0f, 1.0f, 0.0f);
    set_fm_operator(cfg.patch.fm.ops[delayed_carrier], 1.0f, 50.0f, 200.0f, 0.0f);

    NativeSynth synth(cfg);
    synth.prepare(kRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));

    // op0 is already over, while the second carrier is still in its delay.
    render_left(synth, 2048);
    REQUIRE(synth.active_voice_count() == 1);
    render_left(synth, 4800);
    REQUIRE(synth.active_voice_count() == 1);

    render_left(synth, static_cast<int>(kRate));
    CHECK(synth.active_voice_count() == 0);
  };

  SECTION("kAdd2") { check_algorithm(FmAlgorithm::kAdd2, 1); }
  SECTION("kPair2x2") { check_algorithm(FmAlgorithm::kPair2x2, 2); }
}

TEST_CASE("FM one-shot does not retire on a zero crossing while a carrier sustains",
          "[midi][synth][fm]") {
  NativeSynthConfig cfg = fm_lifecycle_config(FmAlgorithm::kStack2);
  set_fm_operator(cfg.patch.fm.ops[0], 1.0f, 0.0f, 0.0f, 1.0f);

  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const std::vector<float> first = render_left(synth, 1);
  // The carrier starts at phase zero, so this first sample is a zero crossing.
  REQUIRE(first.size() == 1);
  CHECK(std::fabs(first.front()) < 1.0e-7f);
  REQUIRE(synth.active_voice_count() == 1);

  const std::vector<float> sustained = render_left(synth, 4096);
  CHECK(absolute_peak(sustained) > 0.01f);
  CHECK(synth.active_voice_count() == 1);
}

TEST_CASE("silent FM carriers do not keep one-shot slots alive", "[midi][synth][fm]") {
  NativeSynthConfig cfg = fm_lifecycle_config(FmAlgorithm::kAdd2);
  set_fm_operator(cfg.patch.fm.ops[0], 1.0f, 0.0f, 20.0f, 0.0f);
  set_fm_operator(cfg.patch.fm.ops[1], 0.0f, 0.0f, 0.0f, 1.0f);
  set_fm_operator(cfg.patch.fm.ops[2], 1.0f, 0.0f, 0.0f, 1.0f);

  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  REQUIRE(absolute_peak(render_left(synth, 256)) > 0.01f);
  REQUIRE(synth.active_voice_count() == 1);
  render_left(synth, static_cast<int>(kRate));
  CHECK(synth.active_voice_count() == 0);
}

TEST_CASE("FM one-shot tail includes a delayed finite carrier at note zero",
          "[midi][synth][fm][fm-tail]") {
  NativeSynthConfig cfg = fm_lifecycle_config(FmAlgorithm::kStack2);
  cfg.patch.amp_env.release_ms = 1.0f;
  set_fm_operator(cfg.patch.fm.ops[0], 1.0f, 100.0f, 100.0f, 0.0f);
  cfg.patch.fm.ops[0].key_rate_scale = 1.0f;
  // The modulator sustains, but it is not audible by itself and must not make
  // the tail infinite.
  set_fm_operator(cfg.patch.fm.ops[1], 1.0f, 0.0f, 0.0f, 1.0f);

  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  const int64_t wrapper_tail = sonare::midi::synth::DahdsrEnvelope::release_tail_samples(
      kRate, cfg.patch.amp_env.release_ms);
  const int64_t note_zero_carrier_tail = sonare::midi::synth::DahdsrEnvelope::one_shot_tail_samples(
      kRate, cfg.patch.fm.ops[0].env, 1.0f, std::exp2(5.0f));

  CHECK(static_cast<int64_t>(synth.tail_samples()) > wrapper_tail);
  CHECK(static_cast<int64_t>(synth.tail_samples()) >= note_zero_carrier_tail);

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 0, 127)));
  const int64_t probe_samples = wrapper_tail + 256;
  render_left(synth, static_cast<int>(probe_samples));
  // The wrapper envelope's own tail has elapsed, but the carrier is still in its
  // 100 ms delay, so the one-shot slot must remain live.
  REQUIRE(synth.active_voice_count() == 1);

  const std::vector<float> delayed_onset = render_left(synth, static_cast<int>(kRate * 0.12));
  REQUIRE(absolute_peak(delayed_onset) > 1.0e-3f);
  REQUIRE(synth.active_voice_count() == 1);

  int64_t remaining = static_cast<int64_t>(synth.tail_samples()) - probe_samples -
                      static_cast<int64_t>(delayed_onset.size());
  while (remaining > 0 && synth.active_voice_count() > 0) {
    const int chunk = static_cast<int>(std::min<int64_t>(remaining, 4096));
    render_left(synth, chunk);
    remaining -= chunk;
  }
  CHECK(synth.active_voice_count() == 0);
}

TEST_CASE("FM one-shot tail is infinite only for audible sustaining carriers",
          "[midi][synth][fm][fm-tail]") {
  NativeSynthConfig cfg = fm_lifecycle_config(FmAlgorithm::kStack2);
  // A silent carrier with a sustaining envelope must be ignored.
  set_fm_operator(cfg.patch.fm.ops[0], 0.0f, 0.0f, 100.0f, 1.0f);
  // A sustaining modulator is not part of the lifetime bound either.
  set_fm_operator(cfg.patch.fm.ops[1], 1.0f, 0.0f, 100.0f, 1.0f);

  NativeSynth finite(cfg);
  finite.prepare(kRate, 256);
  CHECK(finite.tail_samples() < std::numeric_limits<int>::max());

  set_fm_operator(cfg.patch.fm.ops[0], 1.0f, 0.0f, 100.0f, 1.0f);
  NativeSynth infinite(cfg);
  infinite.prepare(kRate, 256);
  CHECK(infinite.tail_samples() == std::numeric_limits<int>::max());

  NativeSynthConfig gm_cfg = fm_lifecycle_config(FmAlgorithm::kStack2);
  gm_cfg.use_gm_programs = true;
  set_fm_operator(gm_cfg.patch.fm.ops[0], 1.0f, 0.0f, 100.0f, 1.0f);
  NativeSynth gm(gm_cfg);
  gm.prepare(kRate, 256);
  CHECK(gm.tail_samples() < std::numeric_limits<int>::max());
}

TEST_CASE("FM one-shot tail covers the key-rate minimum decay", "[midi][synth][fm][fm-tail]") {
  NativeSynthConfig cfg = fm_lifecycle_config(FmAlgorithm::kStack2);
  cfg.patch.amp_env.release_ms = 0.0f;
  set_fm_operator(cfg.patch.fm.ops[0], 1.0f, 0.0f, 0.0f, 0.0f);
  cfg.patch.fm.ops[0].key_rate_scale = 1.0f;
  sonare::midi::synth::FmVoiceCore core;
  core.start(cfg.patch.fm, kRate, 0, sonare::midi::Velocity16::from7(127));
  const int64_t bound = sonare::midi::synth::fm_one_shot_tail_samples(cfg.patch.fm, kRate);
  int elapsed = 0;
  while (!core.finished() && elapsed < 1024) {
    core.render(1.0f);
    ++elapsed;
  }
  REQUIRE(core.finished());
  REQUIRE(elapsed > 2);
  CHECK(bound >= elapsed);
}
