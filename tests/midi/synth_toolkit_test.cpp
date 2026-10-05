/// @file synth_toolkit_test.cpp
/// @brief Shared voice toolkit (midi/synth/): deterministic voice stealing,
///        exponential DAHDSR envelope timing, TPT SVF cutoff accuracy and
///        modulation stability, interpolation error and seeded voice
///        variation determinism. The audio-path pieces are also checked for
///        zero heap allocation (same shared counter as mixing/no_alloc_test).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "midi/synth/body_resonator.h"
#include "midi/synth/envelope.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/interpolation.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/sf2_voice.h"
#include "midi/synth/svf.h"
#include "midi/synth/voice_pool.h"
#include "midi/synth/voice_random.h"
#include "support/alloc_guard.h"

namespace {

using Catch::Approx;
using sonare::midi::synth::AllpassInterpolator;
using sonare::midi::synth::BodyResonator;
using sonare::midi::synth::BodyType;
using sonare::midi::synth::DahdsrConfig;
using sonare::midi::synth::DahdsrEnvelope;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::NativeSynthVoice;
using sonare::midi::synth::Sf2ChannelMod;
using sonare::midi::synth::Sf2Lfo;
using sonare::midi::synth::Sf2Voice;
using sonare::midi::synth::Sf2VoiceParams;
using sonare::midi::synth::TptSvf;
using sonare::midi::synth::VoicePool;
using sonare::midi::synth::VoiceRandomSequence;
using sonare::midi::synth::VoiceState;
using sonare::test::AllocationGuard;

constexpr double kSampleRate = 48000.0;
constexpr double kTwoPi = 6.28318530717958647692;

struct TestVoice : VoiceState {
  int payload = 0;
};

// RMS of a steady sine pushed through the SVF (after settling), per output tap.
float svf_sine_rms(float cutoff_hz, float q, float tone_hz, int tap /*0=lp,1=bp,2=hp*/) {
  TptSvf svf;
  svf.prepare(kSampleRate);
  svf.set(cutoff_hz, q);
  const int settle = 4800;
  const int measure = 9600;
  double sum_sq = 0.0;
  for (int i = 0; i < settle + measure; ++i) {
    const float x = std::sin(kTwoPi * tone_hz * static_cast<double>(i) / kSampleRate);
    const auto out = svf.process(x);
    const float y = tap == 0 ? out.lp : (tap == 1 ? out.bp : out.hp);
    if (i >= settle) sum_sq += static_cast<double>(y) * static_cast<double>(y);
  }
  return static_cast<float>(std::sqrt(sum_sq / measure));
}

}  // namespace

TEST_CASE("VoicePool steals free, then oldest releasing, then oldest", "[midi][synth]") {
  VoicePool<TestVoice> pool;
  pool.prepare(3);

  TestVoice* v1 = pool.allocate(0, 60);
  TestVoice* v2 = pool.allocate(0, 64);
  REQUIRE(v1 != nullptr);
  REQUIRE(v2 != nullptr);
  REQUIRE(v1 != v2);

  // 1. A free slot is preferred over stealing.
  TestVoice* v3 = pool.allocate(0, 67);
  REQUIRE(v3 != nullptr);
  REQUIRE(v3 != v1);
  REQUIRE(v3 != v2);
  REQUIRE(pool.active_count() == 3);

  // 2. With no free slot, the oldest RELEASING voice is stolen, even though an
  //    older held voice exists.
  v2->releasing = true;
  TestVoice* stolen = pool.allocate(0, 72);
  REQUIRE(stolen == v2);
  REQUIRE(stolen->note == 72);
  REQUIRE_FALSE(stolen->releasing);

  // 3. With no free and no releasing slot, the oldest voice overall is stolen.
  TestVoice* oldest = pool.allocate(0, 76);
  REQUIRE(oldest == v1);

  // Determinism: a fresh pool fed the same sequence makes the same decisions.
  VoicePool<TestVoice> pool2;
  pool2.prepare(3);
  TestVoice* w1 = pool2.allocate(0, 60);
  pool2.allocate(0, 64)->releasing = true;
  pool2.allocate(0, 67);
  REQUIRE(pool2.allocate(0, 72)->age == stolen->age);
  REQUIRE(pool2.allocate(0, 76) == w1);
}

