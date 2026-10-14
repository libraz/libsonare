/// @file formant_warp_test.cpp
/// @brief Tests for the offline LPC formant warp.

#include "effects/formant_warp.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

#include "core/audio.h"
#include "core/window.h"
#include "support/alloc_guard.h"
#include "util/constants.h"

namespace {

sonare::Audio voiced_tone(int seconds, int sample_rate) {
  std::vector<float> samples(static_cast<size_t>(seconds) * static_cast<size_t>(sample_rate));
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    samples[i] = 0.3f * std::sin(sonare::constants::kTwoPi * 180.0f * t) +
                 0.1f * std::sin(sonare::constants::kTwoPi * 900.0f * t);
  }
  return sonare::Audio::from_vector(std::move(samples), sample_rate);
}

}  // namespace

TEST_CASE("FormantWarp allocations do not grow with the analysis frame count",
          "[effects][formant]") {
  // Every hop runs an LPC analysis; its plan and scratch must be built once, so
  // three times the frames may not cost more allocations than one time.
  sonare::FormantWarpConfig config;
  config.factor = 1.2f;
  const sonare::FormantWarp warp(config);
  const sonare::Audio short_input = voiced_tone(1, 48000);
  const sonare::Audio long_input = voiced_tone(3, 48000);

  // The production stream obtains its periodic Hann from the thread-local cache during prepare.
  // Warm that shared entry before either guarded call so the comparison counts only the work done
  // by each otherwise-identical stream setup.
  (void)sonare::get_window_cached(sonare::WindowType::Hann, sonare::formant_warp_frame_size(48000),
                                  true);

  auto allocations = [&](const sonare::Audio& input) {
    sonare::test::AllocationGuard guard;
    const sonare::Audio output = warp.process(input);
    const size_t count = guard.count();
    REQUIRE(output.size() == input.size());
    return count;
  };
  const size_t short_count = allocations(short_input);
  const size_t long_count = allocations(long_input);
  CAPTURE(short_count, long_count);
  CHECK(long_count == short_count);
}

namespace {

// Deterministic broadband material: a tone plus a pseudo-random excitation, so every frame has a
// real LPC model and none passes through as silence.
std::vector<float> excited_tone(size_t samples, int sample_rate) {
  std::vector<float> x(samples);
  uint32_t state = 2463534242u;
  float lowpass = 0.0f;
  for (size_t i = 0; i < samples; ++i) {
    state = state * 1664525u + 1013904223u;
    const float noise = static_cast<float>(state >> 8) / 16777216.0f - 0.5f;
    lowpass += 0.3f * (noise - lowpass);
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    x[i] = 0.3f * std::sin(sonare::constants::kTwoPi * 150.0f * t) + 0.4f * lowpass;
  }
  return x;
}

sonare::FormantWarpStream prepared_stream(int sample_rate, float factor) {
  sonare::FormantWarpStream stream;
  stream.prepare(sonare::formant_warp_frame_size(sample_rate),
                 sonare::formant_warp_lpc_order(sample_rate));
  stream.set_factor(factor);
  return stream;
}

}  // namespace

TEST_CASE("FormantWarpStream frame and order follow the rate", "[effects][formant]") {
  CHECK(sonare::formant_warp_frame_size(48000) == sonare::kFormantWarpFrameAt48k);
  CHECK(sonare::formant_warp_frame_size(44100) == 940);
  CHECK(sonare::formant_warp_frame_size(44100) % 4 == 0);
  CHECK(sonare::formant_warp_lpc_order(48000) == 50);
  CHECK(sonare::formant_warp_lpc_order(22050) == 24);
  CHECK(sonare::formant_warp_frame_size(48000) == prepared_stream(48000, 1.2f).latency_samples());
}

TEST_CASE("FormantWarp periodic Hann keeps quarter-hop squared COLA", "[effects][formant]") {
  for (const int frame_size : {940, 1024}) {
    const auto window = sonare::get_window_cached(sonare::WindowType::Hann, frame_size, true);
    const int hop = frame_size / 4;
    float max_error = 0.0f;
    for (int i = 0; i < hop; ++i) {
      float squared_sum = 0.0f;
      for (int offset = 0; offset < 4; ++offset) {
        const float value = (*window)[static_cast<size_t>(i + offset * hop)];
        squared_sum += value * value;
      }
      max_error = std::max(max_error, std::abs(squared_sum - 1.5f));
    }
    CAPTURE(frame_size, max_error);
    CHECK(max_error <= 1.0e-5f);
  }
}

TEST_CASE("FormantWarpStream at unity is the input through its delay, bit for bit",
          "[effects][formant]") {
  for (const int sample_rate : {44100, 48000}) {
    CAPTURE(sample_rate);
    sonare::FormantWarpStream stream = prepared_stream(sample_rate, 1.0f);
    const size_t latency = static_cast<size_t>(stream.latency_samples());
    const std::vector<float> input = excited_tone(latency * 6, sample_rate);
    std::vector<float> output(input.size());
    stream.process(input.data(), output.data(), static_cast<int>(input.size()));
    for (size_t i = 0; i < latency; ++i) REQUIRE(output[i] == 0.0f);
    for (size_t i = latency; i < input.size(); ++i) {
      REQUIRE(output[i] == input[i - latency]);
    }
  }
}

