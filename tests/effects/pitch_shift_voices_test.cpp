/// @file pitch_shift_voices_test.cpp
/// @brief The pitch shifter's second voice, fine offsets, balance, pre-delays, feedback and mix
/// law.
///
/// Delays and levels are read off impulse responses through unity-ratio voices, where the read is
/// one fixed tap and the arrival is exact; intervals are read off a tone with a single-bin DFT.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "effects/common/mix_law.h"
#include "effects/modulation/pitch_shifter.h"
#include "util/constants.h"

namespace {

using Catch::Approx;
using sonare::effects::common::MixLaw;
using sonare::effects::modulation::PitchShifter;
using sonare::effects::modulation::PitchShifterConfig;

constexpr double kRate = 48000.0;

struct Stereo {
  std::vector<float> left;
  std::vector<float> right;
};

Stereo run(const PitchShifterConfig& config, const std::vector<float>& input) {
  PitchShifter shifter(config);
  const int length = static_cast<int>(input.size());
  shifter.prepare(kRate, length);
  Stereo out{input, input};
  float* channels[2] = {out.left.data(), out.right.data()};
  shifter.process(channels, 2, length);
  return out;
}

std::vector<float> impulse(int length) {
  std::vector<float> x(static_cast<std::size_t>(length), 0.0f);
  x[0] = 1.0f;
  return x;
}

std::vector<float> tone(double hz, int length) {
  std::vector<float> x(static_cast<std::size_t>(length));
  for (int i = 0; i < length; ++i) {
    x[static_cast<std::size_t>(i)] =
        static_cast<float>(std::sin(sonare::constants::kTwoPiD * hz * i / kRate));
  }
  return x;
}

float at(const std::vector<float>& x, int index) { return x[static_cast<std::size_t>(index)]; }

/// Amplitude of a sinusoid at @p hz over the second half of @p x.
double amplitude_at(const std::vector<float>& x, double hz) {
  const std::size_t begin = x.size() / 2;
  double re = 0.0;
  double im = 0.0;
  for (std::size_t i = begin; i < x.size(); ++i) {
    const double phase = sonare::constants::kTwoPiD * hz * static_cast<double>(i) / kRate;
    re += static_cast<double>(x[i]) * std::cos(phase);
    im += static_cast<double>(x[i]) * std::sin(phase);
  }
  return 2.0 * std::hypot(re, im) / static_cast<double>(x.size() - begin);
}

/// Impulse-response shape: a unity voice reading @p pre_ms ahead, and a second one at
/// @p pre2_ms, so each lands on a whole sample at 48 kHz.
PitchShifterConfig unity_voices() {
  PitchShifterConfig config;
  config.dry_wet = 1.0f;
  config.pre_delay_ms = 10.0f;   // sample 480
  config.pre_delay2_ms = 25.0f;  // sample 1200
  return config;
}

// A multiple of 1 / window keeps the two taps, one window apart, in phase on the input.
constexpr double kInPhaseToneHz = 22.0 / 0.0225;

constexpr int kFirst = 480;
constexpr int kSecond = 1200;
constexpr int kImpulseLength = 4800;

}  // namespace

TEST_CASE("the second voice lands at its pre-delay, level and balance", "[pitch-voices]") {
  PitchShifterConfig config = unity_voices();

  SECTION("level2 0 leaves only the first voice") {
    const Stereo out = run(config, impulse(kImpulseLength));
    CHECK(at(out.left, kFirst) == Approx(1.0f).margin(1e-3));
    CHECK(at(out.left, kSecond) == 0.0f);
    CHECK(at(out.right, kSecond) == 0.0f);
  }

  SECTION("level2 sets the second voice's gain") {
    config.level2 = 0.5f;
    const Stereo out = run(config, impulse(kImpulseLength));
    CHECK(at(out.left, kFirst) == Approx(1.0f).margin(1e-3));
    CHECK(at(out.left, kSecond) == Approx(0.5f).margin(1e-3));
    CHECK(at(out.right, kSecond) == Approx(0.5f).margin(1e-3));
  }

  SECTION("pan2 places the second voice, unity on the near side") {
    config.level2 = 0.5f;
    config.pan2 = 1.0f;
    Stereo out = run(config, impulse(kImpulseLength));
    CHECK(at(out.left, kSecond) == Approx(0.0f).margin(1e-3));
    CHECK(at(out.right, kSecond) == Approx(0.5f).margin(1e-3));
    config.pan2 = -0.5f;
    out = run(config, impulse(kImpulseLength));
    CHECK(at(out.left, kSecond) == Approx(0.5f).margin(1e-3));
    CHECK(at(out.right, kSecond) == Approx(0.25f).margin(1e-3));
    // The first voice is untouched by the second voice's balance.
    CHECK(at(out.left, kFirst) == Approx(1.0f).margin(1e-3));
    CHECK(at(out.right, kFirst) == Approx(1.0f).margin(1e-3));
  }

  SECTION("pan places the first voice") {
    config.pan = 1.0f;
    const Stereo out = run(config, impulse(kImpulseLength));
    CHECK(at(out.left, kFirst) == Approx(0.0f).margin(1e-3));
    CHECK(at(out.right, kFirst) == Approx(1.0f).margin(1e-3));
  }
}

