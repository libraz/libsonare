/// @file mod_delay_interp_test.cpp
/// @brief Third-order Lagrange delay reads on the six modulated-delay inserts, and the pitch
///        shifter's write-side anti-alias low-pass.
///
/// The droop is read at 10 kHz through a delay held at half a sample past a whole one, where a
/// two-point read loses the most; the same configuration is rendered with `interpolation` 0 and 1
/// and the two gains compared.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "effects/delay/stereo_delay.h"
#include "effects/modulation/chorus.h"
#include "effects/modulation/ensemble.h"
#include "effects/modulation/flanger.h"
#include "effects/modulation/mod_delay_line.h"
#include "effects/modulation/pitch_shifter.h"
#include "effects/modulation/rotary.h"
#include "rt/processor_base.h"
#include "support/audio_fixtures.h"

namespace {

using sonare::effects::delay::StereoDelay;
using sonare::effects::delay::StereoDelayConfig;
using sonare::effects::modulation::Chorus;
using sonare::effects::modulation::ChorusConfig;
using sonare::effects::modulation::DelayInterpolation;
using sonare::effects::modulation::Ensemble;
using sonare::effects::modulation::EnsembleConfig;
using sonare::effects::modulation::Flanger;
using sonare::effects::modulation::FlangerConfig;
using sonare::effects::modulation::ModDelayLine;
using sonare::effects::modulation::PitchShifter;
using sonare::effects::modulation::PitchShifterConfig;
using sonare::effects::modulation::Rotary;
using sonare::effects::modulation::RotaryConfig;
using sonare::test::kRate;

constexpr int kSamples = 9600;  // 0.2 s at 48 kHz.
constexpr std::size_t kSkip = 4800;
constexpr float kDroopToneHz = 10000.0f;
constexpr float kHalfSampleDelay = 100.5f;
// Linear at a half-sample fraction passes 0.79 of a 10 kHz tone and Lagrange about 0.98, so the
// ratio of the two gains sits near 1.23; the bound leaves room for the insert's own filtering.
constexpr float kMinGainRatio = 1.15f;

constexpr float delay_ms(float samples) { return samples / static_cast<float>(kRate) * 1000.0f; }

/// Gain of the 10 kHz tone through @p processor, read as output RMS over input RMS.
float tone_gain(sonare::rt::ProcessorBase& processor) {
  processor.prepare(kRate, kSamples);
  std::vector<float> left =
      sonare::test::generate_sine(kSamples, kDroopToneHz, static_cast<int>(kRate), 0.5f);
  std::vector<float> right = left;
  const float input_rms = sonare::test::rms(left, kSkip);
  sonare::test::process_stereo(processor, left, right);
  return sonare::test::rms(left, kSkip) / input_rms;
}

/// Gain of the same rendering through both reads, [linear, lagrange3].
template <typename Processor, typename Config>
std::array<float, 2> gains(Config config) {
  std::array<float, 2> out{};
  for (int i = 0; i < 2; ++i) {
    config.interpolation = i == 0 ? DelayInterpolation::kLinear : DelayInterpolation::kLagrange3;
    Processor processor(config);
    out[static_cast<std::size_t>(i)] = tone_gain(processor);
  }
  return out;
}

void require_flatter(const char* name, const std::array<float, 2>& g) {
  INFO(name << ": linear " << g[0] << ", lagrange3 " << g[1]);
  WARN(name << " 10 kHz gain: linear " << 20.0 * std::log10(g[0]) << " dB, lagrange3 "
            << 20.0 * std::log10(g[1]) << " dB");
  REQUIRE(g[0] > 0.0f);
  REQUIRE(g[1] > g[0]);
  REQUIRE(g[1] / g[0] >= kMinGainRatio);
}

/// Index of the first sample whose magnitude exceeds @p threshold, or -1.
int first_arrival(const std::vector<float>& x, float threshold = 0.25f) {
  for (std::size_t i = 0; i < x.size(); ++i) {
    if (std::fabs(x[i]) > threshold) return static_cast<int>(i);
  }
  return -1;
}

/// Realtime id the processor publishes for @p name, or -1.
int parameter_id(const sonare::rt::ProcessorBase& processor, const std::string& name) {
  for (const auto& descriptor : processor.parameter_descriptors()) {
    if (name == descriptor.key) return static_cast<int>(descriptor.id);
  }
  return -1;
}

/// A whole-number selector accepts 0 and 1 and refuses everything else.
void require_selector(sonare::rt::ProcessorBase& processor, const std::string& name) {
  const int id = parameter_id(processor, name);
  REQUIRE(id >= 0);
  REQUIRE(processor.parameter_is_realtime_safe(static_cast<unsigned int>(id)));
  const unsigned int uid = static_cast<unsigned int>(id);
  CHECK(processor.set_parameter(uid, 1.0f));
  CHECK(processor.set_parameter(uid, 0.0f));
  CHECK_FALSE(processor.set_parameter(uid, 0.5f));
  CHECK_FALSE(processor.set_parameter(uid, 2.0f));
  CHECK_FALSE(processor.set_parameter(uid, -1.0f));
  CHECK_FALSE(processor.set_parameter(uid, std::nanf("")));
  CHECK_FALSE(processor.set_parameter(uid, INFINITY));
}

}  // namespace

