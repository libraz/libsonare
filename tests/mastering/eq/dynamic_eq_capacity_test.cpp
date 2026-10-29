#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

#include "mastering/dynamics/channel_limits.h"
#include "mastering/eq/dynamic_eq.h"
#include "util/constants.h"
#include "util/exception.h"

namespace {

using sonare::ErrorCode;
using sonare::SonareException;
using sonare::mastering::dynamics::kRealtimePreparedChannels;
using sonare::mastering::eq::DynamicEq;
using sonare::mastering::eq::DynamicEqBand;
using sonare::mastering::eq::EqBandType;

constexpr int kSampleRate = 48000;
constexpr int kBlockSize = 256;
constexpr float kSmallSignal = 0.03f;

std::vector<float> sine_block(size_t offset, float amplitude, int count = kBlockSize) {
  std::vector<float> samples(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    const double sample = static_cast<double>(offset + static_cast<size_t>(i));
    samples[static_cast<size_t>(i)] =
        amplitude *
        static_cast<float>(std::sin(sonare::constants::kTwoPiD * 1000.0 * sample / kSampleRate));
  }
  return samples;
}

void configure(DynamicEq& eq) {
  eq.prepare(kSampleRate, kBlockSize);
  DynamicEqBand band{EqBandType::Peak, 1000.0f, 0.0f, 2.0f, -40.0f, 4.0f, -12.0f, true};
  band.attack_ms = 0.0f;
  band.release_ms = 1000.0f;
  eq.set_band(0, band);
  eq.reset();
}

void process_with_key(DynamicEq& eq, std::vector<float>& program, const std::vector<float>& key) {
  const float* key_channels[] = {key.data()};
  float* program_channels[] = {program.data()};
  eq.set_sidechain(key_channels, 1, static_cast<int>(key.size()));
  eq.process(program_channels, 1, static_cast<int>(program.size()));
}

float max_abs_difference(const std::vector<float>& first, const std::vector<float>& second) {
  REQUIRE(first.size() == second.size());
  float result = 0.0f;
  for (size_t i = 0; i < first.size(); ++i) {
    result = std::max(result, std::abs(first[i] - second[i]));
  }
  return result;
}

struct ObservableState {
  float detector_db;
  std::array<float, DynamicEq::kMaxBands> band_detector_db;
  std::array<float, DynamicEq::kMaxBands> applied_gain_db;
};

ObservableState state_of(const DynamicEq& eq) {
  ObservableState state{};
  state.detector_db = eq.last_detector_db();
  for (size_t i = 0; i < DynamicEq::kMaxBands; ++i) {
    state.band_detector_db[i] = eq.last_band_detector_db(i);
    state.applied_gain_db[i] = eq.last_applied_gain_db(i);
  }
  return state;
}

void require_same_state(const ObservableState& actual, const ObservableState& expected) {
  REQUIRE(std::isfinite(actual.detector_db));
  CHECK(actual.detector_db == expected.detector_db);
  for (size_t i = 0; i < DynamicEq::kMaxBands; ++i) {
    REQUIRE(std::isfinite(actual.band_detector_db[i]));
    REQUIRE(std::isfinite(actual.applied_gain_db[i]));
    CHECK(actual.band_detector_db[i] == expected.band_detector_db[i]);
    CHECK(actual.applied_gain_db[i] == expected.applied_gain_db[i]);
  }
}

}  // namespace

