/// @file reverb_tail_test.cpp
/// @brief Reverb/delay decay-tail reporting so offline bounces are not
///        truncated, plus the convolution empty-IR latency contract.

#include <catch2/catch_test_macros.hpp>

#include "effects/delay/stereo_delay.h"
#include "effects/reverb/convolution_reverb.h"
#include "effects/reverb/fdn_reverb.h"
#include "effects/reverb/velvet_reverb.h"

using sonare::effects::delay::StereoDelay;
using sonare::effects::delay::StereoDelayConfig;
using sonare::effects::reverb::ConvolutionReverb;
using sonare::effects::reverb::ConvolutionReverbConfig;
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
