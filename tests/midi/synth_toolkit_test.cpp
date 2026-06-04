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

#include "midi/synth/envelope.h"
#include "midi/synth/interpolation.h"
#include "midi/synth/svf.h"
#include "midi/synth/voice_pool.h"
#include "midi/synth/voice_random.h"
#include "support/alloc_guard.h"

namespace {

using Catch::Approx;
using sonare::midi::synth::AllpassInterpolator;
using sonare::midi::synth::DahdsrConfig;
using sonare::midi::synth::DahdsrEnvelope;
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