TEST_CASE("FormantWarpStream output does not depend on how the input is cut into blocks",
          "[effects][formant]") {
  constexpr int sample_rate = 48000;
  const std::vector<float> input = excited_tone(30000, sample_rate);
  sonare::FormantWarpStream whole = prepared_stream(sample_rate, 1.3f);
  std::vector<float> expected(input.size());
  whole.process(input.data(), expected.data(), static_cast<int>(input.size()));

  for (const size_t block : {size_t{1}, size_t{7}, size_t{128}, size_t{1000}}) {
    CAPTURE(block);
    sonare::FormantWarpStream stream = prepared_stream(sample_rate, 1.3f);
    std::vector<float> output(input.size());
    for (size_t pos = 0; pos < input.size(); pos += block) {
      const size_t n = std::min(block, input.size() - pos);
      stream.process(input.data() + pos, output.data() + pos, static_cast<int>(n));
    }
    REQUIRE(output == expected);
  }
}

TEST_CASE("FormantWarpStream processes in place", "[effects][formant]") {
  constexpr int sample_rate = 48000;
  const std::vector<float> input = excited_tone(12000, sample_rate);
  sonare::FormantWarpStream separate = prepared_stream(sample_rate, 0.8f);
  std::vector<float> expected(input.size());
  separate.process(input.data(), expected.data(), static_cast<int>(input.size()));

  sonare::FormantWarpStream in_place = prepared_stream(sample_rate, 0.8f);
  std::vector<float> buffer = input;
  in_place.process(buffer.data(), buffer.data(), static_cast<int>(buffer.size()));
  REQUIRE(buffer == expected);
}

TEST_CASE("FormantWarpStream process allocates nothing", "[effects][formant]") {
  constexpr int sample_rate = 48000;
  sonare::FormantWarpStream stream = prepared_stream(sample_rate, 1.25f);
  const std::vector<float> input = excited_tone(20000, sample_rate);
  std::vector<float> output(input.size());
  size_t count = 0;
  {
    sonare::test::AllocationGuard guard;
    stream.process(input.data(), output.data(), 5000);
    stream.set_factor(0.7f);
    stream.process(input.data() + 5000, output.data() + 5000, 5000);
    stream.set_factor(1.0f);
    stream.process(input.data() + 10000, output.data() + 10000, 5000);
    stream.set_factor(1.4f);
    stream.reset();
    stream.process(input.data() + 15000, output.data() + 15000, 5000);
    count = guard.count();
  }
  CHECK(count == 0);
}

TEST_CASE("FormantWarpStream crossing unity does not step the output", "[effects][formant]") {
  constexpr int sample_rate = 48000;
  constexpr float kWarp = 0.6f;
  const std::vector<float> input = excited_tone(sample_rate, sample_rate);
  sonare::FormantWarpStream stream = prepared_stream(sample_rate, kWarp);
  std::vector<float> output(input.size());
  const int third = static_cast<int>(input.size() / 3);
  stream.process(input.data(), output.data(), third);
  stream.set_factor(1.0f);
  stream.process(input.data() + third, output.data() + third, third);
  stream.set_factor(kWarp);
  stream.process(input.data() + 2 * third, output.data() + 2 * third,
                 static_cast<int>(input.size()) - 2 * third);

  // The largest sample-to-sample step across the switches stays within that of the warped
  // signal away from them: a hard cut between the delayed input and the overlap-add output
  // would show as a step of their difference.
  auto largest_step = [](const std::vector<float>& x, size_t from, size_t to) {
    float step = 0.0f;
    for (size_t i = from + 1; i < to; ++i) step = std::max(step, std::abs(x[i] - x[i - 1]));
    return step;
  };
  const size_t latency = static_cast<size_t>(stream.latency_samples());
  const size_t third_samples = static_cast<size_t>(third);
  // The first third, past the start-up, is warped throughout; the switches to and from unity
  // land at output indices third and 2 * third, where the crossfade runs.
  const float steady = largest_step(output, 2 * latency, third_samples);
  const float crossing = largest_step(output, third_samples - 64, 2 * third_samples + 1024);
  CAPTURE(steady, crossing);
  CHECK(crossing < 1.25f * steady);
}

TEST_CASE("FormantWarp is the streaming stage over the whole buffer", "[effects][formant]") {
  for (const int sample_rate : {44100, 48000}) {
    CAPTURE(sample_rate);
    const std::vector<float> samples = excited_tone(static_cast<size_t>(sample_rate), sample_rate);
    sonare::FormantWarpConfig config;
    config.factor = 1.2f;
    config.lpc_order = 0;
    config.frame_in_time = true;
    const sonare::Audio one_shot =
        sonare::FormantWarp(config).process(sonare::Audio::from_vector(samples, sample_rate));

    sonare::FormantWarpStream stream = prepared_stream(sample_rate, 1.2f);
    const size_t latency = static_cast<size_t>(stream.latency_samples());
    std::vector<float> padded = samples;
    padded.resize(samples.size() + latency, 0.0f);
    std::vector<float> streamed(padded.size());
    stream.process(padded.data(), streamed.data(), static_cast<int>(padded.size()));
    REQUIRE(one_shot.size() == samples.size());
    REQUIRE(std::equal(one_shot.data(), one_shot.data() + one_shot.size(),
                       streamed.begin() + static_cast<std::ptrdiff_t>(latency)));
  }
}

