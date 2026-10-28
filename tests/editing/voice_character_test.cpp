/// @file voice_character_test.cpp
/// @brief CharacterStage: band limits, requested physical quantities across sample rates, and
///        the realtime allocation contract.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "editing/voice_changer/character_stage.h"
#include "support/alloc_guard.h"
#include "util/constants.h"

namespace {

using sonare::constants::kTwoPiD;
using sonare::editing::voice_changer::CharacterConfig;
using sonare::editing::voice_changer::CharacterStage;
using sonare::editing::voice_changer::CharacterType;

constexpr float kAmount = 0.1f;
constexpr double kToneSeconds = 0.5;
constexpr double kSettleSeconds = 0.15;

CharacterStage make_stage(double sr, CharacterType type, float amount, float tone) {
  CharacterStage stage;
  stage.prepare(sr);
  CharacterConfig config;
  config.type = type;
  config.amount = amount;
  config.tone = tone;
  stage.set_config(config);
  return stage;
}

/// Hann-windowed single-bin amplitude of the settled part of @p y at @p hz.
double bin_amplitude(const std::vector<float>& y, double sr, double hz, std::size_t from) {
  double re = 0.0;
  double im = 0.0;
  double wsum = 0.0;
  const std::size_t n = y.size() - from;
  for (std::size_t i = 0; i < n; ++i) {
    const double w =
        0.5 - 0.5 * std::cos(kTwoPiD * static_cast<double>(i) / static_cast<double>(n));
    const double phase = kTwoPiD * hz * static_cast<double>(from + i) / sr;
    re += w * y[from + i] * std::cos(phase);
    im += w * y[from + i] * std::sin(phase);
    wsum += w;
  }
  return 2.0 * std::sqrt(re * re + im * im) / wsum;
}

/// Amplitude of a sine at @p hz after the stage.
double tone_gain(double sr, CharacterType type, float amount, float tone, double hz,
                 float input_amplitude) {
  CharacterStage stage = make_stage(sr, type, amount, tone);
  const auto n = static_cast<std::size_t>(kToneSeconds * sr);
  std::vector<float> y(n);
  for (std::size_t i = 0; i < n; ++i) {
    const float x =
        input_amplitude * static_cast<float>(std::sin(kTwoPiD * hz * static_cast<double>(i) / sr));
    y[i] = stage.process_sample(x);
  }
  return bin_amplitude(y, sr, hz, static_cast<std::size_t>(kSettleSeconds * sr)) / input_amplitude;
}

double db(double ratio) { return 20.0 * std::log10(ratio); }

/// Mean rising-crossing period of a constant input through the strobe, in seconds.
double strobe_period(double sr, float tone, float amount) {
  CharacterStage stage = make_stage(sr, CharacterType::Strobo, amount, tone);
  const auto n = static_cast<std::size_t>(1.1 * sr);
  const float threshold = 1.0f - 0.5f * amount;
  float prev = stage.process_sample(1.0f);
  std::vector<std::size_t> rising;
  for (std::size_t i = 1; i < n; ++i) {
    const float y = stage.process_sample(1.0f);
    if (prev < threshold && y >= threshold) rising.push_back(i);
    prev = y;
  }
  REQUIRE(rising.size() >= 3);
  return static_cast<double>(rising.back() - rising.front()) /
         static_cast<double>(rising.size() - 1) / sr;
}

/// Position in milliseconds of the chorus wet peak for an impulse fed at @p onset_s.
double chorus_peak_ms(double sr, double onset_s) {
  CharacterStage stage = make_stage(sr, CharacterType::BbdChorus, 1.0f, 0.0f);
  const auto onset = static_cast<std::size_t>(std::llround(onset_s * sr));
  const auto n = onset + static_cast<std::size_t>(0.02 * sr);
  std::size_t best = onset + 10;
  float best_value = -1.0f;
  for (std::size_t i = 0; i < n; ++i) {
    const float y = stage.process_sample(i == onset ? 1.0f : 0.0f);
    if (i >= onset + 10 && y > best_value) {
      best_value = y;
      best = i;
    }
  }
  return static_cast<double>(best - onset) / sr * 1000.0;
}

/// Index k of the 1/6-octave tone around 1200 Hz with the highest megaphone gain.
int megaphone_peak_step(double sr, float tone) {
  int best_k = 0;
  double best = -1.0;
  for (int k = -9; k <= 9; ++k) {
    const double hz = 1200.0 * std::exp2(static_cast<double>(k) / 6.0);
    const double g = tone_gain(sr, CharacterType::Megaphone, 0.0f, tone, hz, 0.05f);
    if (g > best) {
      best = g;
      best_k = k;
    }
  }
  return best_k;
}

}  // namespace

