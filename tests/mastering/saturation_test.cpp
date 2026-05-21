#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "mastering/saturation/bitcrusher.h"
#include "mastering/saturation/exciter.h"
#include "mastering/saturation/hard_clipper.h"
#include "mastering/saturation/multiband_exciter.h"
#include "mastering/saturation/soft_clipper.h"
#include "mastering/saturation/tape.h"
#include "mastering/saturation/transformer.h"
#include "mastering/saturation/tube.h"
#include "mastering/saturation/waveshaper.h"

using Catch::Matchers::WithinAbs;
using namespace sonare::mastering::saturation;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(float frequency_hz, int sample_rate, int samples, float amplitude = 0.25f) {
  std::vector<float> out(static_cast<size_t>(samples));
  for (int i = 0; i < samples; ++i) {
    out[static_cast<size_t>(i)] =
        amplitude * static_cast<float>(std::sin(2.0 * kPi * frequency_hz * i / sample_rate));
  }
  return out;
}

float peak_abs(const std::vector<float>& samples) {
  float peak = 0.0f;
  for (float sample : samples) peak = std::max(peak, std::abs(sample));
  return peak;
}

float rms_tail(const std::vector<float>& samples, size_t skip = 0) {
  double sum = 0.0;
  size_t count = 0;
  for (size_t i = std::min(skip, samples.size()); i < samples.size(); ++i) {
    sum += static_cast<double>(samples[i]) * samples[i];
    ++count;
  }
  return count == 0 ? 0.0f : static_cast<float>(std::sqrt(sum / static_cast<double>(count)));
}

void process(sonare::mastering::common::ProcessorBase& processor, std::vector<float>& mono) {
  float* channels[] = {mono.data()};
  processor.process(channels, 1, static_cast<int>(mono.size()));
}

}  // namespace

TEST_CASE("Waveshaper applies nonlinear shaping", "[mastering][saturation]") {
  Waveshaper shaper({12.0f, 1.0f, 0.0f, 0.0f, WaveshaperCurve::Tanh});
  shaper.prepare(48000.0, 128);
  std::vector<float> signal = {-0.8f, -0.2f, 0.0f, 0.2f, 0.8f};
  process(shaper, signal);
  REQUIRE(std::abs(signal.front()) < 0.999f);
  REQUIRE(signal[1] != -0.2f);
}

TEST_CASE("SoftClipper and HardClipper constrain peaks", "[mastering][saturation]") {
  std::vector<float> soft = {-2.0f, -0.5f, 0.5f, 2.0f};
  std::vector<float> hard = soft;

  SoftClipper soft_clipper({12.0f, 0.75f, 1.0f});
  HardClipper hard_clipper({0.5f});
  soft_clipper.prepare(48000.0, 128);
  hard_clipper.prepare(48000.0, 128);
  process(soft_clipper, soft);
  process(hard_clipper, hard);

  REQUIRE(peak_abs(soft) <= 0.751f);
  REQUIRE_THAT(hard.front(), WithinAbs(-0.5f, 0.000001f));
  REQUIRE_THAT(hard.back(), WithinAbs(0.5f, 0.000001f));
}

TEST_CASE("Tube and Transformer introduce asymmetric shaping", "[mastering][saturation]") {
  std::vector<float> tube_signal = {-0.5f, 0.5f};
  std::vector<float> transformer_signal = tube_signal;

  Tube tube({12.0f, 0.2f, 1.0f});
  Transformer transformer({12.0f, 0.2f, 1.0f});
  tube.prepare(48000.0, 128);
  transformer.prepare(48000.0, 128);
  process(tube, tube_signal);
  process(transformer, transformer_signal);

  REQUIRE(std::abs(tube_signal[0] + tube_signal[1]) > 0.01f);
  REQUIRE(std::abs(transformer_signal[0] + transformer_signal[1]) > 0.01f);
}

TEST_CASE("Tape saturation changes driven signal and keeps state resettable",
          "[mastering][saturation]") {
  auto signal = sine(1000.0f, 48000, 48000, 0.8f);
  auto first = signal;
  auto second = signal;
  Tape tape({9.0f, 0.8f, 0.2f, -3.0f});
  tape.prepare(48000.0, 1024);
  process(tape, first);
  tape.reset();
  process(tape, second);
  REQUIRE(rms_tail(first, 4096) != rms_tail(signal, 4096));
  REQUIRE_THAT(rms_tail(first, 4096), WithinAbs(rms_tail(second, 4096), 0.000001f));
}

