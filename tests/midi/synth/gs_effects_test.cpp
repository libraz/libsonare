#include "midi/synth/gs_effects.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using sonare::midi::synth::gs_effects_config_from;
using sonare::midi::synth::GsEffectBus;
using sonare::midi::synth::GsEffectsConfig;
using sonare::midi::synth::GsSystemEffects;

struct TailMeasurement {
  int64_t declared = 0;
  int last_above_floor = -1;
  float peak_after_declared = 0.0f;
  int nonfinite = 0;
};

TailMeasurement render_reverb(int sample_rate) {
  GsSystemEffects system;
  system.reverb_time = 0;
  GsEffectsConfig config = gs_effects_config_from(system);
  config.enable_chorus = false;
  config.enable_delay = false;
  GsEffectBus bus(config);
  bus.prepare(sample_rate);

  const int64_t declared = bus.tail_samples(sample_rate);
  const int frame_count = static_cast<int>(declared + 4 * sample_rate);
  std::vector<float> left(static_cast<size_t>(frame_count), 0.0f);
  std::vector<float> right(static_cast<size_t>(frame_count), 0.0f);
  TailMeasurement result;
  result.declared = declared;
  for (int offset = 0; offset < frame_count; offset += GsEffectBus::kBlockFrames) {
    const int count = std::min(GsEffectBus::kBlockFrames, frame_count - offset);
    bus.begin_chunk();
    if (offset == 0) {
      bus.reverb_in(0)[0] = 0.5f;
      bus.reverb_in(1)[0] = 0.5f;
    }
    bus.render_returns(left.data() + offset, right.data() + offset, count);
  }
  for (int i = 0; i < frame_count; ++i) {
    for (float value : {left[static_cast<size_t>(i)], right[static_cast<size_t>(i)]}) {
      if (!std::isfinite(value)) ++result.nonfinite;
      if (i >= declared)
        result.peak_after_declared = std::max(result.peak_after_declared, std::fabs(value));
      if (std::fabs(value) > 1.0e-4f) result.last_above_floor = i;
    }
  }
  return result;
}

TailMeasurement render_long_chorus(int sample_rate) {
  GsEffectsConfig config;
  config.enable_reverb = false;
  config.enable_delay = false;
  config.chorus_level = 1.0f;
  config.chorus_rate_hz = 0.0f;
  config.chorus_delay_ms = 100.0f;
  config.chorus_depth_ms = 30.0f;
  GsEffectBus bus(config);
  bus.prepare(sample_rate);

  const int64_t declared = bus.tail_samples(sample_rate);
  const int frame_count = static_cast<int>(declared + 4 * sample_rate);
  std::vector<float> left(static_cast<size_t>(frame_count), 0.0f);
  std::vector<float> right(static_cast<size_t>(frame_count), 0.0f);
  TailMeasurement result;
  result.declared = declared;
  for (int offset = 0; offset < frame_count; offset += GsEffectBus::kBlockFrames) {
    const int count = std::min(GsEffectBus::kBlockFrames, frame_count - offset);
    bus.begin_chunk();
    if (offset == 0) {
      bus.chorus_in(0)[0] = 0.5f;
      bus.chorus_in(1)[0] = 0.5f;
    }
    bus.render_returns(left.data() + offset, right.data() + offset, count);
  }
  for (int i = 0; i < frame_count; ++i) {
    for (float value : {left[static_cast<size_t>(i)], right[static_cast<size_t>(i)]}) {
      if (!std::isfinite(value)) ++result.nonfinite;
      if (i >= declared)
        result.peak_after_declared = std::max(result.peak_after_declared, std::fabs(value));
      if (std::fabs(value) > 1.0e-5f) result.last_above_floor = i;
    }
  }
  return result;
}

