/// @file gs_efx_receiver_range_test.cpp
/// @brief A translated GS EFX byte reaches its control unflattened: the
///        feedback, pre-delay and level a printed range promises are values the
///        receiving insert accepts, on construction and on automation alike.
///
/// Each case reads the quantity back off the audio rather than off a stored
/// field, at two sample rates, so a receiver clamping the top of a range shows
/// as two bytes rendering the same thing.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#if defined(SONARE_MIDI_WITH_FX) && defined(SONARE_WITH_MASTERING)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "effects/common/control_ranges.h"
#include "effects/modulation/chorus.h"
#include "effects/modulation/ensemble.h"
#include "effects/modulation/flanger.h"
#include "effects/modulation/pitch_shifter.h"
#include "mastering/api/insert_factory.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_efx_bindings.h"
#include "midi/synth/gs_efx_convert.h"
#include "midi/synth/gs_efx_tables.h"
#include "midi/synth/gs_layer.h"
#include "rt/processor_base.h"
#include "util/constants.h"

namespace {

namespace s = sonare::midi::synth;
namespace fx = sonare::effects::modulation;
using Catch::Approx;

constexpr std::array<double, 2> kRates = {44100.0, 48000.0};
constexpr int kBlock = 256;
constexpr int kImpulseAt = 64;
constexpr float kImpulse = 0.1f;

using Bytes = std::vector<std::pair<int, int>>;

std::string gs_json(uint16_t type, const Bytes& bytes) {
  std::string json =
      "{\"typeMsb\":" + std::to_string(type >> 8) + ",\"typeLsb\":" + std::to_string(type & 0x7F);
  for (const auto& [slot, value] : bytes) {
    json += ",\"byte" + std::to_string(slot) + "\":" + std::to_string(value);
  }
  return json + "}";
}

/// Runs @p proc over @p left / @p right in blocks, both legs in place.
void run(sonare::rt::ProcessorBase& proc, std::vector<float>& left, std::vector<float>& right) {
  for (size_t offset = 0; offset < left.size(); offset += kBlock) {
    const int n = static_cast<int>(std::min<size_t>(kBlock, left.size() - offset));
    float* channels[] = {left.data() + offset, right.data() + offset};
    proc.process(channels, 2, n);
  }
}

/// The left leg of @p proc's response to an impulse on both legs.
std::vector<float> impulse_response(sonare::rt::ProcessorBase& proc, size_t frames) {
  std::vector<float> left(frames, 0.0f);
  std::vector<float> right(frames, 0.0f);
  left[kImpulseAt] = kImpulse;
  right[kImpulseAt] = kImpulse;
  run(proc, left, right);
  for (size_t i = 0; i < frames; ++i) {
    REQUIRE(std::isfinite(left[i]));
    REQUIRE(std::isfinite(right[i]));
  }
  return left;
}

/// effects.gsEfx built with @p bytes, prepared at @p rate.
std::unique_ptr<sonare::rt::ProcessorBase> built(uint16_t type, const Bytes& bytes, double rate) {
  auto proc = sonare::mastering::api::make_insert("effects.gsEfx", gs_json(type, bytes));
  REQUIRE(proc != nullptr);
  proc->prepare(rate, kBlock);
  return proc;
}

/// effects.gsEfx built with @p base, then moved to @p byte at @p slot by
/// automation and reset, which is the path a live edit followed by a stop takes.
std::unique_ptr<sonare::rt::ProcessorBase> automated(uint16_t type, const Bytes& base, int slot,
                                                     int byte, double rate) {
  auto proc = built(type, base, rate);
  REQUIRE(proc->set_parameter(static_cast<unsigned int>(slot), static_cast<float>(byte)));
  proc->reset();
  return proc;
}

float max_difference(const std::vector<float>& a, const std::vector<float>& b) {
  REQUIRE(a.size() == b.size());
  float worst = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::fabs(a[i] - b[i]));
  return worst;
}

/// Sum of @p x over [centre - half, centre + half], which a fractional or
/// filtered echo keeps whole where a single sample would not.
double window_sum(const std::vector<float>& x, double centre, int half) {
  const int lo = std::max(0, static_cast<int>(std::lround(centre)) - half);
  const int hi =
      std::min(static_cast<int>(x.size()) - 1, static_cast<int>(std::lround(centre)) + half);
  double sum = 0.0;
  for (int i = lo; i <= hi; ++i) sum += x[static_cast<size_t>(i)];
  return sum;
}