TEST_CASE("megaphone attenuates outside its band", "[voice_character]") {
  for (double sr : {44100.0, 48000.0}) {
    CAPTURE(sr);
    double peak = 0.0;
    for (double hz : {800.0, 1200.0, 1700.0}) {
      peak = std::max(peak, tone_gain(sr, CharacterType::Megaphone, kAmount, 0.0f, hz, 0.05f));
    }
    REQUIRE(peak > 0.0);
    REQUIRE(db(tone_gain(sr, CharacterType::Megaphone, kAmount, 0.0f, 300.0, 0.05f) / peak) <=
            -6.0);
    REQUIRE(db(tone_gain(sr, CharacterType::Megaphone, kAmount, 0.0f, 4800.0, 0.05f) / peak) <=
            -6.0);
  }
}

TEST_CASE("megaphone saturates harder as amount grows", "[voice_character]") {
  const double soft = tone_gain(48000.0, CharacterType::Megaphone, 0.0f, 0.0f, 1200.0, 0.8f);
  const double hard = tone_gain(48000.0, CharacterType::Megaphone, 1.0f, 0.0f, 1200.0, 0.8f);
  // A 0.8 sine through a tanh drive loses fundamental gain relative to its small-signal value.
  const double soft_small = tone_gain(48000.0, CharacterType::Megaphone, 0.0f, 0.0f, 1200.0, 0.01f);
  const double hard_small = tone_gain(48000.0, CharacterType::Megaphone, 1.0f, 0.0f, 1200.0, 0.01f);
  REQUIRE(hard / hard_small < soft / soft_small);
}

TEST_CASE("radio attenuates outside its band", "[voice_character]") {
  for (double sr : {44100.0, 48000.0}) {
    CAPTURE(sr);
    double peak = 0.0;
    for (double hz : {700.0, 1000.0, 1500.0}) {
      peak = std::max(peak, tone_gain(sr, CharacterType::Radio, kAmount, 0.0f, hz, 0.5f));
    }
    REQUIRE(peak > 0.0);
    REQUIRE(db(tone_gain(sr, CharacterType::Radio, kAmount, 0.0f, 150.0, 0.5f) / peak) <= -18.0);
    REQUIRE(db(tone_gain(sr, CharacterType::Radio, kAmount, 0.0f, 6000.0, 0.5f) / peak) <= -18.0);
  }
}

TEST_CASE("strobo period follows the tone to rate mapping", "[voice_character]") {
  // tone -1 / 0 / +1 map to 4 / 10 / 16 Hz.
  const struct {
    float tone;
    double rate_hz;
  } cases[] = {{-1.0f, 4.0}, {0.0f, 10.0}, {1.0f, 16.0}};
  for (const auto& c : cases) {
    CAPTURE(c.tone);
    for (double sr : {44100.0, 48000.0}) {
      CAPTURE(sr);
      REQUIRE(std::abs(strobe_period(sr, c.tone, kAmount) * c.rate_hz - 1.0) < 0.005);
    }
  }
}

TEST_CASE("strobo edges are smoothed over 2 ms", "[voice_character]") {
  for (double sr : {44100.0, 48000.0}) {
    CAPTURE(sr);
    CharacterStage stage = make_stage(sr, CharacterType::Strobo, 1.0f, 0.0f);
    float prev = stage.process_sample(1.0f);
    float max_step = 0.0f;
    float min_value = 1.0f;
    for (std::size_t i = 1; i < static_cast<std::size_t>(0.5 * sr); ++i) {
      const float y = stage.process_sample(1.0f);
      max_step = std::max(max_step, std::abs(y - prev));
      min_value = std::min(min_value, y);
      prev = y;
    }
    REQUIRE(min_value < 0.01f);
    REQUIRE(std::abs(max_step * 0.002 * sr - 1.0) < 0.01);
  }
}

TEST_CASE("the megaphone centre follows the same physical frequency at every rate",
          "[voice_character]") {
  for (double sr : {22050.0, 44100.0, 48000.0}) {
    CAPTURE(sr);
    REQUIRE(megaphone_peak_step(sr, 0.0f) == 0);    // 1200 Hz
    REQUIRE(megaphone_peak_step(sr, 1.0f) == 6);    // 2400 Hz
    REQUIRE(megaphone_peak_step(sr, -1.0f) == -6);  // 600 Hz
  }
}

