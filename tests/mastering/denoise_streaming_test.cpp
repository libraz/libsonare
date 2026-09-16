#include "mastering/repair/denoise_streaming.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "mastering/repair/denoise_classical.h"
#include "repair_metrics.h"
#include "support/alloc_guard.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"
#include "util/exception.h"

using namespace sonare;
using namespace sonare::mastering::repair;

namespace metrics = sonare::test::repair_metrics;

namespace {

using sonare::test::rms;

constexpr int kSampleRate = 48000;

/// The default config with the one field a stream cannot carry replaced.
DenoiseClassicalConfig streaming_default() {
  DenoiseClassicalConfig config;
  config.noise_estimator = DenoiseNoiseEstimator::Imcra;
  return config;
}

/// @brief Pushes @p input through @p processor in blocks of @p block samples.
/// @return The processed signal, which is @p input delayed by latency_samples().
std::vector<float> run_mono(StreamingDenoise& processor, const std::vector<float>& input,
                            int block) {
  std::vector<float> output = input;
  for (std::size_t offset = 0; offset < output.size(); offset += static_cast<std::size_t>(block)) {
    const auto count =
        static_cast<int>(std::min(static_cast<std::size_t>(block), output.size() - offset));
    float* channels[1] = {output.data() + offset};
    processor.process(channels, 1, count);
  }
  return output;
}

/// @brief Two-channel @ref run_mono; both planes advance through one process().
void run_stereo(StreamingDenoise& processor, std::vector<float>& left, std::vector<float>& right,
                int block) {
  for (std::size_t offset = 0; offset < left.size(); offset += static_cast<std::size_t>(block)) {
    const auto count =
        static_cast<int>(std::min(static_cast<std::size_t>(block), left.size() - offset));
    float* channels[2] = {left.data() + offset, right.data() + offset};
    processor.process(channels, 2, count);
  }
}

std::vector<float> tone(std::size_t length, float frequency_hz, float amplitude) {
  std::vector<float> samples(length, 0.0f);
  const double step = 2.0 * constants::kPiD * frequency_hz / kSampleRate;
  for (std::size_t i = 0; i < length; ++i) {
    samples[i] = amplitude * static_cast<float>(std::sin(step * static_cast<double>(i)));
  }
  return samples;
}

/// @brief A tone switched on and off every 125 ms, after @p lead_in of silence.
/// @details Two things a minimum-statistics noise estimator needs, and a plain
///   tone denies it both. The gaps give the tone's own bin a floor to find: a
///   tone that never stops is tracked as the floor itself and then removed, and
///   measured that way every estimator here lands under 1 dB, the offline
///   quantile one included. The silent lead-in is what the first analysis frame
///   seeds the tracker from -- a stream starts where it starts, with no centred
///   padding to quieten frame 0, so beginning mid-programme seeds the floor at
///   programme level and holds it there for the half second the minimum window
///   spans.
std::vector<float> gated_tone(std::size_t lead_in, std::size_t length, float frequency_hz,
                              float amplitude) {
  constexpr std::size_t kPeriod = kSampleRate / 4;
  constexpr std::size_t kOn = kPeriod / 2;
  constexpr std::size_t kRamp = kSampleRate / 200;
  std::vector<float> samples = tone(length, frequency_hz, amplitude);
  for (std::size_t i = 0; i < length; ++i) {
    if (i < lead_in) {
      samples[i] = 0.0f;
      continue;
    }
    const std::size_t phase = (i - lead_in) % kPeriod;
    if (phase >= kOn) {
      samples[i] = 0.0f;
      continue;
    }
    const std::size_t edge = std::min(phase, kOn - 1 - phase);
    if (edge < kRamp) {
      samples[i] *=
          0.5f * (1.0f - std::cos(static_cast<float>(constants::kPiD * static_cast<double>(edge) /
                                                     static_cast<double>(kRamp))));
    }
  }
  return samples;
}

std::vector<float> with_noise(const std::vector<float>& clean, float level, unsigned seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> noise(0.0f, level);
  std::vector<float> samples = clean;
  for (float& sample : samples) sample += noise(rng);
  return samples;
}

/// Index of the first sample above @p floor, or the size when there is none.
std::size_t first_above(const std::vector<float>& samples, float floor) {
  for (std::size_t i = 0; i < samples.size(); ++i) {
    if (std::abs(samples[i]) > floor) return i;
  }
  return samples.size();
}

std::size_t count_mismatches(const std::vector<float>& lhs, const std::vector<float>& rhs) {
  REQUIRE(lhs.size() == rhs.size());
  std::size_t mismatches = 0;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (lhs[i] != rhs[i]) ++mismatches;
  }
  return mismatches;
}

}  // namespace