TailMeasurement render_reverb_live_decay_switch(int sample_rate) {
  GsEffectsConfig high;
  high.enable_chorus = false;
  high.enable_delay = false;
  high.reverb_character = 4;
  high.reverb_decay = 0.9f;
  GsEffectBus bus(high);
  bus.prepare(sample_rate);

  const int high_declared = bus.tail_samples(sample_rate);
  std::vector<float> left(static_cast<size_t>(high_declared + sample_rate), 0.0f);
  std::vector<float> right(left.size(), 0.0f);
  for (int offset = 0; offset < 4 * GsEffectBus::kBlockFrames;
       offset += GsEffectBus::kBlockFrames) {
    bus.begin_chunk();
    if (offset == 0) {
      bus.reverb_in(0)[0] = 0.5f;
      bus.reverb_in(1)[0] = 0.5f;
    }
    bus.render_returns(left.data() + offset, right.data() + offset, GsEffectBus::kBlockFrames);
  }

  GsEffectsConfig low = high;
  low.reverb_decay = 0.0f;
  bus.set_config(low);
  TailMeasurement result;
  result.declared = bus.tail_samples(sample_rate);
  const int frame_count =
      static_cast<int>(std::min<int64_t>(left.size(), result.declared + sample_rate));
  for (int offset = 4 * GsEffectBus::kBlockFrames; offset < frame_count;
       offset += GsEffectBus::kBlockFrames) {
    const int count = std::min(GsEffectBus::kBlockFrames, frame_count - offset);
    bus.begin_chunk();
    bus.render_returns(left.data() + offset, right.data() + offset, count);
  }
  for (int i = 0; i < frame_count; ++i) {
    for (float value : {left[static_cast<size_t>(i)], right[static_cast<size_t>(i)]}) {
      if (!std::isfinite(value)) ++result.nonfinite;
      if (i >= result.declared)
        result.peak_after_declared = std::max(result.peak_after_declared, std::fabs(value));
      if (std::fabs(value) > 1.0e-5f) result.last_above_floor = i;
    }
  }
  return result;
}

}  // namespace

TEST_CASE("GS effect bus tail covers the prepared Dattorro geometry", "[boundary]") {
  for (const int sample_rate : {48000, 96000}) {
    const TailMeasurement result = render_reverb(sample_rate);
    INFO("sample rate " << sample_rate << ", declared " << result.declared << ", last "
                        << result.last_above_floor << ", post-bound peak "
                        << result.peak_after_declared);
    CHECK(result.nonfinite == 0);
    CHECK(result.last_above_floor <= result.declared);
  }
}

TEST_CASE("GS effect bus converts child tails to the requested sample rate", "[boundary]") {
  GsEffectsConfig config;
  config.enable_chorus = false;
  config.enable_delay = false;
  GsEffectBus bus(config);
  bus.prepare(48000.0);

  const int64_t prepared_tail = bus.tail_samples(48000.0);
  REQUIRE(prepared_tail > 0);
  const auto expected_at = [prepared_tail](double sample_rate) {
    return static_cast<int64_t>(
        std::ceil((static_cast<double>(prepared_tail) / 48000.0) * sample_rate));
  };
  CHECK(bus.tail_samples(96000.0) == expected_at(96000.0));
  CHECK(bus.tail_samples(53777.0) == expected_at(53777.0));
}

TEST_CASE("GS effect bus tail covers custom long chorus taps", "[boundary]") {
  for (const int sample_rate : {48000, 96000}) {
    const TailMeasurement result = render_long_chorus(sample_rate);
    INFO("sample rate " << sample_rate << ", declared " << result.declared << ", last "
                        << result.last_above_floor << ", post-bound peak "
                        << result.peak_after_declared);
    CHECK(result.nonfinite == 0);
    CHECK(result.last_above_floor <= result.declared);
  }
}

TEST_CASE("GS effect bus retains a child tail across a live decay switch", "[boundary]") {
  for (const int sample_rate : {48000, 96000}) {
    const TailMeasurement result = render_reverb_live_decay_switch(sample_rate);
    INFO("sample rate " << sample_rate << ", declared " << result.declared << ", last "
                        << result.last_above_floor << ", post-bound peak "
                        << result.peak_after_declared);
    CHECK(result.nonfinite == 0);
    CHECK(result.last_above_floor <= result.declared);
  }
}
