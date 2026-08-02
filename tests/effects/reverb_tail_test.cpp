/// @file reverb_tail_test.cpp
/// @brief Reverb/delay decay-tail reporting so offline bounces are not
///        truncated, plus the convolution empty-IR latency contract.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "effects/delay/stereo_delay.h"
#include "effects/reverb/convolution_reverb.h"
#include "effects/reverb/dattorro_reverb.h"
#include "effects/reverb/fdn_reverb.h"
#include "effects/reverb/velvet_reverb.h"

using sonare::effects::delay::StereoDelay;
using sonare::effects::delay::StereoDelayConfig;
using sonare::effects::reverb::ConvolutionReverb;
using sonare::effects::reverb::ConvolutionReverbConfig;
using sonare::effects::reverb::DattorroReverb;
using sonare::effects::reverb::DattorroReverbConfig;
using sonare::effects::reverb::FdnReverb;
using sonare::effects::reverb::FdnReverbConfig;
using sonare::effects::reverb::VelvetReverb;
using sonare::effects::reverb::VelvetReverbConfig;

TEST_CASE("FdnReverb reports a non-zero decay tail", "[effects][reverb][fdn]") {
  FdnReverbConfig config;
  config.decay = 0.6f;
  FdnReverb reverb(config);
  reverb.prepare(48000.0, 512);
  // decay 0.6 -> T60_lf = 6 s -> ~288000 samples at 48 kHz.
  REQUIRE(reverb.tail_samples() > 48000);
}

TEST_CASE("VelvetReverb reports a non-zero decay tail", "[effects][reverb][velvet]") {
  VelvetReverbConfig config;
  config.reverb_time_s = 1.5f;
  config.decay = 0.45f;
  VelvetReverb reverb(config);
  reverb.prepare(48000.0, 512);
  // rt60 = 1.5 * (0.5 + 0.45) ~= 1.425 s -> ~68400 samples.
  REQUIRE(reverb.tail_samples() > 48000);
}

TEST_CASE("VelvetReverb bounds excessive reverb time and tap work", "[effects][reverb][velvet]") {
  VelvetReverbConfig config;
  config.reverb_time_s = 40.0f;
  config.decay = 1.0f;
  config.density_hz = 3000.0f;
  VelvetReverb reverb(config);
  reverb.prepare(48000.0, 512);

  // 12 s base time × the maximum 1.5 decay factor, rather than the requested 40 s.
  REQUIRE(reverb.tail_samples() <= 18 * 48000);

  std::vector<float> samples(512, 0.0f);
  samples[0] = 1.0f;
  float* channels[] = {samples.data()};
  reverb.process(channels, 1, static_cast<int>(samples.size()));
  for (const float sample : samples) REQUIRE(std::isfinite(sample));
}

TEST_CASE("VelvetReverb hybrid tap reconstruction is independent of host block boundaries",
          "[effects][reverb][velvet]") {
  constexpr int kSamples = 4096;
  VelvetReverbConfig config;
  config.dry_wet = 1.0f;
  config.enable_shelf = false;
  std::vector<float> whole(kSamples, 0.0f);
  std::vector<float> split(kSamples, 0.0f);
  whole[0] = 1.0f;
  split[0] = 1.0f;

  VelvetReverb whole_fx(config);
  whole_fx.prepare(48000.0, kSamples);
  float* whole_channels[] = {whole.data()};
  whole_fx.process(whole_channels, 1, kSamples);

  VelvetReverb split_fx(config);
  split_fx.prepare(48000.0, 257);
  for (int offset = 0; offset < kSamples;) {
    const int count = std::min(37 + (offset % 211), kSamples - offset);
    float* split_channels[] = {split.data() + offset};
    split_fx.process(split_channels, 1, count);
    offset += count;
  }

  for (int i = 0; i < kSamples; ++i) {
    REQUIRE(whole[static_cast<size_t>(i)] ==
            Catch::Approx(split[static_cast<size_t>(i)]).margin(1e-5));
  }
}

TEST_CASE("ConvolutionReverb reports the IR length as its tail", "[effects][reverb][convolution]") {
  ConvolutionReverbConfig config;
  config.decay_sec = 1.0f;
  ConvolutionReverb reverb(config);
  reverb.prepare(48000.0, 512);
  // A synthesized decaying-noise IR spans roughly the requested decay window.
  REQUIRE(reverb.tail_samples() > 0);
  REQUIRE(reverb.tail_samples() == reverb.ir_size());
}

TEST_CASE("ConvolutionReverb with an empty IR reports zero latency and tail",
          "[effects][reverb][convolution]") {
  ConvolutionReverb reverb;  // default-constructed, no IR loaded
  reverb.load_ir(nullptr, 0);
  reverb.prepare(48000.0, 512);
  // process() is a true no-op with no IR, so it must not claim partition latency
  // nor a decay tail.
  REQUIRE(reverb.ir_size() == 0);
  REQUIRE(reverb.latency_samples() == 0);
  REQUIRE(reverb.tail_samples() == 0);
}

TEST_CASE("StereoDelay reports a feedback-extended tail", "[effects][delay][stereo]") {
  StereoDelayConfig config;
  config.delay_time_l_ms = 250.0f;
  config.delay_time_r_ms = 250.0f;
  config.feedback = 0.5f;
  StereoDelay delay(config);
  delay.prepare(48000.0, 512);
  const int one_delay = static_cast<int>(0.250 * 48000.0);
  // Feedback keeps echoes ringing past a single delay period until they hit
  // -60 dB, so the tail must exceed one delay length.
  REQUIRE(delay.tail_samples() > one_delay);
}

TEST_CASE("StereoDelay with no feedback reports one delay length", "[effects][delay][stereo]") {
  StereoDelayConfig config;
  config.delay_time_l_ms = 100.0f;
  config.delay_time_r_ms = 300.0f;
  config.feedback = 0.0f;
  StereoDelay delay(config);
  delay.prepare(48000.0, 512);
  const int longest = static_cast<int>(0.300 * 48000.0);
  // Without feedback the tail is a single pass through the longer delay line.
  REQUIRE(delay.tail_samples() >= longest);
  REQUIRE(delay.tail_samples() < 2 * longest);
}

TEST_CASE("Delay and reverb tails are zero for dry-only configurations",
          "[effects][reverb][delay][tail]") {
  SECTION("stereo delay") {
    StereoDelayConfig config;
    config.dry_wet = 0.0f;
    StereoDelay effect(config);
    effect.prepare(48000.0, 512);
    REQUIRE(effect.tail_samples() == 0);
  }
  SECTION("Dattorro") {
    DattorroReverbConfig config;
    config.dry_wet = 0.0f;
    DattorroReverb effect(config);
    effect.prepare(48000.0, 512);
    REQUIRE(effect.tail_samples() == 0);
  }
  SECTION("FDN") {
    FdnReverbConfig config;
    config.dry_wet = 0.0f;
    FdnReverb effect(config);
    effect.prepare(48000.0, 512);
    REQUIRE(effect.tail_samples() == 0);
  }
  SECTION("velvet") {
    VelvetReverbConfig config;
    config.dry_wet = 0.0f;
    VelvetReverb effect(config);
    effect.prepare(48000.0, 512);
    REQUIRE(effect.tail_samples() == 0);
  }
  SECTION("convolution") {
    ConvolutionReverbConfig config;
    config.dry_wet = 0.0f;
    ConvolutionReverb effect(config);
    effect.prepare(48000.0, 512);
    REQUIRE(effect.latency_samples() > 0);
    REQUIRE(effect.tail_samples() == 0);
  }
}