TEST_CASE("the pre-delays place each voice", "[pitch-voices]") {
  PitchShifterConfig config = unity_voices();
  config.level2 = 1.0f;
  const Stereo out = run(config, impulse(kImpulseLength));
  for (int i = 0; i < kImpulseLength; ++i) {
    if (i == kFirst || i == kSecond) continue;
    INFO("index " << i);
    REQUIRE(std::fabs(at(out.left, i)) < 1e-3f);
  }
  CHECK(at(out.left, kFirst) == Approx(1.0f).margin(1e-3));
  CHECK(at(out.left, kSecond) == Approx(1.0f).margin(1e-3));
}

TEST_CASE("feedback repeats the shifted sum once per pre-delay", "[pitch-voices]") {
  PitchShifterConfig config = unity_voices();
  config.feedback = 0.5f;
  const Stereo out = run(config, impulse(kImpulseLength));
  CHECK(at(out.left, kFirst) == Approx(1.0f).margin(1e-3));
  CHECK(at(out.left, 2 * kFirst) == Approx(0.5f).margin(1e-3));
  CHECK(at(out.left, 3 * kFirst) == Approx(0.25f).margin(1e-3));

  config.feedback = -0.5f;
  const Stereo inverted = run(config, impulse(kImpulseLength));
  CHECK(at(inverted.left, 2 * kFirst) == Approx(-0.5f).margin(1e-3));
}

TEST_CASE("feedback decays with every voice configuration", "[pitch-voices]") {
  // A low tone keeps the two faded taps of a shifting voice correlated, which is
  // the worst case for the loop gain; both voices at full level and centred.
  constexpr int kLength = 3 * 48000;
  std::vector<float> burst(static_cast<std::size_t>(kLength), 0.0f);
  for (int i = 0; i < 24000; ++i) {
    burst[static_cast<std::size_t>(i)] =
        static_cast<float>(0.5 * std::sin(sonare::constants::kTwoPiD * 30.0 * i / kRate));
  }
  for (const float feedback : {0.75f, 0.95f, -0.95f}) {
    for (const float semitones : {0.0f, 7.0f}) {
      CAPTURE(feedback, semitones);
      PitchShifterConfig config;
      config.semitones = semitones;
      config.semitones2 = -5.0f;
      config.level2 = 1.0f;
      config.pre_delay_ms = 10.0f;
      config.pre_delay2_ms = 10.0f;
      config.feedback = feedback;
      const Stereo out = run(config, burst);
      float early = 0.0f;
      float late = 0.0f;
      for (int i = 0; i < 24000; ++i) early = std::max(early, std::fabs(at(out.left, i)));
      for (int i = kLength - 24000; i < kLength; ++i) {
        late = std::max(late, std::fabs(at(out.left, i)));
      }
      REQUIRE(std::isfinite(early));
      REQUIRE(std::isfinite(late));
      CHECK(late < 0.01f * early);
    }
  }
}

TEST_CASE("feedback holds a non-finite sample out of the loop", "[pitch-voices]") {
  PitchShifterConfig config = unity_voices();
  config.feedback = 0.5f;
  std::vector<float> input = impulse(kImpulseLength);
  input[10] = std::nanf("");
  PitchShifter shifter(config);
  shifter.prepare(kRate, kImpulseLength);
  std::vector<float> left = input;
  std::vector<float> right = input;
  float* channels[2] = {left.data(), right.data()};
  shifter.process(channels, 2, kImpulseLength);
  std::vector<float> clean = impulse(kImpulseLength);
  std::vector<float> clean_r = clean;
  float* clean_channels[2] = {clean.data(), clean_r.data()};
  shifter.process(clean_channels, 2, kImpulseLength);
  for (const float sample : clean) REQUIRE(std::isfinite(sample));
}

