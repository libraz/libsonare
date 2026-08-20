#include <algorithm>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "mastering/common/hysteresis_ja.h"
#include "mastering/saturation/tape.h"
#include "mastering/saturation/transformer.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;
using sonare::mastering::saturation::Tape;
using sonare::mastering::saturation::TapeConfig;
using sonare::mastering::saturation::Transformer;
using sonare::mastering::saturation::TransformerConfig;

namespace common = sonare::mastering::common;

namespace {

using sonare::constants::kTwoPi;

constexpr double kSampleRate = 48000.0;
constexpr int kTailLength = 24000;   // 500 ms
constexpr int kSettleIndex = 12000;  // 250 ms

// Drives a stage with a single impulse and returns the whole buffer, so a caller
// can look at both the transient and the tail that follows it.
template <typename Stage>
std::vector<float> impulse_response(Stage& stage, float amplitude) {
  stage.prepare(kSampleRate, kTailLength);
  std::vector<float> buffer(static_cast<size_t>(kTailLength), 0.0f);
  buffer[0] = amplitude;
  float* channels[] = {buffer.data()};
  stage.process(channels, 1, static_cast<int>(buffer.size()));
  return buffer;
}

float peak_magnitude(const std::vector<float>& buffer) {
  float peak = 0.0f;
  for (float sample : buffer) peak = std::max(peak, std::abs(sample));
  return peak;
}

// Output of the last full sine cycle sampled once while the input is rising
// through `level` and once while it is falling through it. A memoryless
// transfer curve returns the same output for both; a hysteresis loop does not.
struct LoopBranches {
  float rising = 0.0f;
  float falling = 0.0f;
};

constexpr int kSineLength = 4096;
constexpr float kSineFrequency = 100.0f;

std::vector<float> sine_input(float amplitude) {
  std::vector<float> input(static_cast<size_t>(kSineLength), 0.0f);
  for (int i = 0; i < kSineLength; ++i) {
    input[static_cast<size_t>(i)] =
        amplitude * std::sin(kTwoPi * kSineFrequency * static_cast<float>(i) / kSampleRate);
  }
  return input;
}

template <typename Stage>
std::vector<float> sine_response(Stage& stage, float amplitude) {
  stage.prepare(kSampleRate, kSineLength);
  std::vector<float> output = sine_input(amplitude);
  float* channels[] = {output.data()};
  stage.process(channels, 1, kSineLength);
  return output;
}

template <typename Stage>
LoopBranches loop_branches(Stage& stage, float amplitude, float level) {
  const int period = static_cast<int>(kSampleRate / kSineFrequency);
  const std::vector<float> input = sine_input(amplitude);
  const std::vector<float> output = sine_response(stage, amplitude);

  LoopBranches branches;
  for (int i = kSineLength - period + 1; i < kSineLength; ++i) {
    const size_t n = static_cast<size_t>(i);
    if (std::abs(input[n] - level) > 0.01f) continue;
    if (input[n] > input[n - 1]) {
      branches.rising = output[n];
    } else {
      branches.falling = output[n];
    }
  }
  return branches;
}

// Peak output for one frequency at a ladder of input amplitudes.
template <typename Stage>
std::vector<float> level_response(float hz, const std::vector<float>& amplitudes) {
  std::vector<float> peaks;
  for (float amplitude : amplitudes) {
    Stage stage{};
    std::vector<float> signal(static_cast<size_t>(kSineLength), 0.0f);
    for (int i = 0; i < kSineLength; ++i) {
      signal[static_cast<size_t>(i)] =
          amplitude * std::sin(kTwoPi * hz * static_cast<float>(i) / kSampleRate);
    }
    stage.prepare(kSampleRate, kSineLength);
    float* channels[] = {signal.data()};
    stage.process(channels, 1, kSineLength);
    peaks.push_back(peak_magnitude(signal));
  }
  return peaks;
}

// Same reading for a tape whose config the caller supplies. The band case below
// is generic over stages and so can only use their default configs; the drive
// ceiling case needs a tape configured away from its defaults.
std::vector<float> tape_level_response(const TapeConfig& config, float hz,
                                       const std::vector<float>& amplitudes) {
  std::vector<float> peaks;
  for (float amplitude : amplitudes) {
    Tape stage{config};
    std::vector<float> signal(static_cast<size_t>(kSineLength), 0.0f);
    for (int i = 0; i < kSineLength; ++i) {
      signal[static_cast<size_t>(i)] =
          amplitude * std::sin(kTwoPi * hz * static_cast<float>(i) / kSampleRate);
    }
    stage.prepare(kSampleRate, kSineLength);
    float* channels[] = {signal.data()};
    stage.process(channels, 1, kSineLength);
    peaks.push_back(peak_magnitude(signal));
  }
  return peaks;
}

}  // namespace