TEST_CASE("Lagrange3 keeps more of a 10 kHz tone than a linear read at a half-sample delay",
          "[mod-interp]") {
  SECTION("chorus") {
    ChorusConfig config;
    config.rate_hz = 0.0f;
    config.depth_ms = 0.0f;
    config.center_delay_ms = delay_ms(kHalfSampleDelay);
    config.dry_wet = 1.0f;
    require_flatter("chorus", gains<Chorus>(config));
  }

  SECTION("flanger") {
    FlangerConfig config;
    config.rate_hz = 0.0f;
    config.depth_ms = 0.0f;
    config.center_delay_ms = delay_ms(kHalfSampleDelay);
    config.feedback = 0.0f;
    config.dry_wet = 1.0f;
    require_flatter("flanger", gains<Flanger>(config));
  }

  SECTION("ensemble") {
    EnsembleConfig config;
    config.depth_slow_ms = 0.0f;
    config.depth_fast_ms = 0.0f;
    config.center_delay_ms = delay_ms(kHalfSampleDelay);
    config.dry_wet = 1.0f;
    require_flatter("ensemble", gains<Ensemble>(config));
  }

  SECTION("rotary") {
    RotaryConfig config;
    config.rate_hz = 0.0f;
    config.drum_rate_hz = 0.0f;
    config.stereo_spread = 0.0f;
    config.tremolo = 0.0f;
    config.depth_ms = delay_ms(kHalfSampleDelay);
    config.dry_wet = 1.0f;
    require_flatter("rotary", gains<Rotary>(config));
  }

  SECTION("stereo delay") {
    StereoDelayConfig config;
    config.delay_time_l_ms = delay_ms(kHalfSampleDelay);
    config.delay_time_r_ms = delay_ms(kHalfSampleDelay);
    config.feedback = 0.0f;
    config.dry_wet = 1.0f;
    require_flatter("stereo delay", gains<StereoDelay>(config));
  }

  SECTION("pitch shifter") {
    PitchShifterConfig config;
    config.pre_delay_ms = delay_ms(kHalfSampleDelay);
    config.dry_wet = 1.0f;
    require_flatter("pitch shifter", gains<PitchShifter>(config));
  }
}

TEST_CASE("a Lagrange3 read never reaches closer than one sample to the write head",
          "[mod-interp]") {
  SECTION("the line returns the input one sample late for a requested delay of zero") {
    ModDelayLine line;
    line.prepare(64);
    line.set_interpolation(DelayInterpolation::kLagrange3);
    CHECK(line.process(1.0f, 0.0f) == 0.0f);
    CHECK(line.process(0.0f, 0.0f) == 1.0f);
    CHECK(line.process(0.0f, 0.0f) == 0.0f);

    // Linear still answers at once, so the floor is the new read's and not the line's.
    ModDelayLine linear;
    linear.prepare(64);
    CHECK(linear.process(1.0f, 0.0f) == 1.0f);
  }

  SECTION("a whole delay is reproduced exactly") {
    ModDelayLine line;
    line.prepare(64);
    line.set_interpolation(DelayInterpolation::kLagrange3);
    std::vector<float> out;
    for (int i = 0; i < 20; ++i) out.push_back(line.process(i == 0 ? 1.0f : 0.0f, 7.0f));
    CHECK(first_arrival(out) == 7);
    CHECK(out[7] == 1.0f);
  }

  SECTION("the flanger's wet path lands one sample late, not on the input's sample") {
    FlangerConfig config;
    config.rate_hz = 0.0f;
    config.depth_ms = 0.0f;
    config.center_delay_ms = 0.0f;
    config.feedback = 0.0f;
    config.dry_wet = 1.0f;
    int arrival[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
      config.interpolation = i == 0 ? DelayInterpolation::kLinear : DelayInterpolation::kLagrange3;
      Flanger flanger(config);
      flanger.prepare(kRate, 256);
      std::vector<float> x = sonare::test::generate_impulse(256);
      sonare::test::process(flanger, x);
      arrival[i] = first_arrival(x);
    }
    CHECK(arrival[0] == 0);
    CHECK(arrival[1] == 1);
  }

  SECTION("the stereo delay's loop keeps a sample of delay at a time of zero") {
    StereoDelayConfig config;
    config.delay_time_l_ms = 0.0f;
    config.delay_time_r_ms = 0.0f;
    config.feedback = 0.5f;
    config.dry_wet = 1.0f;
    int arrival[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
      config.interpolation = i == 0 ? DelayInterpolation::kLinear : DelayInterpolation::kLagrange3;
      StereoDelay delay(config);
      delay.prepare(kRate, 256);
      std::vector<float> x = sonare::test::generate_impulse(256);
      sonare::test::process(delay, x);
      arrival[i] = first_arrival(x);
      for (const float v : x) REQUIRE(std::isfinite(v));
    }
    CHECK(arrival[0] == 0);
    CHECK(arrival[1] == 1);
  }
}