/// The ratio of the second echo to the first for a loop of @p period samples.
double repeat_ratio(const std::vector<float>& x, double period) {
  const int half = static_cast<int>(period / 2.0) - 1;
  const double first = window_sum(x, kImpulseAt + period, half);
  const double second = window_sum(x, kImpulseAt + 2.0 * period, half);
  REQUIRE(std::fabs(first) > 0.5 * kImpulse);
  return second / first;
}

/// The control value the generated row for (@p type, @p slot) gives @p byte on
/// @p stage, so a receiver test is fed exactly what the chain would send.
float row_value(uint16_t type, uint8_t slot, std::string_view stage, uint8_t byte) {
  for (const s::GsEfxBindingRow& row : s::kGsEfxBindingRows) {
    if (row.type == type && row.slot == slot && s::kGsEfxRowStages[row.stage] == stage) {
      return s::gs_efx_binding_value(row, byte);
    }
  }
  FAIL("no row binds " << type << " slot " << int(slot) << " to " << stage);
  return 0.0f;
}

// The translated feedback range: 0F-71 against -98%..+98%, two points a byte.
struct FeedbackByte {
  uint8_t byte;
  double fraction;
};
constexpr std::array<FeedbackByte, 7> kFeedbackBytes = {{{0x0F, -0.98},
                                                         {0x10, -0.96},
                                                         {0x11, -0.94},
                                                         {0x40, 0.0},
                                                         {0x6F, 0.94},
                                                         {0x70, 0.96},
                                                         {0x71, 0.98}}};

// --- stereo delay through the GS unit ---------------------------------------

constexpr uint16_t kStereoDelay = 0x0150;
constexpr int kFeedbackSlot = 2;

/// Fixed 2.1875 ms lines, each fed back into itself, damping off, all effect,
/// flat tone and unity level.
Bytes stereo_delay_bytes(int feedback) {
  return {{0, 22},  {1, 22},  {kFeedbackSlot, feedback}, {3, 0}, {7, 127}, {15, 127}, {16, 64},
          {17, 64}, {19, 127}};
}

TEST_CASE("every translated stereo-delay feedback byte reaches its own loop gain",
          "[gs][gs-efx][effects]") {
  for (const double rate : kRates) {
    INFO("rate " << rate);
    const double period = s::gs_efx_delay_ms(22, s::GsTimeLadder::kLadder3) * 0.001 * rate;
    std::vector<std::vector<float>> responses;
    for (const FeedbackByte& fb : kFeedbackBytes) {
      INFO("byte " << int(fb.byte));
      const auto made = built(kStereoDelay, stereo_delay_bytes(fb.byte), rate);
      const std::vector<float> x = impulse_response(*made, 4096);
      if (fb.fraction == 0.0) {
        // An open loop leaves the first echo alone.
        CHECK(std::fabs(window_sum(x, kImpulseAt + 2.0 * period, 3)) < 1e-6);
      } else {
        CHECK(std::fabs(repeat_ratio(x, period) - fb.fraction) < 1e-3);
      }
      const auto moved =
          automated(kStereoDelay, stereo_delay_bytes(0x40), kFeedbackSlot, fb.byte, rate);
      CHECK(max_difference(impulse_response(*moved, 4096), x) < 1e-6f);
      responses.push_back(x);
    }
    // Neighbouring endpoint bytes stay apart rather than sharing a ceiling.
    CHECK(max_difference(responses[0], responses[1]) > 1e-4f);
    CHECK(max_difference(responses[5], responses[6]) > 1e-4f);
  }
}

// --- the modulated-delay receivers ------------------------------------------

constexpr float kLoopMs = 20.0f;

/// Feedback bytes fed to a receiver built from @p make, read off the echo ratio.
template <typename Make>
void check_feedback_ratios(Make make, uint16_t type, uint8_t slot, std::string_view stage) {
  for (const double rate : kRates) {
    INFO("rate " << rate);
    std::vector<double> ratios;
    for (const FeedbackByte& fb : kFeedbackBytes) {
      if (fb.fraction == 0.0) continue;
      INFO("byte " << int(fb.byte));
      const float value = row_value(type, slot, stage, fb.byte);
      REQUIRE(value == static_cast<float>(fb.fraction));
      std::unique_ptr<sonare::rt::ProcessorBase> proc = make(value);
      proc->prepare(rate, kBlock);
      const std::vector<float> x = impulse_response(*proc, 8192);
      // The loop closes up to one sample past the line; the windows cover both.
      const double ratio = repeat_ratio(x, kLoopMs * 0.001 * rate + 0.5);
      CHECK(std::fabs(ratio - fb.fraction) < 2e-3);
      ratios.push_back(ratio);
    }
    // 94, 96 and 98% stay three distinct loops on each side.
    REQUIRE(ratios.size() == 6);
    CHECK(ratios[1] - ratios[0] > 0.01);
    CHECK(ratios[2] - ratios[1] > 0.01);
    CHECK(ratios[4] - ratios[3] > 0.01);
    CHECK(ratios[5] - ratios[4] > 0.01);
  }
}