TEST_CASE("mixLaw 1 keeps dry and wet as two independent ramps", "[pitch-voices]") {
  PitchShifterConfig config = unity_voices();
  struct Row {
    float dry_wet;
    float dry;
    float wet;
  };
  // Each gain reaches unity at the halfway setting and only the far one falls away.
  for (const Row& row : {Row{0.25f, 1.0f, 0.5f}, Row{0.5f, 1.0f, 1.0f}, Row{0.75f, 0.5f, 1.0f}}) {
    config.dry_wet = row.dry_wet;
    config.mix_law = MixLaw::kTwoRamps;
    const Stereo ramps = run(config, impulse(kImpulseLength));
    INFO("dry_wet " << row.dry_wet);
    CHECK(at(ramps.left, 0) == Approx(row.dry).margin(1e-5));
    CHECK(at(ramps.left, kFirst) == Approx(row.wet).margin(1e-3));

    config.mix_law = MixLaw::kCrossfade;
    const Stereo fade = run(config, impulse(kImpulseLength));
    CHECK(at(fade.left, 0) == Approx(1.0f - row.dry_wet).margin(1e-5));
    CHECK(at(fade.left, kFirst) == Approx(row.dry_wet).margin(1e-3));
  }
}

TEST_CASE("the second voice sounds at its interval and is absent at level2 0", "[pitch-voices]") {
  constexpr double kTone = kInPhaseToneHz;
  constexpr int kLength = 24000;
  PitchShifterConfig config;
  config.dry_wet = 1.0f;
  config.semitones = 12.0f;
  config.semitones2 = -12.0f;
  const std::vector<float> input = tone(kTone, kLength);

  const Stereo off = run(config, input);
  config.level2 = 0.5f;
  const Stereo half = run(config, input);
  config.level2 = 1.0f;
  const Stereo full = run(config, input);

  const double up = amplitude_at(full.left, 2.0 * kTone);
  CHECK(up > 0.5);
  CHECK(amplitude_at(off.left, 0.5 * kTone) < 0.02 * up);
  const double octave_down = amplitude_at(full.left, 0.5 * kTone);
  CHECK(octave_down > 0.5 * up);
  CHECK(amplitude_at(half.left, 0.5 * kTone) == Approx(0.5 * octave_down).epsilon(0.05));
  // The first voice is unchanged by the second.
  CHECK(amplitude_at(half.left, 2.0 * kTone) == Approx(up).epsilon(0.05));
}

TEST_CASE("cents offset each voice from its semitone interval", "[pitch-voices]") {
  constexpr double kTone = kInPhaseToneHz;
  constexpr int kLength = 24000;
  const std::vector<float> input = tone(kTone, kLength);

  SECTION("voice 1") {
    PitchShifterConfig config;
    config.dry_wet = 1.0f;
    config.semitones = 12.0f;
    config.cents = 100.0f;
    const Stereo out = run(config, input);
    const double target = kTone * std::exp2(13.0 / 12.0);
    const double flat = kTone * std::exp2(12.0 / 12.0);
    CHECK(amplitude_at(out.left, target) > 0.5);
    CHECK(amplitude_at(out.left, flat) < 0.25 * amplitude_at(out.left, target));
  }

  SECTION("voice 2") {
    PitchShifterConfig config;
    config.dry_wet = 1.0f;
    config.semitones = 12.0f;
    config.semitones2 = -12.0f;
    config.cents2 = 100.0f;
    config.level2 = 1.0f;
    const Stereo out = run(config, input);
    const double target = kTone * std::exp2(-11.0 / 12.0);
    const double flat = kTone * std::exp2(-12.0 / 12.0);
    CHECK(amplitude_at(out.left, target) > 0.5);
    CHECK(amplitude_at(out.left, flat) < 0.25 * amplitude_at(out.left, target));
  }
}

TEST_CASE("realtime ids reach the voices and pre-delays", "[pitch-voices]") {
  PitchShifter shifter;
  shifter.prepare(kRate, 512);
  for (unsigned int id = 2; id <= 8; ++id) REQUIRE(shifter.set_parameter(id, 0.25f));
  REQUIRE(shifter.set_parameter(9, 1.0f));
  for (const float bad : {-1.0f, 0.5f, 2.0f}) REQUIRE_FALSE(shifter.set_parameter(9, bad));
  REQUIRE(shifter.set_parameter(12, 25.0f));
  REQUIRE(shifter.set_parameter(13, 10.0f));
  REQUIRE(shifter.set_parameter(14, 25.0f));
  REQUIRE(shifter.parameter_is_realtime_safe(12));
  REQUIRE(shifter.parameter_is_realtime_safe(13));
  REQUIRE(shifter.parameter_is_realtime_safe(14));
  bool has_window = false;
  bool has_pre_delay = false;
  bool has_pre_delay2 = false;
  for (const auto& descriptor : shifter.parameter_descriptors()) {
    has_window = has_window || (descriptor.key == std::string("windowMs") && descriptor.id == 12);
    has_pre_delay =
        has_pre_delay || (descriptor.key == std::string("preDelayMs") && descriptor.id == 13);
    has_pre_delay2 =
        has_pre_delay2 || (descriptor.key == std::string("preDelay2Ms") && descriptor.id == 14);
  }
  REQUIRE(has_window);
  REQUIRE(has_pre_delay);
  REQUIRE(has_pre_delay2);
}