TEST_CASE("DynamicEq rejects over-capacity blocks before consuming detector state",
          "[dynamic-eq-capacity]") {
  DynamicEq subject;
  DynamicEq control;
  configure(subject);
  configure(control);

  std::vector<float> last_program_input;
  std::vector<float> last_subject_output;
  for (size_t offset = 0; offset < static_cast<size_t>(8 * kBlockSize);
       offset += static_cast<size_t>(kBlockSize)) {
    last_program_input = sine_block(offset, kSmallSignal);
    const std::vector<float> key = sine_block(offset, kSmallSignal);
    last_subject_output = last_program_input;
    auto control_output = last_program_input;
    process_with_key(subject, last_subject_output, key);
    process_with_key(control, control_output, key);
    REQUIRE(std::all_of(last_subject_output.begin(), last_subject_output.end(),
                        [](float sample) { return std::isfinite(sample); }));
    REQUIRE(last_subject_output == control_output);
  }

  REQUIRE(std::isfinite(subject.last_detector_db()));
  REQUIRE(std::isfinite(subject.last_band_detector_db(0)));
  CHECK(subject.last_band_detector_db(0) > -40.0f);
  CHECK(subject.last_applied_gain_db(0) < 0.0f);
  CHECK(max_abs_difference(last_program_input, last_subject_output) > 1.0e-5f);

  const std::vector<float> loud_key = sine_block(8 * kBlockSize, 0.9f);
  const float* loud_key_channels[] = {loud_key.data()};
  subject.set_sidechain(loud_key_channels, 1, kBlockSize);

  const size_t invalid_channel_count = kRealtimePreparedChannels + 1;
  std::vector<std::vector<float>> invalid_audio(invalid_channel_count,
                                                std::vector<float>(kBlockSize, 0.0f));
  for (size_t channel = 0; channel < invalid_audio.size(); ++channel) {
    for (size_t sample = 0; sample < invalid_audio[channel].size(); ++sample) {
      invalid_audio[channel][sample] =
          0.001f * static_cast<float>(channel + 1) + 0.00001f * static_cast<float>(sample);
    }
  }
  const auto original_invalid_audio = invalid_audio;
  std::vector<float*> invalid_channels;
  invalid_channels.reserve(invalid_audio.size());
  for (auto& channel : invalid_audio) invalid_channels.push_back(channel.data());

  const ObservableState before_rejection = state_of(subject);
  bool rejected = false;
  try {
    subject.process(invalid_channels.data(), static_cast<int>(invalid_channels.size()), kBlockSize);
  } catch (const SonareException& error) {
    rejected = true;
    CHECK(error.code() == ErrorCode::InvalidParameter);
  }
  CHECK(rejected);
  CHECK(invalid_audio == original_invalid_audio);
  require_same_state(state_of(subject), before_rejection);

  const size_t next_offset = static_cast<size_t>(8 * kBlockSize);
  auto subject_program = sine_block(next_offset, kSmallSignal);
  auto control_program = subject_program;
  const auto next_key = sine_block(next_offset, kSmallSignal);
  process_with_key(subject, subject_program, next_key);
  process_with_key(control, control_program, next_key);

  REQUIRE(std::all_of(subject_program.begin(), subject_program.end(),
                      [](float sample) { return std::isfinite(sample); }));
  REQUIRE(std::all_of(control_program.begin(), control_program.end(),
                      [](float sample) { return std::isfinite(sample); }));
  CHECK(subject_program == control_program);
  require_same_state(state_of(subject), state_of(control));
}

TEST_CASE("DynamicEq keeps over-capacity empty blocks as no-ops", "[dynamic-eq-capacity]") {
  DynamicEq eq;
  configure(eq);

  std::vector<float> storage(1, 0.25f);
  std::vector<float*> channels(kRealtimePreparedChannels + 1, storage.data());
  const ObservableState before = state_of(eq);
  CHECK_NOTHROW(eq.process(channels.data(), static_cast<int>(channels.size()), 0));
  require_same_state(state_of(eq), before);
}

TEST_CASE("DynamicEq sidechain capacity follows enabled detector state", "[dynamic-eq-capacity]") {
  const size_t wide_count = kRealtimePreparedChannels + 1;
  std::vector<std::vector<float>> key_storage(wide_count, sine_block(0, 0.9f));
  std::vector<const float*> key_channels;
  key_channels.reserve(wide_count);
  for (const auto& channel : key_storage) key_channels.push_back(channel.data());

  SECTION("an active band rejects the wide key before changing state") {
    DynamicEq eq;
    configure(eq);
    std::vector<float> program = sine_block(0, kSmallSignal);
    const auto original_program = program;
    float* program_channels[] = {program.data()};
    eq.set_sidechain(key_channels.data(), static_cast<int>(key_channels.size()), kBlockSize);
    const ObservableState before = state_of(eq);

    bool rejected = false;
    try {
      eq.process(program_channels, 1, kBlockSize);
    } catch (const SonareException& error) {
      rejected = true;
      CHECK(error.code() == ErrorCode::InvalidParameter);
    }
    CHECK(rejected);
    CHECK(program == original_program);
    require_same_state(state_of(eq), before);
  }

  SECTION("disabled bands accept a wide key and leave the program unchanged") {
    DynamicEq subject;
    DynamicEq control;
    subject.prepare(kSampleRate, kBlockSize);
    control.prepare(kSampleRate, kBlockSize);
    std::vector<float> subject_program = sine_block(0, kSmallSignal);
    auto control_program = subject_program;
    float* subject_channels[] = {subject_program.data()};
    float* control_channels[] = {control_program.data()};

    subject.set_sidechain(key_channels.data(), static_cast<int>(key_channels.size()), kBlockSize);
    CHECK_NOTHROW(subject.process(subject_channels, 1, kBlockSize));
    CHECK_NOTHROW(control.process(control_channels, 1, kBlockSize));
    CHECK(subject_program == control_program);
  }
}