TEST_CASE("StreamingDenoise refuses the quantile estimator and accepts a recursive one",
          "[mastering][repair][denoise-streaming]") {
  DenoiseClassicalConfig config;
  REQUIRE(config.noise_estimator == DenoiseNoiseEstimator::Quantile);

  bool threw = false;
  try {
    StreamingDenoise refused(config);
  } catch (const SonareException& error) {
    threw = true;
    CHECK(error.code() == ErrorCode::InvalidParameter);
    // The caller has to learn which field to change and what the alternatives
    // are; a message naming neither leaves substitution as the only way out.
    const std::string message = error.what();
    CHECK(message.find("noise_estimator") != std::string::npos);
    CHECK(message.find("Mcra") != std::string::npos);
    CHECK(message.find("Imcra") != std::string::npos);
    CHECK(message.find("Spp") != std::string::npos);
  }
  REQUIRE(threw);

  for (const auto estimator :
       {DenoiseNoiseEstimator::Mcra, DenoiseNoiseEstimator::Imcra, DenoiseNoiseEstimator::Spp}) {
    config.noise_estimator = estimator;
    REQUIRE_NOTHROW(StreamingDenoise{config});
  }
}

TEST_CASE("StreamingDenoise delays an impulse by exactly the latency it reports",
          "[mastering][repair][denoise-streaming]") {
  StreamingDenoise processor(streaming_default());
  processor.prepare(kSampleRate, 256, 1);

  // Well past n_fft, so the impulse sits where the analysis windows overlap
  // fully rather than in the ramp the first frames leave.
  constexpr std::size_t kImpulseIndex = 6000;
  std::vector<float> signal(24000, 0.0f);
  signal[kImpulseIndex] = 1.0f;

  const auto output = run_mono(processor, signal, 256);
  REQUIRE(rms(output) > 0.0f);

  // An impulse in silence has a flat magnitude spectrum, so every frame that
  // spans it takes a flat gain and the reconstruction stays a single sample.
  const std::size_t first = first_above(output, 1.0e-3f);
  REQUIRE(first < output.size());
  REQUIRE(static_cast<int>(first) - static_cast<int>(kImpulseIndex) == processor.latency_samples());
}

TEST_CASE("StreamingDenoise output does not depend on the block size",
          "[mastering][repair][denoise-streaming]") {
  const auto input = with_noise(tone(14400, 440.0f, 0.3f), 0.05f, 4711u);

  std::vector<std::vector<float>> outputs;
  for (const int block : {64, 256, 517}) {
    StreamingDenoise processor(streaming_default());
    processor.prepare(kSampleRate, block, 1);
    outputs.push_back(run_mono(processor, input, block));
  }

  // A processor that emitted silence would pass a three-way equality, and so
  // would one whose output was all delay: require signal past the delay too.
  const StreamingDenoise reference{streaming_default()};
  const int latency = reference.latency_samples();
  for (const auto& output : outputs) {
    REQUIRE(rms(output) > 0.0f);
    REQUIRE(rms(output, static_cast<std::size_t>(latency)) > 0.0f);
  }
  // 517 is deliberately not a divisor of the 256-sample hop, so a ring that
  // wrapped wrongly on a partial hop shows here and almost nowhere else.
  CHECK(count_mismatches(outputs[0], outputs[1]) == 0);
  CHECK(count_mismatches(outputs[0], outputs[2]) == 0);
}

TEST_CASE("StreamingDenoise treats a doubled channel as the mono signal",
          "[mastering][repair][denoise-streaming]") {
  const auto input = with_noise(tone(14400, 660.0f, 0.3f), 0.05f, 91u);

  StreamingDenoise mono_processor(streaming_default());
  mono_processor.prepare(kSampleRate, 256, 1);
  const auto mono = run_mono(mono_processor, input, 256);

  StreamingDenoise stereo_processor(streaming_default());
  stereo_processor.prepare(kSampleRate, 256, 2);
  std::vector<float> left = input;
  std::vector<float> right = input;
  run_stereo(stereo_processor, left, right, 256);

  REQUIRE(rms(mono) > 0.0f);
  REQUIRE(rms(left) > 0.0f);
  // The mask is built from the summed power and divided back out by the same
  // sum, so doubling every channel cancels and the two planes stay the mono answer.
  CHECK(count_mismatches(left, right) == 0);
  CHECK(count_mismatches(left, mono) == 0);
}

