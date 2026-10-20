/// @file pitch_shifter_non_finite_test.cpp
/// @brief The pitch shifter reports the non-finite state it returns to rest, as its sibling inserts
/// do.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include "effects/modulation/chorus.h"
#include "effects/modulation/pitch_shifter.h"
#include "util/constants.h"

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 256;
constexpr int kBlocks = 24;
constexpr int kPoisonBlock = 1;
constexpr int kPoisonIndex = 100;

/// Streams a sine through @p processor, putting a NaN in one sample of one block when @p poison.
template <typename Processor>
void run(Processor& processor, bool poison) {
  std::vector<float> left(kBlockSize);
  std::vector<float> right(kBlockSize);
  for (int b = 0; b < kBlocks; ++b) {
    for (int i = 0; i < kBlockSize; ++i) {
      const double t = static_cast<double>(b * kBlockSize + i) / kSampleRate;
      left[i] = right[i] =
          0.5f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * 440.0 * t));
    }
    if (poison && b == kPoisonBlock) {
      left[kPoisonIndex] = right[kPoisonIndex] = std::numeric_limits<float>::quiet_NaN();
    }
    float* channels[] = {left.data(), right.data()};
    processor.process(channels, 2, kBlockSize);
  }
}

template <typename Make>
void check_discard_is_counted(const Make& make) {
  auto control = make();
  run(*control, false);
  CHECK(control->non_finite_discard_count() == 0);

  auto poisoned = make();
  run(*poisoned, true);
  CHECK(poisoned->non_finite_discard_count() >= 1);
}

}  // namespace

TEST_CASE("the chorus counts the state it discarded (control)", "[effects][non_finite]") {
  using namespace sonare::effects::modulation;
  ChorusConfig config;
  config.dry_wet = 0.5f;
  config.pre_filter_mode = PreFilterMode::kLowPass;
  config.pre_filter_hz = 2000.0f;
  check_discard_is_counted([config] {
    auto processor = std::make_shared<Chorus>(config);
    processor->prepare(kSampleRate, kBlockSize);
    return processor;
  });
}

TEST_CASE("the pitch shifter counts a feedback path it returned to rest", "[effects][non_finite]") {
  using namespace sonare::effects::modulation;
  PitchShifterConfig config;
  config.semitones = 7.0f;
  config.feedback = 0.5f;
  config.dry_wet = 1.0f;
  check_discard_is_counted([config] {
    auto processor = std::make_shared<PitchShifter>(config);
    processor->prepare(kSampleRate, kBlockSize);
    return processor;
  });
}

TEST_CASE("the pitch shifter counts anti-alias sections it returned to rest",
          "[effects][non_finite]") {
  using namespace sonare::effects::modulation;
  PitchShifterConfig config;
  config.semitones = 7.0f;
  config.anti_alias = true;
  config.dry_wet = 1.0f;
  check_discard_is_counted([config] {
    auto processor = std::make_shared<PitchShifter>(config);
    processor->prepare(kSampleRate, kBlockSize);
    return processor;
  });
}