TEST_CASE("the chorus takes the whole translated feedback range", "[gs][gs-efx][effects]") {
  check_feedback_ratios(
      [](float feedback) {
        fx::ChorusConfig config;
        config.rate_hz = 0.0f;
        config.depth_ms = 0.0f;
        config.center_delay_ms = kLoopMs;
        config.dry_wet = 1.0f;
        config.feedback = feedback;
        return std::make_unique<fx::Chorus>(config);
      },
      0x0400, 14, "effects.modulation.chorus");
}

TEST_CASE("the flanger takes the whole translated feedback range", "[gs][gs-efx][effects]") {
  check_feedback_ratios(
      [](float feedback) {
        fx::FlangerConfig config;
        config.rate_hz = 0.0f;
        config.depth_ms = 0.0f;
        config.center_delay_ms = kLoopMs;
        config.dry_wet = 1.0f;
        config.feedback = feedback;
        return std::make_unique<fx::Flanger>(config);
      },
      0x0123, 5, "effects.modulation.flanger");
}

TEST_CASE("the pitch shifter takes the whole translated feedback range", "[gs][gs-efx][effects]") {
  check_feedback_ratios(
      [](float feedback) {
        fx::PitchShifterConfig config;
        config.semitones = 0.0f;
        config.dry_wet = 1.0f;
        config.pre_delay_ms = kLoopMs;
        config.feedback = feedback;
        return std::make_unique<fx::PitchShifter>(config);
      },
      0x0161, 2, "effects.modulation.pitchShifter");
}

TEST_CASE("feedback past the shared limit stops at it on every receiver", "[gs][gs-efx][effects]") {
  using sonare::effects::common::kMaxFeedback;
  constexpr float kFarPast = 40.0f;
  fx::ChorusConfig chorus;
  chorus.rate_hz = 0.0f;
  chorus.depth_ms = 0.0f;
  chorus.center_delay_ms = kLoopMs;
  chorus.dry_wet = 1.0f;
  chorus.feedback = kFarPast;
  fx::FlangerConfig flanger;
  flanger.rate_hz = 0.0f;
  flanger.depth_ms = 0.0f;
  flanger.center_delay_ms = kLoopMs;
  flanger.dry_wet = 1.0f;
  flanger.feedback = kFarPast;
  fx::PitchShifterConfig shifter;
  shifter.dry_wet = 1.0f;
  shifter.pre_delay_ms = kLoopMs;
  shifter.feedback = kFarPast;
  std::vector<std::unique_ptr<sonare::rt::ProcessorBase>> procs;
  procs.push_back(std::make_unique<fx::Chorus>(chorus));
  procs.push_back(std::make_unique<fx::Flanger>(flanger));
  procs.push_back(std::make_unique<fx::PitchShifter>(shifter));
  for (const auto& proc : procs) {
    proc->prepare(48000.0, kBlock);
    const std::vector<float> x = impulse_response(*proc, 8192);
    CHECK(std::fabs(repeat_ratio(x, kLoopMs * 0.001 * 48000.0 + 0.5) - kMaxFeedback) < 2e-3);
  }
}

// --- pre-delay --------------------------------------------------------------

/// Index of the largest magnitude in @p x.
size_t peak_index(const std::vector<float>& x) {
  size_t best = 0;
  for (size_t i = 1; i < x.size(); ++i) {
    if (std::fabs(x[i]) > std::fabs(x[best])) best = i;
  }
  return best;
}

/// The first sample past a small fraction of the impulse.
size_t onset_index(const std::vector<float>& x) {
  for (size_t i = kImpulseAt + 1; i < x.size(); ++i) {
    if (std::fabs(x[i]) > 1e-3f * kImpulse) return i;
  }
  return x.size();
}