TEST_CASE("DahdsrEnvelope attack reaches full level in the configured time", "[midi][synth]") {
  DahdsrConfig cfg;
  cfg.attack_ms = 50.0f;
  cfg.decay_ms = 100.0f;
  cfg.sustain = 0.5f;
  DahdsrEnvelope env;
  env.configure(kSampleRate, cfg);
  env.note_on();

  int samples_to_peak = 0;
  while (env.stage() == DahdsrEnvelope::Stage::kAttack ||
         env.stage() == DahdsrEnvelope::Stage::kDelay) {
    env.next();
    ++samples_to_peak;
    REQUIRE(samples_to_peak < 48000);  // never stalls
  }
  // One-pole overshoot attack crosses 1.0 at ~attack_ms (50 ms = 2400 samples).
  const double expected = 0.050 * kSampleRate;
  REQUIRE(samples_to_peak > expected * 0.7);
  REQUIRE(samples_to_peak < expected * 1.3);

  // Decay then approaches sustain monotonically from above.
  float prev = env.level();
  for (int i = 0; i < 48000 && env.stage() != DahdsrEnvelope::Stage::kSustain; ++i) {
    const float l = env.next();
    REQUIRE(l <= prev + 1.0e-6f);
    prev = l;
  }
  REQUIRE(env.level() == Approx(0.5f).margin(2.0e-3));
}

TEST_CASE("DahdsrEnvelope release decays exponentially to silence", "[midi][synth]") {
  DahdsrConfig cfg;
  cfg.attack_ms = 1.0f;
  cfg.decay_ms = 1.0f;
  cfg.sustain = 1.0f;
  cfg.release_ms = 30.0f;
  DahdsrEnvelope env;
  env.configure(kSampleRate, cfg);
  env.note_on();
  for (int i = 0; i < 4800; ++i) env.next();
  REQUIRE(env.level() == Approx(1.0f).margin(1.0e-3));

  env.note_off();
  REQUIRE(env.releasing());
  int tail = 0;
  while (env.active()) {
    env.next();
    ++tail;
    REQUIRE(tail < 96000);
  }
  REQUIRE(env.level() == 0.0f);
  // The static tail estimate bounds the actual tail.
  REQUIRE(tail <= DahdsrEnvelope::release_tail_samples(kSampleRate, cfg.release_ms) + 4);

  // Exponential (not linear): re-run and check convex shape — the level at
  // half the tail is far below 0.5 of the start.
  env.note_on();
  for (int i = 0; i < 4800; ++i) env.next();
  env.note_off();
  for (int i = 0; i < tail / 2; ++i) env.next();
  REQUIRE(env.level() < 0.25f);
}

TEST_CASE("DahdsrEnvelope honours delay and hold segments", "[midi][synth]") {
  DahdsrConfig cfg;
  cfg.delay_ms = 10.0f;
  cfg.attack_ms = 1.0f;
  cfg.hold_ms = 10.0f;
  cfg.decay_ms = 20.0f;
  cfg.sustain = 0.3f;
  DahdsrEnvelope env;
  env.configure(kSampleRate, cfg);
  env.note_on();

  // During the delay segment the level stays at zero.
  for (int i = 0; i < 400; ++i) REQUIRE(env.next() == 0.0f);
  // After delay + attack the envelope holds at 1.0 for ~10 ms.
  for (int i = 0; i < 200; ++i) env.next();
  REQUIRE(env.level() == Approx(1.0f).margin(1.0e-3));
  for (int i = 0; i < 300; ++i) env.next();
  REQUIRE(env.level() == Approx(1.0f).margin(1.0e-3));
}

TEST_CASE("NativeSynthVoice steal retriggers from residual envelope level", "[midi][synth]") {
  NativeSynthPatch patch;
  patch.amp_env.attack_ms = 20.0f;
  patch.amp_env.decay_ms = 1.0f;
  patch.amp_env.sustain = 1.0f;
  patch.filter_env = patch.amp_env;

  NativeSynthVoice voice;
  voice.active = true;
  voice.channel = 0;
  voice.note = 60;
  voice.age = 1;
  voice.start(patch, kSampleRate, sonare::midi::Velocity16::from7(127), 0);

  Sf2ChannelMod mod;
  for (int i = 0; i < 480; ++i) {
    static_cast<void>(voice.render(mod));
  }
  const float amp_before = voice.amp_env.level();
  const float filter_before = voice.filter_env.level();
  REQUIRE(amp_before > 0.1f);
  REQUIRE(filter_before > 0.1f);

  voice.note = 67;
  voice.age = 2;
  voice.start(patch, kSampleRate, sonare::midi::Velocity16::from7(127), 0);

  REQUIRE(voice.amp_env.level() == Approx(amp_before).margin(1.0e-6f));
  REQUIRE(voice.filter_env.level() == Approx(filter_before).margin(1.0e-6f));
}

