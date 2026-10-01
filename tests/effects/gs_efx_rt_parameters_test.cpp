/// @file gs_efx_rt_parameters_test.cpp
/// @brief Realtime descriptors and live updates for GS-bound effect controls.

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "effects/modulation/pitch_shifter.h"
#include "effects/modulation/rotary.h"
#include "effects/reverb/dattorro_reverb.h"
#include "mastering/api/insert_factory.h"
#include "mastering/saturation/bitcrusher.h"
#include "support/alloc_guard.h"
#include "support/audio_fixtures.h"

#ifdef SONARE_WITH_FX

namespace {

using sonare::effects::modulation::PitchShifter;
using sonare::effects::modulation::PitchShifterConfig;
using sonare::effects::modulation::Rotary;
using sonare::effects::modulation::RotaryConfig;
using sonare::effects::reverb::DattorroReverb;
using sonare::effects::reverb::DattorroReverbConfig;
using sonare::mastering::api::insert_param_info_json;
using sonare::mastering::api::make_insert;
using sonare::mastering::saturation::BitCrusher;
using sonare::mastering::saturation::BitCrusherConfig;

constexpr double kRate = 48000.0;

bool descriptor_for(const sonare::rt::ProcessorBase& processor, const std::string& key,
                    unsigned int* id) {
  for (const auto& descriptor : processor.parameter_descriptors()) {
    if (descriptor.key != key) continue;
    *id = descriptor.id;
    return true;
  }
  return false;
}

template <typename Processor>
std::vector<float> render_stereo(Processor& processor, std::vector<float> left,
                                 std::vector<float> right) {
  processor.prepare(kRate, static_cast<int>(left.size()));
  float* channels[] = {left.data(), right.data()};
  processor.process(channels, 2, static_cast<int>(left.size()));
  left.insert(left.end(), right.begin(), right.end());
  return left;
}

template <typename Processor>
std::vector<float> process_stereo(Processor& processor, std::vector<float> left,
                                  std::vector<float> right) {
  float* channels[] = {left.data(), right.data()};
  processor.process(channels, 2, static_cast<int>(left.size()));
  left.insert(left.end(), right.begin(), right.end());
  return left;
}

std::vector<float> render_mono(BitCrusher& processor, std::vector<float> samples) {
  processor.prepare(kRate, static_cast<int>(samples.size()));
  float* channels[] = {samples.data()};
  processor.process(channels, 1, static_cast<int>(samples.size()));
  return samples;
}

std::vector<float> input_train(int length, int offset = 0) {
  std::vector<float> samples(static_cast<std::size_t>(length));
  for (int i = 0; i < length; ++i) {
    const double n = static_cast<double>(i + offset);
    samples[static_cast<std::size_t>(i)] =
        static_cast<float>(0.2 * std::sin(0.013 * n) + 0.07 * std::cos(0.037 * n));
  }
  return samples;
}

template <typename Processor>
void require_descriptor(const Processor& processor, const char* key, unsigned int id) {
  unsigned int actual_id = 0;
  REQUIRE(descriptor_for(processor, key, &actual_id));
  REQUIRE(actual_id == id);
  REQUIRE(processor.parameter_is_realtime_safe(id));
}

template <typename Processor>
void require_rt_setter_without_allocation(Processor& processor, unsigned int id, float value) {
  bool accepted = false;
  std::size_t allocations = 0;
  {
    sonare::test::AllocationGuard guard;
    accepted = processor.set_parameter(id, value);
    allocations = guard.count();
  }
  REQUIRE(accepted);
  REQUIRE(allocations == 0);
}

TEST_CASE("GS-bound insert controls publish realtime descriptors", "[fx][gs][automation]") {
  DattorroReverb reverb;
  require_descriptor(reverb, "preDelayMs", 9);
  require_descriptor(reverb, "character", 10);

  PitchShifter shifter;
  require_descriptor(shifter, "windowMs", 12);
  require_descriptor(shifter, "preDelayMs", 13);
  require_descriptor(shifter, "preDelay2Ms", 14);

  Rotary rotary;
  require_descriptor(rotary, "stereoSpread", 14);
  require_descriptor(rotary, "accelTauS", 15);
  require_descriptor(rotary, "decelTauS", 16);
  require_descriptor(rotary, "undershootHz", 17);
  require_descriptor(rotary, "drumUndershootHz", 18);

  BitCrusher crusher;
  require_descriptor(crusher, "typeLadder", 19);

  struct FactoryCase {
    const char* name;
    const char* params;
    std::array<const char*, 5> keys;
    std::size_t key_count;
  };
  const FactoryCase cases[] = {
      {"effects.reverb.dattorro",
       R"({"preDelayMs":17,"character":4})",
       {"preDelayMs", "character", nullptr, nullptr, nullptr},
       2},
      {"effects.modulation.pitchShifter",
       R"({"windowMs":64,"preDelayMs":17,"preDelay2Ms":29})",
       {"windowMs", "preDelayMs", "preDelay2Ms", nullptr, nullptr},
       3},
      {"effects.modulation.rotary",
       R"({"stereoSpread":0.25,"accelTauS":0.003,"decelTauS":0.004,"undershootHz":2,"drumUndershootHz":3})",
       {"stereoSpread", "accelTauS", "decelTauS", "undershootHz", "drumUndershootHz"},
       5},
      {"saturation.bitcrusher",
       R"({"typeLadder":4})",
       {"typeLadder", nullptr, nullptr, nullptr, nullptr},
       1},
  };

  for (const auto& test : cases) {
    auto processor = make_insert(test.name, test.params);
    REQUIRE(processor != nullptr);
    const std::string metadata = insert_param_info_json(test.name);
    for (std::size_t i = 0; i < test.key_count; ++i) {
      unsigned int id = 0;
      const bool found = descriptor_for(*processor, test.keys[i], &id);
      INFO(test.name << ":" << test.keys[i]);
      REQUIRE(found);
      REQUIRE(processor->parameter_is_realtime_safe(id));
      float value = 0.0f;
      REQUIRE(processor->constructed_parameter_value(id, &value));
      REQUIRE(std::isfinite(value));
      REQUIRE(metadata.find(test.keys[i]) != std::string::npos);
    }
  }
}

TEST_CASE("GS-bound setters preserve construction rendering", "[fx][gs][automation]") {
  const std::vector<float> impulse = sonare::test::generate_impulse(16384);

  SECTION("Dattorro pre-delay and character") {
    DattorroReverbConfig final_config;
    final_config.dry_wet = 1.0f;
    final_config.mod_depth_samples = 0.0f;
    final_config.pre_delay_samples =
        37.0f * static_cast<float>(DattorroReverb::kReferenceSampleRate) / 1000.0f;
    final_config.character = 4;
    DattorroReverb constructed(final_config);
    DattorroReverbConfig baseline = final_config;
    baseline.pre_delay_samples = 0.0f;
    baseline.character = 0;
    DattorroReverb automated(baseline);
    automated.prepare(kRate, static_cast<int>(impulse.size()));
    require_rt_setter_without_allocation(automated, 9, 37.0f);
    require_rt_setter_without_allocation(automated, 10, 4.0f);
    const auto expected = render_stereo(constructed, impulse, impulse);
    const auto actual = process_stereo(automated, impulse, impulse);
    REQUIRE(sonare::test::max_abs_difference(expected, actual) < 1.0e-6f);
    REQUIRE_FALSE(automated.set_parameter(10, 1.5f));
    REQUIRE_FALSE(automated.set_parameter(10, 7.0f));
    REQUIRE(automated.set_parameter(9, -1.0f));
    REQUIRE_FALSE(automated.set_parameter(9, std::numeric_limits<float>::quiet_NaN()));
  }

  SECTION("Pitch shifter window and pre-delays") {
    PitchShifterConfig final_config;
    final_config.semitones = 7.0f;
    final_config.window_ms = 64.0f;
    final_config.pre_delay_ms = 17.0f;
    final_config.pre_delay2_ms = 29.0f;
    PitchShifter constructed(final_config);
    PitchShifterConfig baseline = final_config;
    baseline.window_ms = PitchShifterConfig{}.window_ms;
    baseline.pre_delay_ms = 0.0f;
    baseline.pre_delay2_ms = 0.0f;
    PitchShifter automated(baseline);
    automated.prepare(kRate, static_cast<int>(impulse.size()));
    require_rt_setter_without_allocation(automated, 12, 64.0f);
    require_rt_setter_without_allocation(automated, 13, 17.0f);
    require_rt_setter_without_allocation(automated, 14, 29.0f);
    const auto expected = render_stereo(constructed, impulse, impulse);
    const auto actual = process_stereo(automated, impulse, impulse);
    REQUIRE(sonare::test::max_abs_difference(expected, actual) < 1.0e-6f);
    REQUIRE(automated.set_parameter(12, 0.0f));
    REQUIRE(automated.set_parameter(13, -1.0f));
    REQUIRE_FALSE(automated.set_parameter(14, std::numeric_limits<float>::infinity()));
  }

  SECTION("Rotary stereo geometry and glide controls") {
    RotaryConfig final_config;
    final_config.stereo_spread = 0.25f;
    final_config.accel_tau_s = 0.003f;
    final_config.decel_tau_s = 0.004f;
    final_config.undershoot_hz = 2.0f;
    final_config.drum_undershoot_hz = 3.0f;
    Rotary constructed(final_config);
    Rotary automated;
    automated.prepare(kRate, static_cast<int>(impulse.size()));
    require_rt_setter_without_allocation(automated, 14, 0.25f);
    require_rt_setter_without_allocation(automated, 15, 0.003f);
    require_rt_setter_without_allocation(automated, 16, 0.004f);
    require_rt_setter_without_allocation(automated, 17, 2.0f);
    require_rt_setter_without_allocation(automated, 18, 3.0f);
    const auto expected = render_stereo(constructed, impulse, impulse);
    const auto actual = process_stereo(automated, impulse, impulse);
    REQUIRE(sonare::test::max_abs_difference(expected, actual) < 1.0e-5f);
    REQUIRE(automated.set_parameter(14, -1.0f));
    REQUIRE(automated.set_parameter(17, -1.0f));
    REQUIRE_FALSE(automated.set_parameter(18, std::numeric_limits<float>::quiet_NaN()));
  }

  SECTION("BitCrusher type ladder") {
    BitCrusherConfig final_config;
    final_config.bit_depth = 8;
    final_config.type_ladder = 4;
    BitCrusher constructed(final_config);
    BitCrusherConfig baseline = final_config;
    baseline.type_ladder = 0;
    BitCrusher automated(baseline);
    automated.prepare(kRate, static_cast<int>(impulse.size()));
    require_rt_setter_without_allocation(automated, 19, 4.0f);
    const auto expected = render_mono(constructed, impulse);
    const auto actual = [&] {
      std::vector<float> samples = impulse;
      float* channels[] = {samples.data()};
      automated.process(channels, 1, static_cast<int>(samples.size()));
      return samples;
    }();
    REQUIRE(sonare::test::max_abs_difference(expected, actual) < 1.0e-6f);
    REQUIRE_FALSE(automated.set_parameter(19, 1.5f));
    REQUIRE_FALSE(automated.set_parameter(19, 10.0f));
  }
}

TEST_CASE("pitch pre-delay growth keeps the prepared ring history", "[fx][gs][automation]") {
  constexpr int kFirstLength = 3000;
  constexpr int kSecondLength = 1500;
  constexpr int kShortDelay = 48;  // 1 ms at 48 kHz.
  constexpr int kLongDelay = 960;  // 20 ms at 48 kHz.

  PitchShifterConfig config;
  config.dry_wet = 1.0f;
  config.pre_delay_ms = 1.0f;
  PitchShifter shifter(config);
  shifter.prepare(kRate, kSecondLength);

  const std::vector<float> all = input_train(kFirstLength + kSecondLength);
  std::vector<float> first = input_train(kFirstLength);
  float* first_channels[] = {first.data()};
  shifter.process(first_channels, 1, kFirstLength);
  REQUIRE(shifter.set_parameter(13, 20.0f));

  std::vector<float> second = input_train(kSecondLength, kFirstLength);
  float* second_channels[] = {second.data()};
  shifter.process(second_channels, 1, kSecondLength);
  for (int i = 0; i < kSecondLength; ++i) {
    const int source = kFirstLength + i - kLongDelay;
    const float expected = source >= 0 ? all[static_cast<std::size_t>(source)] : 0.0f;
    INFO("sample " << i << " (short delay was " << kShortDelay << ")");
    REQUIRE(second[static_cast<std::size_t>(i)] == Catch::Approx(expected).margin(1.0e-6f));
  }
}

TEST_CASE("reverb pre-delay growth keeps a continuous input history", "[fx][gs][automation]") {
  constexpr int kFirstLength = 3000;
  constexpr int kSecondLength = 1500;
  constexpr int kShortDelay = 48;  // 1 ms at 48 kHz.
  constexpr int kLongDelay = 960;  // 20 ms at 48 kHz.

  DattorroReverbConfig config;
  config.decay = 0.4f;
  config.dry_wet = 1.0f;
  config.mod_depth_samples = 0.0f;
  config.pre_delay_samples =
      1.0f * static_cast<float>(DattorroReverb::kReferenceSampleRate) / 1000.0f;
  DattorroReverb actual(config);
  DattorroReverbConfig reference_config = config;
  reference_config.pre_delay_samples = 0.0f;
  DattorroReverb reference(reference_config);
  actual.prepare(kRate, 256);
  reference.prepare(kRate, 256);

  std::vector<float> delay_ring(2048, 0.0f);
  std::size_t delay_index = 0;
  auto external_delay = [&](const std::vector<float>& input, int delay) {
    std::vector<float> delayed(input.size(), 0.0f);
    for (std::size_t i = 0; i < input.size(); ++i) {
      const std::size_t read =
          (delay_index + delay_ring.size() - static_cast<std::size_t>(delay)) % delay_ring.size();
      delayed[i] = delay > 0 ? delay_ring[read] : input[i];
      delay_ring[delay_index] = input[i];
      delay_index = (delay_index + 1) % delay_ring.size();
    }
    return delayed;
  };
  auto process_pair = [&](std::vector<float> input, int delay) {
    std::vector<float> delayed = external_delay(input, delay);
    float* actual_channels[] = {input.data()};
    float* reference_channels[] = {delayed.data()};
    actual.process(actual_channels, 1, static_cast<int>(input.size()));
    reference.process(reference_channels, 1, static_cast<int>(delayed.size()));
    return std::pair<std::vector<float>, std::vector<float>>{std::move(input), std::move(delayed)};
  };

  const auto first = process_pair(input_train(kFirstLength), kShortDelay);
  REQUIRE(sonare::test::max_abs_difference(first.first, first.second) < 1.0e-6f);
  REQUIRE(actual.set_parameter(9, 20.0f));
  const auto second = process_pair(input_train(kSecondLength, kFirstLength), kLongDelay);
  REQUIRE(sonare::test::max_abs_difference(second.first, second.second) < 1.0e-6f);
}

TEST_CASE("reverb character changes preserve nonzero tank history", "[fx][gs][automation]") {
  DattorroReverbConfig config;
  config.decay = 0.7f;
  config.dry_wet = 1.0f;
  config.mod_depth_samples = 0.0f;
  DattorroReverb control(config);
  DattorroReverb toggled(config);
  control.prepare(kRate, 256);
  toggled.prepare(kRate, 256);

  std::vector<float> control_history = input_train(12000);
  std::vector<float> toggled_history = control_history;
  float* control_history_channels[] = {control_history.data()};
  control.process(control_history_channels, 1, static_cast<int>(control_history.size()));
  float* toggled_history_channels[] = {toggled_history.data()};
  toggled.process(toggled_history_channels, 1, static_cast<int>(toggled_history.size()));
  REQUIRE(toggled.set_parameter(10, 1.0f));
  REQUIRE(toggled.set_parameter(10, 0.0f));

  std::vector<float> next = input_train(2048, 12000);
  std::vector<float> control_next = next;
  float* control_channels[] = {control_next.data()};
  control.process(control_channels, 1, static_cast<int>(control_next.size()));
  float* toggled_channels[] = {next.data()};
  toggled.process(toggled_channels, 1, static_cast<int>(next.size()));
  REQUIRE(sonare::test::max_abs_difference(control_next, next) < 1.0e-6f);
}

TEST_CASE("reverb preserves a direct pre-delay above the realtime ladder", "[fx][gs][automation]") {
  constexpr double kPreDelayMs = 1200.0;
  constexpr float kOnsetThreshold = 1.0e-6f;
  constexpr int kRoundingSamples = 8;
  constexpr double kOnsetWindowMs = 10.0;

  DattorroReverbConfig config;
  config.dry_wet = 1.0f;
  config.pre_delay_samples =
      static_cast<float>(kPreDelayMs * DattorroReverb::kReferenceSampleRate / 1000.0);
  DattorroReverb reverb(config);

  const int length = static_cast<int>(kRate * (kPreDelayMs + 50.0) / 1000.0);
  const std::vector<float> impulse = sonare::test::generate_impulse(length);
  const std::vector<float> rendered = render_stereo(reverb, impulse, impulse);

  std::size_t onset = rendered.size();
  for (std::size_t i = 0; i < static_cast<std::size_t>(length); ++i) {
    if (std::abs(rendered[i]) > kOnsetThreshold) {
      onset = i;
      break;
    }
  }
  const double expected = kRate * kPreDelayMs / 1000.0;
  REQUIRE(onset < static_cast<std::size_t>(length));
  CHECK(static_cast<double>(onset) >= expected - kRoundingSamples);
  CHECK(static_cast<double>(onset) <= expected + kRate * kOnsetWindowMs / 1000.0);
}

}  // namespace

#endif  // SONARE_WITH_FX