TEST_CASE("Tape hysteresis loop depends on prior signal direction", "[mastering][saturation]") {
  // A defining property of the Jiles-Atherton model: the output at H=0 differs
  // depending on whether the field is rising or falling — the hallmark of
  // hysteresis. A memoryless saturator (tanh) cannot exhibit this property.
  Tape tape({6.0f, 0.5f, 0.9f, 0.0f});
  tape.prepare(48000.0, 1024);

  std::vector<float> ramp_up(2048, 0.0f);
  for (size_t i = 0; i < ramp_up.size(); ++i) {
    ramp_up[i] = static_cast<float>(i) / static_cast<float>(ramp_up.size() - 1);  // 0 → 1
  }
  std::vector<float> ramp_down(2048, 0.0f);
  for (size_t i = 0; i < ramp_down.size(); ++i) {
    ramp_down[i] = 1.0f - static_cast<float>(i) / static_cast<float>(ramp_down.size() - 1);
  }

  auto rising = ramp_up;
  process(tape, rising);
  const float at_top = rising.back();

  auto falling = ramp_down;
  process(tape, falling);
  const float at_bottom = falling.back();

  // Returning to H = 0 from saturation does not return to M = 0 (remanence).
  REQUIRE(std::abs(at_bottom) > 0.01f);
  REQUIRE(at_top > at_bottom);
}

TEST_CASE("BitCrusher quantizes and holds samples", "[mastering][saturation]") {
  std::vector<float> signal = {0.1f, 0.2f, 0.3f, 0.4f};
  BitCrusher crusher({4, 2, 1.0f});
  crusher.prepare(48000.0, 128);
  process(crusher, signal);
  REQUIRE_THAT(signal[1], WithinAbs(signal[0], 0.000001f));
  REQUIRE_THAT(signal[3], WithinAbs(signal[2], 0.000001f));
}

TEST_CASE("Exciter adds high-frequency enhancement", "[mastering][saturation]") {
  auto signal = sine(8000.0f, 48000, 48000, 0.2f);
  const float before = rms_tail(signal, 4096);
  Exciter exciter({3000.0f, 12.0f, 0.5f});
  exciter.prepare(48000.0, 1024);
  process(exciter, signal);
  REQUIRE(rms_tail(signal, 4096) > before * 1.1f);
}

TEST_CASE("MultibandExciter can enhance high band while leaving low band close",
          "[mastering][saturation]") {
  MultibandExciterConfig config;
  config.crossover = {{1000.0f},
                      sonare::mastering::multiband::CrossoverSlope::LR2,
                      sonare::mastering::multiband::CrossoverMode::LinkwitzRiley};
  config.bands = {{3000.0f, 0.0f, 0.0f}, {3000.0f, 12.0f, 0.5f}};
  MultibandExciter exciter(config);
  exciter.prepare(48000.0, 1024);

  auto low = sine(100.0f, 48000, 48000, 0.2f);
  auto high = sine(8000.0f, 48000, 48000, 0.2f);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);
  process(exciter, low);
  exciter.reset();
  process(exciter, high);
  REQUIRE(rms_tail(low, 4096) > low_before * 0.9f);
  REQUIRE(rms_tail(high, 4096) > high_before * 1.05f);
}

TEST_CASE("Saturation processors validate configurations", "[mastering][saturation]") {
  REQUIRE_THROWS(Waveshaper({0.0f, -0.1f, 0.0f, 0.0f, WaveshaperCurve::Tanh}));
  REQUIRE_THROWS(SoftClipper({0.0f, 0.0f, 1.0f}));
  REQUIRE_THROWS(HardClipper({0.0f}));
  REQUIRE_THROWS(Tube({0.0f, 0.0f, 1.5f}));
  REQUIRE_THROWS(Tape({0.0f, -0.1f, 0.2f, 0.0f}));
  REQUIRE_THROWS(Transformer({0.0f, 0.0f, -0.1f}));
  REQUIRE_THROWS(BitCrusher({0, 1, 1.0f}));
  REQUIRE_THROWS(Exciter({0.0f, 0.0f, 0.1f}));
  MultibandExciterConfig config;
  config.bands.resize(1);
  REQUIRE_THROWS(MultibandExciter(config));
}