TEST_CASE("Sf2Voice steal retriggers from residual envelope level", "[midi][sf2]") {
  const float samples[] = {1.0f, 1.0f, 1.0f, 1.0f};
  Sf2VoiceParams params;
  params.start = 0;
  params.end = 4;
  params.loop_start = 0;
  params.loop_end = 4;
  params.loop_mode = 1;
  params.volume_env.attack_ms = 20.0f;
  params.volume_env.decay_ms = 1.0f;
  params.volume_env.sustain = 1.0f;
  params.mod_env = params.volume_env;

  Sf2Voice voice;
  voice.active = true;
  voice.channel = 0;
  voice.note = 60;
  voice.age = 1;
  voice.start(samples, params, kSampleRate, 1.0f);

  Sf2ChannelMod mod;
  for (int i = 0; i < 480; ++i) {
    static_cast<void>(voice.render(mod));
  }
  const float amp_before = voice.env.level();
  const float mod_before = voice.mod_env.level();
  REQUIRE(amp_before > 0.1f);
  REQUIRE(mod_before > 0.1f);

  voice.note = 67;
  voice.age = 2;
  voice.start(samples, params, kSampleRate, 1.0f);

  REQUIRE(voice.env.level() == Approx(amp_before).margin(1.0e-6f));
  REQUIRE(voice.mod_env.level() == Approx(mod_before).margin(1.0e-6f));
}

TEST_CASE("TptSvf lowpass passes below cutoff and attenuates above", "[midi][synth]") {
  // Tone an octave below cutoff: ~unity. Two octaves above: strongly attenuated.
  const float pass = svf_sine_rms(2000.0f, 0.7071f, 1000.0f, 0);
  const float stop = svf_sine_rms(2000.0f, 0.7071f, 8000.0f, 0);
  const float ref = 0.70710678f;  // RMS of a unit sine
  REQUIRE(pass == Approx(ref).margin(0.08));
  REQUIRE(stop < ref * 0.12f);  // > ~18 dB down (2nd-order slope, 2 octaves)

  // Highpass mirrors it.
  const float hp_stop = svf_sine_rms(2000.0f, 0.7071f, 500.0f, 2);
  const float hp_pass = svf_sine_rms(2000.0f, 0.7071f, 8000.0f, 2);
  REQUIRE(hp_pass == Approx(ref).margin(0.08));
  REQUIRE(hp_stop < ref * 0.12f);

  // Bandpass peaks at the centre and a high-Q peak resonates above unity gain
  // relative to an off-centre tone.
  const float bp_centre = svf_sine_rms(2000.0f, 8.0f, 2000.0f, 1);
  const float bp_off = svf_sine_rms(2000.0f, 8.0f, 500.0f, 1);
  REQUIRE(bp_centre > 4.0f * bp_off);
}

TEST_CASE("TptSvf stays bounded under audio-rate cutoff modulation", "[midi][synth]") {
  TptSvf svf;
  svf.prepare(kSampleRate);
  float peak = 0.0f;
  for (int i = 0; i < 48000; ++i) {
    const double t = static_cast<double>(i) / kSampleRate;
    // Sweep cutoff 100 Hz .. 12 kHz at 30 Hz with high resonance.
    const float fc =
        100.0f + 11900.0f * 0.5f * (1.0f + static_cast<float>(std::sin(kTwoPi * 30.0 * t)));
    svf.set(fc, 12.0f);
    const float x = static_cast<float>(std::sin(kTwoPi * 220.0 * t));
    const auto out = svf.process(x);
    REQUIRE(std::isfinite(out.lp));
    peak = std::max(peak, std::fabs(out.lp));
  }
  REQUIRE(peak < 16.0f);  // resonant boost is fine; divergence is not
}