namespace {

/// A 173 Hz voiced harmonic series under one formant near 900 Hz, peak-normalised to 1.
std::vector<float> formant_voiced_shape(size_t samples, int sample_rate) {
  std::vector<double> shape(samples, 0.0);
  for (int h = 1; 173.0 * h < sample_rate / 2.0; ++h) {
    const double f = 173.0 * h;
    const double envelope = 1.0 / (1.0 + std::pow((f - 900.0) / 600.0, 2.0)) / h;
    for (size_t i = 0; i < samples; ++i) {
      shape[i] += envelope * std::sin(sonare::constants::kTwoPiD * f * static_cast<double>(i) /
                                      static_cast<double>(sample_rate));
    }
  }
  double peak = 0.0;
  for (const double v : shape) peak = std::max(peak, std::abs(v));
  std::vector<float> out(samples);
  for (size_t i = 0; i < samples; ++i) out[i] = static_cast<float>(shape[i] / peak);
  return out;
}

}  // namespace

TEST_CASE("FormantWarp warps quiet voiced input like loud input", "[effects][formant]") {
  // Amplitude scaling must carry the warp with it: a quiet but energetic voiced frame
  // is predictable, so its absolute LPC error is tiny, and that alone must not pass it
  // through unwarped.
  constexpr int kRate = 48000;
  constexpr size_t kSamples = 24000;
  constexpr size_t kEdge = 2048;
  const std::vector<float> shape = formant_voiced_shape(kSamples, kRate);
  const auto warped = [&](float factor, float amp) {
    std::vector<float> input(shape);
    for (float& v : input) v *= amp;
    const sonare::Audio out = sonare::FormantWarp({factor, 12, 1.0f})
                                  .process(sonare::Audio::from_vector(std::move(input), kRate));
    REQUIRE(out.size() == kSamples);
    std::vector<double> normalised(kSamples);
    for (size_t i = 0; i < kSamples; ++i) {
      REQUIRE(std::isfinite(out[i]));
      normalised[i] = static_cast<double>(out[i]) / amp;
    }
    return normalised;
  };
  const auto relative_rms = [&](const std::vector<double>& a, const std::vector<double>& b) {
    double diff = 0.0;
    double ref = 0.0;
    for (size_t i = kEdge; i < kSamples - kEdge; ++i) {
      diff += (a[i] - b[i]) * (a[i] - b[i]);
      ref += b[i] * b[i];
    }
    return std::sqrt(diff / ref);
  };
  std::vector<double> dry(kSamples);
  for (size_t i = 0; i < kSamples; ++i) dry[i] = shape[i];

  for (const float factor : {0.55f, 1.3f, 1.65f}) {
    CAPTURE(factor);
    const std::vector<double> loud = warped(factor, 0.2f);
    // The loud render is a real warp, not the input.
    CHECK(relative_rms(loud, dry) > 0.05);
    for (const float amp : {0.05f, 0.0125f}) {
      CAPTURE(amp);
      CHECK(relative_rms(warped(factor, amp), loud) < 0.02);
    }
  }

  // Unity and exact silence stay identities.
  const std::vector<double> unity = warped(1.0f, 0.0125f);
  CHECK(relative_rms(unity, dry) < 1.0e-6);
  std::vector<float> silence(kSamples, 0.0f);
  const sonare::Audio silent =
      sonare::FormantWarp({1.3f, 12, 1.0f}).process(sonare::Audio::from_vector(silence, kRate));
  CHECK(
      std::all_of(silent.data(), silent.data() + silent.size(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("FormantWarp keeps the level of a pure tone at any amplitude", "[effects][formant]") {
  // A pure tone is predicted almost exactly, leaving no envelope to move; recolouring that
  // singular a predictor would change the level, so the tone passes at every amplitude.
  constexpr int kRate = 48000;
  constexpr size_t kSamples = 24000;
  for (const float amp : {0.5f, 0.0125f}) {
    CAPTURE(amp);
    std::vector<float> input(kSamples);
    for (size_t i = 0; i < kSamples; ++i) {
      input[i] = amp * std::sin(sonare::constants::kTwoPi * 440.0f * static_cast<float>(i) /
                                static_cast<float>(kRate));
    }
    const sonare::Audio out =
        sonare::FormantWarp({1.3f, 12, 1.0f}).process(sonare::Audio::from_vector(input, kRate));
    double in_energy = 0.0;
    double out_energy = 0.0;
    for (size_t i = 2048; i < kSamples - 2048; ++i) {
      in_energy += static_cast<double>(input[i]) * input[i];
      out_energy += static_cast<double>(out[i]) * out[i];
    }
    CHECK(std::abs(10.0 * std::log10(out_energy / in_energy)) < 1.5);
  }
}
