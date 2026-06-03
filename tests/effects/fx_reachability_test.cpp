// Coverage for the mastering + creative-FX audit fixes:
//   * effects.reverb.convolution now synthesizes a default IR from scene params
//     and produces an actual wet tail (previously a dead passthrough);
//   * the creative effects (reverbs / modulation / delay) are reachable from the
//     one-shot named-processor apply path and change the signal;
//   * multiband.imager / multiband.dynamicEq accept a non-default crossover plus
//     per-band settings without throwing "band count must match crossover" and
//     actually change the output;
//   * insert_factory_names() lists the newly-registered FX names (the names the
//     sonare_mastering_insert_names() C-ABI getter joins).
//
// These cases live in tests/effects/ but exercise the mastering API surface,
// matching how param_wiring_test.cpp / insert_factory_extra_test.cpp drive the
// named-processor apply path and the insert factory.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "rt/processor_base.h"

namespace {

using sonare::mastering::api::apply_named_processor;
using sonare::mastering::api::apply_named_processor_stereo;
using sonare::mastering::api::insert_factory_names;
using sonare::mastering::api::make_insert;
using sonare::mastering::api::Param;

bool ListContains(const std::vector<std::string>& names, const std::string& target) {
  return std::find(names.begin(), names.end(), target) != names.end();
}

// A short impulse (unit spike followed by silence) so a reverb's tail energy is
// trivially distinguishable from the dry input.
std::vector<float> impulse(std::size_t length) {
  std::vector<float> x(length, 0.0f);
  if (!x.empty()) x[0] = 1.0f;
  return x;
}

double tail_energy(const std::vector<float>& x, std::size_t start) {
  double energy = 0.0;
  for (std::size_t i = std::min(start, x.size()); i < x.size(); ++i) {
    energy += static_cast<double>(x[i]) * static_cast<double>(x[i]);
  }
  return energy;
}

bool all_finite(const std::vector<float>& x) {
  return std::all_of(x.begin(), x.end(), [](float s) { return std::isfinite(s); });
}

}  // namespace

#ifdef SONARE_WITH_FX

TEST_CASE("effects.reverb.convolution produces a real wet tail from scene params",
          "[effects][reverb][convolution][audit]") {
  constexpr int kSampleRate = 48000;
  constexpr std::size_t kLength = 24000;  // 0.5 s
  constexpr int kBlock = 512;

  // Fully wet so the synthesized decaying-noise IR dominates; a 0.4 s tail.
  auto processor = make_insert("effects.reverb.convolution", R"({"decaySec":0.4,"dryWet":1.0})");
  REQUIRE(processor != nullptr);
  processor->prepare(static_cast<double>(kSampleRate), kBlock);

  std::vector<float> buffer = impulse(kLength);
  for (std::size_t pos = 0; pos < buffer.size(); pos += kBlock) {
    const int n = static_cast<int>(std::min<std::size_t>(kBlock, buffer.size() - pos));
    float* ch = buffer.data() + pos;
    float* channels[] = {ch};
    processor->process(channels, 1, n);
  }

  REQUIRE(all_finite(buffer));
  // The dry input is a single spike at sample 0; a working convolution reverb
  // must spread energy well past the input into the decay tail. (Account for the
  // partitioned-convolver latency by measuring from a few thousand samples in.)
  const double tail = tail_energy(buffer, 4000);
  REQUIRE(tail > 1e-6);
}

TEST_CASE("effects.reverb.convolution default decaySec differs from a near-dry mix",
          "[effects][reverb][convolution][audit]") {
  constexpr int kSampleRate = 48000;
  constexpr std::size_t kLength = 12000;
  constexpr int kBlock = 512;

  auto run = [&](const char* params) {
    auto p = make_insert("effects.reverb.convolution", params);
    REQUIRE(p != nullptr);
    p->prepare(static_cast<double>(kSampleRate), kBlock);
    std::vector<float> buf = impulse(kLength);
    for (std::size_t pos = 0; pos < buf.size(); pos += kBlock) {
      const int n = static_cast<int>(std::min<std::size_t>(kBlock, buf.size() - pos));
      float* ch = buf.data() + pos;
      float* channels[] = {ch};
      p->process(channels, 1, n);
    }
    return buf;
  };

  const std::vector<float> wet = run(R"({"decaySec":0.3,"dryWet":1.0})");
  const std::vector<float> dry = run(R"({"decaySec":0.3,"dryWet":0.0})");
  // Fully-dry passes the (latency-delayed) input through with essentially no
  // tail; fully-wet has a substantial tail. They must differ.
  REQUIRE(tail_energy(wet, 4000) > tail_energy(dry, 4000) + 1e-6);
}

TEST_CASE("effects.reverb.fdn is reachable from the one-shot named-processor path",
          "[mastering][named_processor][effects][audit]") {
  constexpr int kSampleRate = 48000;
  constexpr std::size_t kLength = 24000;

  std::vector<float> input = impulse(kLength);
  const std::vector<Param> params{{"decaySec", 1.5}, {"dryWet", 0.6}};
  const auto result =
      apply_named_processor("effects.reverb.fdn", input.data(), input.size(), kSampleRate, params);

  REQUIRE(result.samples.size() == input.size());
  REQUIRE(all_finite(result.samples));
  // A reverb spreads the input spike into a tail: there must be energy far past
  // the original impulse, which the dry input does not have.
  REQUIRE(tail_energy(result.samples, 4000) > 1e-6);
}