TEST_CASE("read_sample_linear tracks a sine within linear-interp error", "[midi][synth]") {
  // A 256-sample sine table read at fractional positions stays within the
  // textbook linear-interpolation error bound for this oversampling.
  std::vector<float> table(257);
  for (size_t i = 0; i < table.size(); ++i) {
    table[i] = static_cast<float>(std::sin(kTwoPi * static_cast<double>(i) / 256.0));
  }
  double max_err = 0.0;
  for (int k = 0; k < 10000; ++k) {
    const double pos = 256.0 * static_cast<double>(k) / 10000.0;
    const float y = sonare::midi::synth::read_sample_linear(table.data(), table.size(), pos);
    const double ref = std::sin(kTwoPi * pos / 256.0);
    max_err = std::max(max_err, std::abs(static_cast<double>(y) - ref));
  }
  // Error bound ~ (pi/N)^2 / 2 for a sine; allow float slack.
  REQUIRE(max_err < 1.5e-4);

  // Edge clamping.
  REQUIRE(sonare::midi::synth::read_sample_linear(table.data(), table.size(), -1.0) ==
          table.front());
  REQUIRE(sonare::midi::synth::read_sample_linear(table.data(), table.size(), 1000.0) ==
          table.back());
}

TEST_CASE("AllpassInterpolator approximates a half-sample delay", "[midi][synth]") {
  AllpassInterpolator ap;
  ap.set_fraction(0.5f);
  // Feed a slow sine; output should match the input delayed by ~0.5 samples.
  double max_err = 0.0;
  float y = 0.0f;
  for (int i = 0; i < 2000; ++i) {
    const double phase = kTwoPi * 200.0 * static_cast<double>(i) / kSampleRate;
    y = ap.process(static_cast<float>(std::sin(phase)));
    if (i > 100) {
      const double expected =
          std::sin(kTwoPi * 200.0 * (static_cast<double>(i) - 0.5) / kSampleRate);
      max_err = std::max(max_err, std::abs(static_cast<double>(y) - expected));
    }
  }
  REQUIRE(max_err < 1.0e-3);
}

TEST_CASE("voice_random is deterministic and decorrelated across voices", "[midi][synth]") {
  using sonare::midi::synth::voice_random_bipolar;
  using sonare::midi::synth::voice_seed;

  // Same identifiers -> bit-identical values.
  REQUIRE(voice_random_bipolar(voice_seed(3, 60, 41)) ==
          voice_random_bipolar(voice_seed(3, 60, 41)));
  // Different voice / note / age each change the value.
  REQUIRE(voice_random_bipolar(voice_seed(3, 60, 41)) !=
          voice_random_bipolar(voice_seed(4, 60, 41)));
  REQUIRE(voice_random_bipolar(voice_seed(3, 61, 41)) !=
          voice_random_bipolar(voice_seed(3, 60, 41)));
  REQUIRE(voice_random_bipolar(voice_seed(3, 60, 42)) !=
          voice_random_bipolar(voice_seed(3, 60, 41)));

  // Range and rough zero-mean over many draws.
  VoiceRandomSequence seq;
  seq.reseed(1, 60, 1);
  double mean = 0.0;
  for (int i = 0; i < 4096; ++i) {
    const float v = seq.next_bipolar();
    REQUIRE(v >= -1.0f);
    REQUIRE(v < 1.0f);
    mean += v;
  }
  mean /= 4096.0;
  REQUIRE(std::abs(mean) < 0.05);

  // Counter-based random access matches the stream.
  VoiceRandomSequence a;
  a.reseed(2, 64, 7);
  const float first = a.next_unipolar();
  VoiceRandomSequence b;
  b.reseed(2, 64, 7);
  REQUIRE(b.unipolar_at(0) == first);
}

TEST_CASE("gm_fallback_sends weights ambience per program", "[midi][synth]") {
  using sonare::midi::synth::gm_fallback_sends;
  using sonare::midi::synth::GmFallbackSends;

  // The weights are fitted constants, so what is asserted here is the shape
  // they have to keep: a scale is a finite non-negative multiplier of the
  // channel send (that is what makes CC 0 stay dry whatever the program), and
  // the room a program carries is ordered the way the instruments are.
  for (int program = 0; program < 128; ++program) {
    const GmFallbackSends s = gm_fallback_sends(0, static_cast<uint8_t>(program));
    INFO("program " << program);
    REQUIRE(std::isfinite(s.reverb_scale));
    REQUIRE(std::isfinite(s.chorus_scale));
    REQUIRE(s.reverb_scale >= 0.0f);
    REQUIRE(s.chorus_scale >= 0.0f);
  }

  const GmFallbackSends drums = gm_fallback_sends(128, 0);
  const GmFallbackSends piano = gm_fallback_sends(0, 0);
  REQUIRE(std::isfinite(drums.reverb_scale));
  REQUIRE(drums.reverb_scale < piano.reverb_scale);  // a kit is tighter than a melodic part
  REQUIRE(drums.chorus_scale < piano.chorus_scale);

  // Church Organ (19) lives in a cathedral: no melodic program carries more
  // room, and an electric bass (33) carries markedly less.
  const GmFallbackSends organ = gm_fallback_sends(0, 19);
  for (int program = 0; program < 128; ++program) {
    INFO("program " << program);
    REQUIRE(gm_fallback_sends(0, static_cast<uint8_t>(program)).reverb_scale <= organ.reverb_scale);
  }
  REQUIRE(gm_fallback_sends(0, 33).reverb_scale < 0.5f * organ.reverb_scale);

  // A GS variation bank resolves through the same program, so its weighting
  // follows the capital tone rather than dropping to the neutral default.
  REQUIRE(gm_fallback_sends(8, 19).reverb_scale == organ.reverb_scale);
}