// The property, stated without reference to any particular output number: over
// the whole audio band the stage must stay bounded and must keep responding to
// input level. A single Euler step per sample loses the loop once the drive
// field moves by about the coercivity in one sample, and the magnetization then
// runs into the saturation clamp - which is a slew limit, not a level limit, so
// the failure is a stage whose output peak does not move at all across a wide
// range of input levels while sitting above full scale.
TEMPLATE_TEST_CASE("Saturation stays bounded and level-responsive across the band",
                   "[mastering][saturation]", Tape, Transformer) {
  const std::vector<float> amplitudes = {0.1f, 0.25f, 0.5f, 0.9f};
  for (float hz : {100.0f, 1000.0f, 3000.0f, 6000.0f, 12000.0f, 16000.0f}) {
    CAPTURE(hz);
    const auto peaks = level_response<TestType>(hz, amplitudes);
    for (size_t i = 0; i < peaks.size(); ++i) {
      CAPTURE(amplitudes[i], peaks[i]);
      // Saturation magnetization is 1.0, so a peak at or above it means the
      // state reached the clamp rather than the loop.
      REQUIRE(peaks[i] < 1.0f);
    }
    // Every level step must move the output. Pinned output is bit-identical
    // across levels, so any margin above 1.0 catches it; 1.1 leaves room for
    // the compression a saturator legitimately applies at the top of the ladder.
    for (size_t i = 1; i < peaks.size(); ++i) {
      CAPTURE(amplitudes[i - 1], peaks[i - 1], amplitudes[i], peaks[i]);
      REQUIRE(peaks[i] > peaks[i - 1] * 1.1f);
    }
  }
}

// The same property at the top of the documented drive range. tape.h recommends
// drive up to +24 dB, and the sub-step budget is finite, so there a
// high-frequency drive asks for far more sub-steps than the core will take and
// each sub-step is back above the field change a plain Euler step tracks. The
// band case above runs at the default drive, where the budget is never
// exhausted, so it does not reach this regime.
TEST_CASE("Tape stays bounded and level-responsive at the documented drive ceiling",
          "[mastering][saturation]") {
  // head_bump and gap_loss are linear filters with their own memory, so they are
  // switched off here; what is left is the J-A core and static gain, and a peak
  // read below is the core's.
  TapeConfig config{};
  config.drive_db = 24.0f;
  config.head_bump_db = 0.0f;
  config.gap_loss = 0.0f;

  const std::vector<float> amplitudes = {0.25f, 0.5f, 1.0f};
  for (float hz : {6000.0f, 12000.0f, 16000.0f}) {
    CAPTURE(hz);
    const auto peaks = tape_level_response(config, hz, amplitudes);
    for (size_t i = 0; i < peaks.size(); ++i) {
      CAPTURE(amplitudes[i], peaks[i]);
      // Strictly below the +/-1.2*Ms safety clamp, so a run that reaches the
      // clamp fails. The bound is not 1.0 because the differential form of the
      // loop equation carries the magnetization a few percent past the
      // saturation magnetization at this drive - measured 1.05 at 16 kHz - which
      // is a property of the formulation and not of the step size.
      REQUIRE(peaks[i] < 1.1f);
    }
    // Every level step must still move the output. The stage is deep into
    // compression here, so the step is small - under two percent for the top
    // step at 6 and 12 kHz - and the margin is set by that rather than by the
    // failure it separates from: a stage pinned at the clamp returns the same
    // peak for every level, so its step ratio is exactly 1.
    for (size_t i = 1; i < peaks.size(); ++i) {
      CAPTURE(amplitudes[i - 1], peaks[i - 1], amplitudes[i], peaks[i]);
      REQUIRE(peaks[i] > peaks[i - 1] * 1.01f);
    }
  }
}

TEST_CASE("Tape magnetization returns toward rest after a transient", "[mastering][saturation]") {
  constexpr float kImpulse = 0.5f;
  Tape tape{TapeConfig{}};
  const auto response = impulse_response(tape, kImpulse);

  // The impulse really did drive the core, so the decay below is a decay and
  // not a stage that never moved.
  REQUIRE(peak_magnitude(response) > kImpulse);

  const float settled = response[static_cast<size_t>(kSettleIndex)];
  REQUIRE(std::abs(settled) < 0.01f);
  REQUIRE(std::abs(response.back()) < 0.001f);
  // Latching pins the state bit-exactly at the +/-1.2*Ms safety clamp.
  REQUIRE(std::abs(settled) < 1.19f);
}