constexpr uint16_t kChorus = 0x0143;

/// A still chorus: no depth, all effect, flat tone and unity level.
Bytes chorus_bytes(int pre_delay) {
  return {{0, pre_delay}, {2, 0}, {15, 127}, {16, 64}, {17, 64}, {19, 127}};
}

TEST_CASE("the GS chorus pre-delay reaches the top of its ladder", "[gs][gs-efx][effects]") {
  for (const double rate : kRates) {
    INFO("rate " << rate);
    std::vector<std::vector<float>> responses;
    for (const uint8_t byte :
         {uint8_t{80}, uint8_t{96}, uint8_t{112}, uint8_t{126}, uint8_t{127}}) {
      INFO("byte " << int(byte));
      const float ms = s::gs_efx_delay_ms(byte, s::GsTimeLadder::kLadder0);
      const auto made = built(kChorus, chorus_bytes(byte), rate);
      const std::vector<float> x = impulse_response(*made, 8192);
      const double expected = kImpulseAt + ms * 0.001 * rate;
      CHECK(std::fabs(static_cast<double>(peak_index(x)) - expected) <= 2.0);
      const auto moved = automated(kChorus, chorus_bytes(80), 0, byte, rate);
      CHECK(max_difference(impulse_response(*moved, 8192), x) < 1e-6f);
      responses.push_back(x);
    }
    // 74 ms and 100 ms are two delays, and 126 and 127 are the same one.
    CHECK(peak_index(responses[3]) - peak_index(responses[2]) > 0.02 * rate);
    CHECK(max_difference(responses[3], responses[4]) == 0.0f);
  }
}

constexpr uint16_t kEnsemble = 0x0140;

/// An ensemble with its byte-reachable sweep and spread at zero.
Bytes ensemble_bytes(int pre_delay) {
  return {{0, pre_delay}, {2, 0},   {3, 0},   {4, 0x40}, {5, 0},
          {15, 127},      {16, 64}, {17, 64}, {19, 127}};
}

TEST_CASE("the GS ensemble pre-delay reaches the top of its ladder", "[gs][gs-efx][effects]") {
  // The fast sweep has no byte, so it bounds how close the onset can sit.
  const float fast_ms = fx::EnsembleConfig{}.depth_fast_ms;
  for (const double rate : kRates) {
    INFO("rate " << rate);
    std::vector<size_t> onsets;
    for (const uint8_t byte : {uint8_t{90}, uint8_t{110}, uint8_t{125}}) {
      INFO("byte " << int(byte));
      const float ms = s::gs_efx_delay_ms(byte, s::GsTimeLadder::kLadder0);
      const auto made = built(kEnsemble, ensemble_bytes(byte), rate);
      const std::vector<float> x = impulse_response(*made, 8192);
      const size_t onset = onset_index(x);
      const double expected = kImpulseAt + ms * 0.001 * rate;
      CHECK(std::fabs(static_cast<double>(onset) - expected) <= fast_ms * 0.001 * rate + 2.0);
      const auto moved = automated(kEnsemble, ensemble_bytes(90), 0, byte, rate);
      CHECK(max_difference(impulse_response(*moved, 8192), x) < 1e-6f);
      onsets.push_back(onset);
    }
    CHECK(onsets[1] > onsets[0] + static_cast<size_t>(0.02 * rate));
    CHECK(onsets[2] > onsets[1] + static_cast<size_t>(0.02 * rate));
  }
}

TEST_CASE("the ensemble line covers the shared pre-delay ceiling with every spread",
          "[gs][gs-efx][effects]") {
  for (const double rate : kRates) {
    INFO("rate " << rate);
    for (const float centre : {70.0f, sonare::effects::common::kMaxModulationPreDelayMs}) {
      INFO("centre " << centre);
      fx::EnsembleConfig config;
      config.depth_slow_ms = 0.0f;
      config.depth_fast_ms = 0.0f;
      config.center_delay_ms = centre;
      config.dry_wet = 1.0f;
      config.tone_hz = 20000.0f;
      fx::Ensemble ensemble(config);
      ensemble.prepare(rate, kBlock);
      const std::vector<float> x = impulse_response(ensemble, 8192);
      CHECK(std::fabs(static_cast<double>(onset_index(x)) - (kImpulseAt + centre * 0.001 * rate)) <=
            1.0);
      // The latest voice of the widest spread still reads inside the line.
      fx::EnsembleConfig spread = config;
      spread.pre_delay_dev_ms = 20.0f;
      spread.depth_slow_ms = 10.0f;
      spread.depth_fast_ms = 10.0f;
      spread.depth_dev = 1.0f;
      spread.rate_slow_hz = 0.0f;
      spread.rate_fast_hz = 0.0f;
      fx::Ensemble wide(spread);
      wide.prepare(rate, kBlock);
      const std::vector<float> y = impulse_response(wide, 16384);
      double energy = 0.0;
      for (const float v : y) energy += static_cast<double>(v) * v;
      CHECK(energy > 0.0);
    }
  }
}