TEST_CASE("DahdsrEnvelope rises to a sustain raised above the level during decay",
          "[midi][synth][envelope-live]") {
  DahdsrConfig cfg;
  cfg.attack_ms = 1.0f;
  cfg.decay_ms = 60.0f;
  cfg.sustain = 0.3f;
  DahdsrEnvelope env;
  env.configure(kSampleRate, cfg);
  env.note_on();
  while (env.stage() != DahdsrEnvelope::Stage::kDecay) env.next();
  while (env.level() > 0.6f) env.next();
  REQUIRE(env.stage() == DahdsrEnvelope::Stage::kDecay);

  cfg.sustain = 0.8f;
  env.configure(kSampleRate, cfg);
  float prev = env.level();
  int rising_in_decay = 0;
  for (int i = 0; i < 48000 && env.stage() == DahdsrEnvelope::Stage::kDecay; ++i) {
    const float l = env.next();
    REQUIRE(l >= prev);
    REQUIRE(l <= 0.8f + 1.0e-6f);
    if (env.stage() == DahdsrEnvelope::Stage::kDecay) ++rising_in_decay;
    prev = l;
  }
  // The level climbs over many samples instead of snapping onto the target.
  REQUIRE(rising_in_decay > 100);
  REQUIRE(env.stage() == DahdsrEnvelope::Stage::kSustain);
  REQUIRE(env.level() == Approx(0.8f).margin(1.0e-3));
}

TEST_CASE("DahdsrEnvelope keeps a held note alive when sustain is automated to zero",
          "[midi][synth][envelope-live]") {
  DahdsrConfig cfg;
  cfg.attack_ms = 1.0f;
  cfg.decay_ms = 30.0f;
  cfg.sustain = 0.5f;
  DahdsrEnvelope env;
  env.configure(kSampleRate, cfg);
  env.note_on();
  while (env.stage() != DahdsrEnvelope::Stage::kDecay) env.next();
  while (env.level() > 0.7f) env.next();

  cfg.sustain = 0.0f;
  env.configure(kSampleRate, cfg);
  for (int i = 0; i < 24000; ++i) env.next();
  REQUIRE(env.active());
  REQUIRE(env.stage() == DahdsrEnvelope::Stage::kSustain);
  REQUIRE(env.level() == 0.0f);

  env.note_off();
  REQUIRE(env.releasing());
}

TEST_CASE("DahdsrEnvelope with zero sustain at note-on still ends at the decay landing",
          "[midi][synth][envelope-live]") {
  DahdsrConfig cfg;
  cfg.attack_ms = 1.0f;
  cfg.decay_ms = 20.0f;
  cfg.sustain = 0.0f;
  DahdsrEnvelope env;
  env.configure(kSampleRate, cfg);
  env.note_on();
  for (int i = 0; i < 48000 && env.active(); ++i) env.next();
  REQUIRE_FALSE(env.active());
  REQUIRE(env.level() == 0.0f);
}