TEST_CASE("Transformer magnetization returns toward rest after a transient",
          "[mastering][saturation]") {
  constexpr float kImpulse = 0.5f;
  Transformer driven{TransformerConfig{}};
  const auto response = impulse_response(driven, kImpulse);

  REQUIRE(peak_magnitude(response) > kImpulse);

  const float settled = response[static_cast<size_t>(kSettleIndex)];
  REQUIRE(std::abs(settled) < 0.01f);
  REQUIRE(std::abs(settled) < 1.19f);

  // The rest value is set by the asymmetry bias, not by what the stage last saw:
  // the tail after a transient must agree with the tail from an untouched stage.
  Transformer idle{TransformerConfig{}};
  const auto quiet = impulse_response(idle, 0.0f);
  REQUIRE_THAT(response.back(), WithinAbs(quiet.back(), 1e-4f));
}

TEST_CASE("Held-field relaxation keeps the hysteresis loop open", "[mastering][saturation]") {
  // head_bump and gap_loss are linear filters with their own memory, so they are
  // switched off here; what is left is the J-A core and static gain, and any
  // rising/falling split then comes from the loop itself.
  TapeConfig config{};
  config.head_bump_db = 0.0f;
  config.gap_loss = 0.0f;
  Tape tape{config};
  const auto tape_loop = loop_branches(tape, 0.6f, 0.3f);
  REQUIRE(tape_loop.falling - tape_loop.rising > 0.05f);

  Transformer transformer{TransformerConfig{}};
  const auto transformer_loop = loop_branches(transformer, 0.6f, 0.3f);
  REQUIRE(transformer_loop.falling - transformer_loop.rising > 0.05f);
}

TEST_CASE("Held-field relaxation is opt-in at the shared engine", "[mastering][saturation]") {
  auto config = common::jiles_atherton_presets::silicon_steel();
  auto latching = config;
  latching.viscosity_time_constant_s = 0.0f;

  common::JilesAtherton relaxing(config);
  common::JilesAtherton held(latching);
  common::JilesAtherton defaulted(config);
  common::JilesAthertonState relaxing_state;
  common::JilesAthertonState held_state;
  common::JilesAthertonState defaulted_state;

  float relaxing_tail = 0.0f;
  float held_tail = 0.0f;
  float defaulted_tail = 0.0f;
  for (int i = 0; i < kTailLength; ++i) {
    const float field = i == 0 ? 0.8f : 0.0f;
    relaxing_tail = relaxing.process(relaxing_state, field, static_cast<float>(kSampleRate));
    held_tail = held.process(held_state, field, static_cast<float>(kSampleRate));
    // The two-argument form supplies no sample rate and must stay rate-independent.
    defaulted_tail = defaulted.process(defaulted_state, field);
  }

  // The latching config keeps whatever the transient left behind - a remanent
  // magnetization, measured 0.396 here - while the relaxing one decays to rest.
  // The tail is bounded on both sides: too small is a config that relaxed when
  // it was told not to, and a tail at or above the saturation magnetization is
  // a state pinned at the +/-1.2*Ms clamp rather than sitting on the loop.
  REQUIRE(std::abs(held_tail) > 0.1f);
  REQUIRE(std::abs(held_tail) < config.saturation_magnetization);
  REQUIRE(std::abs(relaxing_tail) < 0.001f);
  REQUIRE_THAT(defaulted_tail, WithinAbs(held_tail, 0.0f));
}

TEST_CASE("Relaxed tape still saturates", "[mastering][saturation]") {
  // Peak gain must fall as level rises; a stage that relaxed its way into
  // linearity would keep the same ratio.
  TapeConfig config{};
  config.drive_db = 12.0f;
  config.head_bump_db = 0.0f;
  config.gap_loss = 0.0f;
  Tape quiet_tape{config};
  Tape loud_tape{config};
  const float quiet_gain = peak_magnitude(sine_response(quiet_tape, 0.02f)) / 0.02f;
  const float loud_gain = peak_magnitude(sine_response(loud_tape, 1.0f)) / 1.0f;
  REQUIRE(loud_gain < quiet_gain * 0.5f);
}