TEST_CASE("StreamingDenoise reset restores the post-construction state",
          "[mastering][repair][denoise-streaming]") {
  const auto input = with_noise(tone(14400, 523.0f, 0.3f), 0.05f, 2024u);

  StreamingDenoise processor(streaming_default());
  processor.prepare(kSampleRate, 256, 1);
  const auto first = run_mono(processor, input, 256);
  processor.reset();
  const auto second = run_mono(processor, input, 256);

  REQUIRE(rms(first) > 0.0f);
  CHECK(count_mismatches(first, second) == 0);
}

TEST_CASE("StreamingDenoise raises the segmental SNR of a noisy tone",
          "[mastering][repair][denoise-streaming]") {
  // 0.5 s of noise before the programme starts, then 1 s of gated tone. The
  // lead-in is the half second the minimum window spans; segmental_snr drops
  // frames whose reference is silent, so it is in neither measured number.
  constexpr std::size_t kLeadIn = 24000;
  constexpr std::size_t kLength = 72000;
  const auto clean = gated_tone(kLeadIn, kLength, 440.0f, 0.3f);
  const auto noisy = with_noise(clean, 0.05f, 7u);

  StreamingDenoise processor(streaming_default());
  processor.prepare(kSampleRate, 512, 1);
  const auto processed = run_mono(processor, noisy, 512);

  const auto latency = static_cast<std::size_t>(processor.latency_samples());
  const std::size_t compared = kLength - latency;
  const std::vector<float> reference(clean.begin(), clean.begin() + compared);
  const std::vector<float> before(noisy.begin(), noisy.begin() + compared);
  const std::vector<float> after(processed.begin() + latency, processed.end());

  REQUIRE(rms(reference) > 0.0f);
  REQUIRE(rms(after) > 0.0f);
  const double input_snr = metrics::segmental_snr(reference, before, kSampleRate);
  const double output_snr = metrics::segmental_snr(reference, after, kSampleRate);
  // Measured 11.14 dB in, 23.29 dB out; the margin is half of that gain.
  CHECK(output_snr > input_snr + 6.0);
}

TEST_CASE("StreamingDenoise process allocates nothing", "[mastering][repair][denoise-streaming]") {
  const auto input = with_noise(tone(4096, 440.0f, 0.3f), 0.05f, 5u);
  std::vector<float> buffer = input;

  StreamingDenoise processor(streaming_default());
  processor.prepare(kSampleRate, 512, 1);

  // Armed from the very first block, so the frame that runs the transforms for
  // the first time is inside the count rather than excused as a warm-up.
  sonare::test::AllocationGuard guard;
  for (std::size_t offset = 0; offset + 512 <= buffer.size(); offset += 512) {
    float* channels[1] = {buffer.data() + offset};
    processor.process(channels, 1, 512);
  }
  CHECK(guard.count() == 0);
}

TEST_CASE("StreamingDenoise recovers from a non-finite sample and counts one block",
          "[mastering][repair][denoise-streaming]") {
  const auto input = with_noise(tone(14400, 440.0f, 0.3f), 0.05f, 31u);

  // The control comes first. A processor that bumped the counter unconditionally
  // would satisfy every assertion below it, so "zero on clean material" is what
  // makes the count mean anything.
  StreamingDenoise clean_processor(streaming_default());
  clean_processor.prepare(kSampleRate, 512, 1);
  const auto clean_output = run_mono(clean_processor, input, 512);
  REQUIRE(rms(clean_output) > 0.0f);
  REQUIRE(clean_processor.non_finite_discard_count() == 0);

  StreamingDenoise processor(streaming_default());
  processor.prepare(kSampleRate, 512, 1);
  std::vector<float> poisoned = input;
  poisoned[1000] = std::numeric_limits<float>::quiet_NaN();
  const auto output = run_mono(processor, poisoned, 512);

  CHECK(processor.non_finite_discard_count() > 0);
  // One caller sample is resident in four consecutive analysis frames, so a
  // count bumped per frame instead of per block would read four here.
  CHECK(processor.non_finite_discard_count() <= 2);

  // Recovery, not detection, is the point: a processor that counted and carried
  // the poison forward would pass everything above and fail here. The tail also
  // has to still carry signal, or returning silence forever would pass too.
  constexpr std::size_t kTail = 8000;
  for (std::size_t i = kTail; i < output.size(); ++i) {
    CAPTURE(i);
    REQUIRE(std::isfinite(output[i]));
  }
  CHECK(rms(output, kTail) > 0.0f);
}