TEST_CASE("radio corners sit at 300 Hz and 3 kHz at every rate", "[voice_character]") {
  double hp[3] = {};
  double lp[3] = {};
  int i = 0;
  for (double sr : {22050.0, 44100.0, 48000.0}) {
    CAPTURE(sr);
    const double ref = tone_gain(sr, CharacterType::Radio, 0.0f, 0.0f, 1000.0, 0.5f);
    hp[i] = db(tone_gain(sr, CharacterType::Radio, 0.0f, 0.0f, 300.0, 0.5f) / ref);
    lp[i] = db(tone_gain(sr, CharacterType::Radio, 0.0f, 0.0f, 3000.0, 0.5f) / ref);
    // Fourth-order Butterworth: -3.01 dB at the corner.
    REQUIRE(hp[i] > -3.8);
    REQUIRE(hp[i] < -2.3);
    REQUIRE(lp[i] > -3.8);
    REQUIRE(lp[i] < -2.3);
    ++i;
  }
  for (int a = 0; a < 3; ++a) {
    for (int b = a + 1; b < 3; ++b) {
      REQUIRE(std::abs(hp[a] - hp[b]) < 0.3);
      REQUIRE(std::abs(lp[a] - lp[b]) < 0.3);
    }
  }
}

TEST_CASE("chorus delay is 7 ms swept by 2 ms at 0.8 Hz at every rate", "[voice_character]") {
  const double rates[] = {44100.0, 48000.0};
  // Phase 0 gives the centre delay, a quarter period (0.3125 s) the maximum, three quarters
  // the minimum.
  const struct {
    double onset_s;
    double delay_ms;
  } points[] = {{0.0, 7.0}, {0.3125, 9.0}, {0.9375, 5.0}};
  for (const auto& p : points) {
    CAPTURE(p.onset_s);
    double measured[2];
    for (int r = 0; r < 2; ++r) {
      measured[r] = chorus_peak_ms(rates[r], p.onset_s);
      REQUIRE(std::abs(measured[r] - p.delay_ms) < 0.12);
    }
    REQUIRE(std::abs(measured[0] - measured[1]) < 0.08);
  }
}

TEST_CASE("every type is finite and bounded at 22.05 kHz", "[voice_character]") {
  const double sr = 22050.0;
  const CharacterType types[] = {CharacterType::None, CharacterType::Megaphone,
                                 CharacterType::Radio, CharacterType::BbdChorus,
                                 CharacterType::Strobo};
  for (CharacterType type : types) {
    for (float tone : {-1.0f, 0.0f, 1.0f}) {
      CharacterStage stage = make_stage(sr, type, 1.0f, tone);
      for (std::size_t i = 0; i < 22050; ++i) {
        const float x = 0.5f * static_cast<float>(std::sin(kTwoPiD * 1000.0 * i / sr));
        const float y = stage.process_sample(x);
        REQUIRE(std::isfinite(y));
        REQUIRE(std::abs(y) < 4.0f);
      }
    }
  }
}

TEST_CASE("none passes audio through unchanged", "[voice_character]") {
  CharacterStage stage = make_stage(48000.0, CharacterType::None, 1.0f, 0.5f);
  for (int i = 0; i < 100; ++i) {
    const float x = 0.01f * static_cast<float>(i);
    REQUIRE(stage.process_sample(x) == x);
  }
}

TEST_CASE("character processing and reconfiguration do not allocate", "[voice_character]") {
  CharacterStage stage;
  stage.prepare(48000.0);
  const CharacterType types[] = {CharacterType::Megaphone, CharacterType::Radio,
                                 CharacterType::BbdChorus, CharacterType::Strobo,
                                 CharacterType::None};
  float sink = 0.0f;
  std::size_t allocations = 0;
  {
    sonare::test::AllocationGuard guard;
    for (CharacterType type : types) {
      CharacterConfig config;
      config.type = type;
      config.amount = 0.7f;
      config.tone = 0.3f;
      stage.set_config(config);
      for (int i = 0; i < 2048; ++i) sink += stage.process_sample(0.1f * std::sin(0.05f * i));
      stage.reset();
    }
    allocations = guard.count();
  }
  REQUIRE(allocations == 0);
  REQUIRE(std::isfinite(sink));
}