TEST_CASE("effects.modulation.phaser is reachable from the one-shot named-processor path",
          "[mastering][named_processor][effects][audit]") {
  constexpr int kSampleRate = 48000;
  constexpr std::size_t kLength = 12000;

  // A steady tone so the phaser's moving notches measurably alter the waveform.
  std::vector<float> input(kLength);
  for (std::size_t i = 0; i < input.size(); ++i) {
    input[i] = 0.5f * std::sin(2.0f * 3.14159265358979f * 440.0f * static_cast<float>(i) /
                               static_cast<float>(kSampleRate));
  }

  const std::vector<Param> params{{"rateHz", 0.5}, {"stages", 6}, {"dryWet", 1.0}};
  const auto result = apply_named_processor("effects.modulation.phaser", input.data(), input.size(),
                                            kSampleRate, params);

  REQUIRE(result.samples.size() == input.size());
  REQUIRE(all_finite(result.samples));

  double diff = 0.0;
  for (std::size_t i = 0; i < input.size(); ++i) {
    const double d = static_cast<double>(result.samples[i]) - static_cast<double>(input[i]);
    diff += d * d;
  }
  REQUIRE(diff > 1e-3);
}

TEST_CASE("FX names are registered in insert_factory_names()",
          "[mastering][insert_factory][effects][audit]") {
  const auto names = insert_factory_names();
  for (const char* name :
       {"effects.reverb.convolution", "effects.reverb.fdn", "effects.reverb.velvet",
        "effects.reverb.dattorro", "effects.modulation.phaser", "effects.delay.stereo"}) {
    DYNAMIC_SECTION(name) { REQUIRE(ListContains(names, name)); }
  }
}

TEST_CASE("FX names are registered in the named-processor registry",
          "[mastering][named_processor][effects][audit]") {
  const auto names = sonare::mastering::api::processor_names();
  REQUIRE(ListContains(names, "effects.reverb.fdn"));
  REQUIRE(ListContains(names, "effects.modulation.phaser"));
}

#endif  // SONARE_WITH_FX

TEST_CASE("multiband.imager accepts a custom crossover with per-band settings",
          "[mastering][multiband][imager][audit]") {
  constexpr int kSampleRate = 48000;
  constexpr std::size_t kLength = 8192;

  // Two cutoffs -> three bands; previously a non-default crossover threw
  // "band count must match crossover" because the band vector stayed at its
  // 3-entry default only by luck. Use distinct per-band widths so the imaging
  // actually changes the stereo signal.
  std::vector<float> left(kLength), right(kLength);
  for (std::size_t i = 0; i < kLength; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
    left[i] = 0.4f * std::sin(2.0f * 3.14159265358979f * 200.0f * t);
    right[i] = 0.4f * std::sin(2.0f * 3.14159265358979f * 200.0f * t + 0.7f);
  }
  const std::vector<float> orig_left = left;

  const std::vector<Param> params{{"cutoff0Hz", 300.0},
                                  {"cutoff1Hz", 3000.0},
                                  {"band0.width", 0.2},
                                  {"band1.width", 1.8},
                                  {"band2.width", 1.5}};
  // Must not throw.
  const auto result = apply_named_processor_stereo("multiband.imager", left.data(), right.data(),
                                                   left.size(), kSampleRate, params);

  REQUIRE(result.left.size() == left.size());
  REQUIRE(std::all_of(result.left.begin(), result.left.end(),
                      [](float s) { return std::isfinite(s); }));

  double diff = 0.0;
  for (std::size_t i = 0; i < orig_left.size(); ++i) {
    const double d = static_cast<double>(result.left[i]) - static_cast<double>(orig_left[i]);
    diff += d * d;
  }
  REQUIRE(diff > 1e-4);
}

TEST_CASE("multiband.imager custom crossover builds as an insert without throwing",
          "[mastering][multiband][imager][insert_factory][audit]") {
  auto processor = make_insert(
      "multiband.imager",
      R"({"cutoff0Hz":250,"cutoff1Hz":2500,"cutoff2Hz":8000,"band0.width":0.5,"band3.width":1.6})");
  REQUIRE(processor != nullptr);  // 3 cutoffs -> 4 bands, no band-count throw
  processor->prepare(48000.0, 512);
}

TEST_CASE("multiband.dynamicEq accepts a custom crossover with per-band dynamic settings",
          "[mastering][multiband][dynamicEq][audit]") {
  constexpr int kSampleRate = 48000;
  constexpr std::size_t kLength = 8192;

  std::vector<float> left(kLength), right(kLength);
  for (std::size_t i = 0; i < kLength; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
    left[i] = 0.6f * std::sin(2.0f * 3.14159265358979f * 1000.0f * t);
    right[i] = left[i];
  }
  const std::vector<float> orig_left = left;

  // Two cutoffs -> three crossover bands; configure one strong dynamic band in
  // the middle crossover band targeting the 1 kHz tone. Must not throw the
  // band-count error and must change the signal.
  const std::vector<Param> params{{"cutoff0Hz", 300.0},
                                  {"cutoff1Hz", 3000.0},
                                  {"band1.dyn0.frequencyHz", 1000.0},
                                  {"band1.dyn0.thresholdDb", -40.0},
                                  {"band1.dyn0.ratio", 6.0},
                                  {"band1.dyn0.rangeDb", -18.0},
                                  {"band1.dyn0.enabled", 1.0}};
  const auto result = apply_named_processor_stereo("multiband.dynamicEq", left.data(), right.data(),
                                                   left.size(), kSampleRate, params);

  REQUIRE(result.left.size() == left.size());
  REQUIRE(std::all_of(result.left.begin(), result.left.end(),
                      [](float s) { return std::isfinite(s); }));

  double diff = 0.0;
  for (std::size_t i = 0; i < orig_left.size(); ++i) {
    const double d = static_cast<double>(result.left[i]) - static_cast<double>(orig_left[i]);
    diff += d * d;
  }
  REQUIRE(diff > 1e-4);
}