TEST_CASE("interpolation is a realtime selector on all six inserts", "[mod-interp]") {
  Chorus chorus;
  Flanger flanger;
  Ensemble ensemble;
  Rotary rotary;
  StereoDelay delay;
  PitchShifter shifter;
  require_selector(chorus, "interpolation");
  require_selector(flanger, "interpolation");
  require_selector(ensemble, "interpolation");
  require_selector(rotary, "interpolation");
  require_selector(delay, "interpolation");
  require_selector(shifter, "interpolation");
  require_selector(shifter, "antiAlias");
  CHECK(parameter_id(chorus, "interpolation") == 8);
  CHECK(parameter_id(flanger, "interpolation") == 9);
  CHECK(parameter_id(ensemble, "interpolation") == 11);
  CHECK(parameter_id(rotary, "interpolation") == 12);
  CHECK(parameter_id(delay, "interpolation") == 22);
  CHECK(parameter_id(shifter, "interpolation") == 10);
  CHECK(parameter_id(shifter, "antiAlias") == 11);
}

TEST_CASE("the pitch shifter's antiAlias removes what +12 semitones would fold back",
          "[mod-interp]") {
  constexpr float kFoldingToneHz = 20000.0f;  // Shifted to 40 kHz, folded to 8 kHz.
  constexpr float kPassbandToneHz = 3000.0f;
  constexpr int kLength = 24000;
  constexpr std::size_t kMeasureSkip = 12000;

  auto shifted_rms = [&](float tone_hz, float semitones, bool anti_alias) {
    PitchShifterConfig config;
    config.semitones = semitones;
    config.anti_alias = anti_alias;
    PitchShifter shifter(config);
    shifter.prepare(kRate, kLength);
    std::vector<float> left =
        sonare::test::generate_sine(kLength, tone_hz, static_cast<int>(kRate), 0.5f);
    std::vector<float> right = left;
    sonare::test::process_stereo(shifter, left, right);
    return sonare::test::rms(left, kMeasureSkip);
  };

  const float folded_off = shifted_rms(kFoldingToneHz, 12.0f, false);
  const float folded_on = shifted_rms(kFoldingToneHz, 12.0f, true);
  const float reduction_db = 20.0f * std::log10(folded_off / folded_on);
  WARN("alias reduction at +12 semitones: " << reduction_db << " dB");
  REQUIRE(folded_off > 0.0f);
  CHECK(reduction_db >= 12.0f);

  // Below the corner the section leaves the tone alone.
  const float passband_off = shifted_rms(kPassbandToneHz, 12.0f, false);
  const float passband_on = shifted_rms(kPassbandToneHz, 12.0f, true);
  CHECK(std::fabs(20.0f * std::log10(passband_on / passband_off)) < 0.5f);

  // A ratio of 1 or less has nothing to fold, so the section is out of the path.
  PitchShifterConfig down;
  down.semitones = -5.0f;
  std::vector<float> off =
      sonare::test::generate_sine(kLength, kPassbandToneHz, static_cast<int>(kRate), 0.5f);
  std::vector<float> on = off;
  std::vector<float> off_right = off;
  std::vector<float> on_right = off;
  PitchShifter plain(down);
  down.anti_alias = true;
  PitchShifter filtered(down);
  plain.prepare(kRate, kLength);
  filtered.prepare(kRate, kLength);
  sonare::test::process_stereo(plain, off, off_right);
  sonare::test::process_stereo(filtered, on, on_right);
  CHECK(off == on);
}