// --- output level -----------------------------------------------------------

constexpr uint16_t kEq = 0x0100;
constexpr int kLevelSlot = 19;

double rms(const std::vector<float>& x) {
  double sum = 0.0;
  for (const float v : x) sum += static_cast<double>(v) * v;
  return std::sqrt(sum / static_cast<double>(x.size()));
}

/// RMS of @p proc's response to a steady tone.
double tone_level(sonare::rt::ProcessorBase& proc, double rate) {
  std::vector<float> left(8192);
  for (size_t i = 0; i < left.size(); ++i) {
    left[i] = 0.25f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * 1000.0 *
                                                  static_cast<double>(i) / rate));
  }
  std::vector<float> right = left;
  run(proc, left, right);
  return rms(left);
}

TEST_CASE("a low GS output level keeps its own amplitude", "[gs][gs-efx][effects]") {
  for (const double rate : kRates) {
    INFO("rate " << rate);
    const auto full = built(kEq, {{kLevelSlot, 127}}, rate);
    const double reference = tone_level(*full, rate);
    REQUIRE(reference > 0.01);
    const std::array<std::pair<uint8_t, double>, 5> levels = {{{4, 3.0 / 127.0},
                                                               {8, 6.0 / 127.0},
                                                               {12, s::gs_efx_level_mul(12)},
                                                               {64, s::gs_efx_level_mul(64)},
                                                               {127, 1.0}}};
    for (const auto& [byte, gain] : levels) {
      INFO("byte " << int(byte));
      const auto made = built(kEq, {{kLevelSlot, byte}}, rate);
      CHECK(std::fabs(tone_level(*made, rate) / reference - gain) < 1e-4 * gain + 1e-7);
      const auto moved = automated(kEq, {{kLevelSlot, 127}}, kLevelSlot, byte, rate);
      CHECK(std::fabs(tone_level(*moved, rate) / reference - gain) < 1e-4 * gain + 1e-7);
    }
    // The silent byte lands on the level floor rather than on an audible step.
    const auto silent = built(kEq, {{kLevelSlot, 0}}, rate);
    const double floor = std::pow(10.0, sonare::effects::common::kLevelFloorDb / 20.0);
    CHECK(tone_level(*silent, rate) / reference <= floor * 1.01);
  }
}

/// The number @p key carries in @p json, which the chain writes as a bare number.
float json_number(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  const size_t at = json.find(needle);
  REQUIRE(at != std::string::npos);
  return std::stof(json.substr(at + needle.size()));
}

TEST_CASE("translated and carried level rows hand the chain the measured dB", "[gs][gs-efx]") {
  struct Case {
    uint16_t type;
    uint8_t slot;
    const char* stage;
    const char* key;
  };
  // A translated output level, and a level the rotary's drum and horn carry.
  for (const Case& c : {Case{0x0100, 19, "utility.gain", "levelDb"},
                        Case{0x0122, 3, "effects.modulation.rotary", "drumLevelDb"},
                        Case{0x0122, 7, "effects.modulation.rotary", "hornLevelDb"}}) {
    INFO(c.stage << "." << c.key);
    s::GsEfx efx;
    efx.type = c.type;
    efx.type_msb = static_cast<uint8_t>(c.type >> 8);
    efx.assigned = true;
    for (size_t slot = 0; slot < efx.params.size(); ++slot) {
      efx.params[slot] = s::gs_efx_parameter_reset_default(c.type, static_cast<uint8_t>(slot));
    }
    for (const uint8_t byte : {uint8_t{4}, uint8_t{8}}) {
      efx.params[c.slot] = byte;
      std::string params;
      for (const s::GsEfxStage& stage : s::gs_efx_insert_chain(efx)) {
        if (stage.name == c.stage) params = stage.params_json;
      }
      CHECK(json_number(params, c.key) ==
            Approx(20.0f * std::log10(s::gs_efx_level_mul(byte))).margin(1e-4));
    }
  }
}