TEST_CASE("Sf2Lfo set_frequency keeps the phase and adopts the new period",
          "[midi][synth][lfo-live]") {
  Sf2Lfo lfo;
  lfo.start(kSampleRate, 0.0f, 100.0f);
  float prev = 0.0f;
  float max_step = 0.0f;
  for (int i = 0; i < 1000; ++i) {
    const float x = lfo.next();
    if (i > 0) max_step = std::max(max_step, std::fabs(x - prev));
    prev = x;
  }
  lfo.set_frequency(kSampleRate, 200.0f);
  std::vector<int> rising;
  for (int i = 0; i < 2000; ++i) {
    const float x = lfo.next();
    // Triangle slope at 200 Hz is 4 * 200 / sr per sample; no jump at the change.
    REQUIRE(std::fabs(x - prev) <= 4.0f * 200.0f / static_cast<float>(kSampleRate) + 1.0e-4f);
    if (prev < 0.0f && x >= 0.0f) rising.push_back(i);
    prev = x;
  }
  REQUIRE(max_step <= 4.0f * 100.0f / static_cast<float>(kSampleRate) + 1.0e-4f);
  REQUIRE(rising.size() >= 6);
  for (size_t k = 1; k < rising.size(); ++k) {
    REQUIRE(rising[k] - rising[k - 1] == Approx(240.0).margin(2.0));
  }
}

TEST_CASE("Sf2Lfo set_frequency to zero stops the output at zero", "[midi][synth][lfo-live]") {
  // 144 samples at 100 Hz leaves the phase near 0.3 (target 0.5); 384 near 0.8 (target 0).
  for (const int run : {144, 384, 100, 480}) {
    Sf2Lfo lfo;
    lfo.start(kSampleRate, 0.0f, 100.0f);
    for (int i = 0; i < run; ++i) lfo.next();
    lfo.set_frequency(kSampleRate, 0.0f);
    // The remaining half period is at most 240 samples; allow one full period.
    for (int i = 0; i < 480; ++i) lfo.next();
    for (int i = 0; i < 1000; ++i) REQUIRE(std::fabs(lfo.next()) < 1.0e-6f);
  }
}

TEST_CASE("BodyResonator set_mix rescales the body path and stop() bypasses the bank",
          "[midi][synth][body-live]") {
  BodyResonator a;
  BodyResonator b;
  a.start(BodyType::kGuitar, kSampleRate, 110.0f, 0.5f);
  b.start(BodyType::kGuitar, kSampleRate, 110.0f, 1.0f);
  REQUIRE(a.active());
  a.set_mix(1.0f);
  for (int i = 0; i < 512; ++i) {
    const float x = i == 0 ? 1.0f : 0.0f;
    REQUIRE(a.process(x) == b.process(x));
  }

  // Mix 0 leaves only the dry path while the bank stays up.
  a.set_mix(0.0f);
  REQUIRE(a.active());
  REQUIRE(a.process(0.25f) == 0.25f);
  // Out-of-range mixes clamp like start().
  BodyResonator c;
  BodyResonator d;
  c.start(BodyType::kGuitar, kSampleRate, 110.0f, 0.5f);
  d.start(BodyType::kGuitar, kSampleRate, 110.0f, 1.0f);
  c.set_mix(2.0f);
  for (int i = 0; i < 64; ++i)
    REQUIRE(c.process(i == 0 ? 1.0f : 0.0f) == d.process(i == 0 ? 1.0f : 0.0f));

  a.stop();
  REQUIRE_FALSE(a.active());
  for (int i = 0; i < 64; ++i)
    REQUIRE(a.process(0.1f * static_cast<float>(i)) == 0.1f * static_cast<float>(i));

  // A stopped bank can be started again.
  a.start(BodyType::kGuitar, kSampleRate, 110.0f, 0.5f);
  REQUIRE(a.active());
}

TEST_CASE("synth toolkit audio path performs no heap allocation", "[midi][synth][rt]") {
  VoicePool<TestVoice> pool;
  pool.prepare(8);
  DahdsrEnvelope env;
  env.configure(kSampleRate, DahdsrConfig{});
  TptSvf svf;
  svf.prepare(kSampleRate);
  AllpassInterpolator ap;
  ap.set_fraction(0.3f);
  VoiceRandomSequence seq;
  seq.reseed(0, 60, 1);

  AllocationGuard guard;
  env.note_on();
  float acc = 0.0f;
  for (int i = 0; i < 512; ++i) {
    pool.allocate(0, static_cast<uint8_t>(48 + (i % 24)));
    const float e = env.next();
    svf.set(200.0f + 50.0f * static_cast<float>(i % 64), 2.0f);
    acc += svf.process(e).lp + ap.process(e) + seq.next_bipolar();
  }
  env.note_off();
  while (env.active()) acc += env.next();
  REQUIRE(guard.count() == 0);
  REQUIRE(std::isfinite(acc));
}