// --- balance ----------------------------------------------------------------

/// @p bytes with @p slot set to @p value.
Bytes with_byte(Bytes bytes, int slot, int value) {
  for (auto& [at, byte] : bytes) {
    if (at == slot) byte = value;
  }
  return bytes;
}

/// The direct and effect gains @p x carries: the impulse itself, and the echo
/// summed over a window around @p echo_at.
std::pair<double, double> projected_gains(const std::vector<float>& x, double echo_at) {
  return {x[kImpulseAt] / kImpulse, window_sum(x, echo_at, 3) / kImpulse};
}

TEST_CASE("a GS balance byte reaches both measured gains, level as well as ratio",
          "[gs][gs-efx][effects]") {
  struct Expected {
    uint8_t byte;
    double direct;
    double effect;
  };
  // The two ramps meet at full in the centre rather than crossing at half.
  const std::array<Expected, 5> bytes = {
      {{0, 1.0, 0.0}, {32, 1.0, 0.359375}, {64, 1.0, 1.0}, {96, 0.359375, 1.0}, {127, 0.0, 1.0}}};
  for (const double rate : kRates) {
    INFO("rate " << rate);
    // The stereo delay with its loop open, and a still chorus.
    const double period = s::gs_efx_delay_ms(22, s::GsTimeLadder::kLadder3) * 0.001 * rate;
    const double chorus_at =
        kImpulseAt + s::gs_efx_delay_ms(80, s::GsTimeLadder::kLadder0) * 0.001 * rate;
    for (const Expected& e : bytes) {
      INFO("byte " << int(e.byte));
      float direct = 0.0f;
      float effect = 0.0f;
      s::gs_efx_balance(e.byte, &direct, &effect);
      REQUIRE(direct == static_cast<float>(e.direct));
      REQUIRE(effect == static_cast<float>(e.effect));

      Bytes delay = with_byte(stereo_delay_bytes(0x40), 15, e.byte);
      const auto made = built(kStereoDelay, delay, rate);
      const std::vector<float> x = impulse_response(*made, 4096);
      const auto [d, w] = projected_gains(x, kImpulseAt + period);
      CHECK(std::fabs(d - e.direct) < 1e-4);
      CHECK(std::fabs(w - e.effect) < 1e-4);
      const auto moved = automated(kStereoDelay, stereo_delay_bytes(0x40), 15, e.byte, rate);
      CHECK(max_difference(impulse_response(*moved, 4096), x) < 1e-6f);

      Bytes chorus = with_byte(chorus_bytes(80), 15, e.byte);
      const auto voiced = built(kChorus, chorus, rate);
      const std::vector<float> y = impulse_response(*voiced, 8192);
      const auto [cd, cw] = projected_gains(y, chorus_at);
      CHECK(std::fabs(cd - e.direct) < 1e-4);
      CHECK(std::fabs(cw - e.effect) < 1e-4);
    }
  }
}

TEST_CASE("every stage a balance row reaches runs the two-ramp law", "[gs][gs-efx]") {
  size_t checked = 0;
  for (const s::GsEfxBindingRow& row : s::kGsEfxBindingRows) {
    if (row.conv_class != s::kGsEfxClassBalance || row.law.form != s::kGsEfxFormNone) continue;
    INFO("type " << row.type << " slot " << int(row.slot) << " -> "
                 << s::kGsEfxRowStages[row.stage]);
    s::GsEfx efx;
    efx.type = row.type;
    efx.type_msb = static_cast<uint8_t>(row.type >> 8);
    efx.assigned = true;
    for (size_t slot = 0; slot < efx.params.size(); ++slot) {
      efx.params[slot] = s::gs_efx_parameter_reset_default(row.type, static_cast<uint8_t>(slot));
    }
    bool found = false;
    for (const s::GsEfxStage& stage : s::gs_efx_insert_chain(efx)) {
      if (stage.name != s::kGsEfxRowStages[row.stage] || stage.ordinal != row.ordinal) continue;
      found = true;
      CHECK(stage.params_json.find("\"mixLaw\":1") != std::string::npos);
    }
    CHECK(found);
    ++checked;
  }
  CHECK(checked > 40);
}

}  // namespace

#endif  // SONARE_MIDI_WITH_FX && SONARE_WITH_MASTERING
