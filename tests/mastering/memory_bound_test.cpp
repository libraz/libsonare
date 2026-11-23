#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/audio.h"
#include "mastering/api/chain.h"
#include "mastering/api/presets.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "support/alloc_guard.h"
#include "util/constants.h"

// Peak live heap of an offline call, measured at two input lengths so the part that grows with the
// input (bytes held per input byte) and the part that does not are bounded separately. A preset's
// lengths all exceed the 64k-sample offline block, so every processor is prepared at one size.

namespace {

using sonare::Audio;
using sonare::test::kLiveBytesMeasured;
using sonare::test::PeakLiveBytesGuard;

constexpr int kSampleRate = 44100;
// The shortest pair that still clears the 64k-sample block at this rate.
constexpr float kShortPresetLengths[2] = {1.6f, 2.2f};
constexpr float kRepairLengths[2] = {0.5f, 1.0f};
constexpr double kMegabyte = 1024.0 * 1024.0;

// A processor that reserves state for the realtime channel maximum instead of the channels an
// offline run prepares took 86 MB here; every preset measures at most 10.5 MB.
constexpr double kFixedBoundBytes = 16.0 * kMegabyte;

std::vector<float> fixture(float seconds, float gain, std::uint32_t seed) {
  const auto count = static_cast<std::size_t>(static_cast<float>(kSampleRate) * seconds);
  std::vector<float> samples(count);
  const float two_pi = 2.0f * static_cast<float>(sonare::constants::kPiD);
  std::uint32_t state = seed;
  for (std::size_t index = 0; index < count; ++index) {
    const float t = static_cast<float>(index) / static_cast<float>(kSampleRate);
    state = state * 1664525u + 1013904223u;
    const float noise = static_cast<float>(state >> 8) / static_cast<float>(1u << 24) - 0.5f;
    const float env = 0.55f + 0.35f * std::sin(two_pi * 1.7f * t);
    samples[index] = gain * (env * (0.18f * std::sin(two_pi * 110.0f * t) +
                                    0.10f * std::sin(two_pi * 440.0f * t) +
                                    0.04f * std::sin(two_pi * 1760.0f * t)) +
                             0.01f * noise);
  }
  return samples;
}

struct PeakLine {
  double slope;          // bytes held per input byte, from the first to the last length
  double fixed_bytes;    // bytes held independently of the input length
  double first_slope;    // slope between the first two lengths
  double last_slope;     // slope between the last two lengths
  double longest_input;  // bytes of the longest input
};

/// @p make_input builds the input for a length in seconds outside the measurement; @p run takes it
/// and returns its size in bytes. The peak covers the whole call, its result included.
template <std::size_t N, typename MakeInput, typename Run>
PeakLine measure(const float (&lengths)[N], MakeInput make_input, Run run) {
  static_assert(N >= 2, "a line needs two lengths");
  double peaks[N] = {};
  double inputs[N] = {};
  for (std::size_t i = 0; i < N; ++i) {
    const auto input = make_input(lengths[i]);
    PeakLiveBytesGuard guard;
    inputs[i] = run(input);
    peaks[i] = static_cast<double>(guard.peak());
  }
  REQUIRE(peaks[0] > 0.0);
  const auto slope_between = [&](std::size_t i, std::size_t j) {
    return (peaks[j] - peaks[i]) / (inputs[j] - inputs[i]);
  };
  const double slope = slope_between(0, N - 1);
  return {slope, peaks[0] - slope * inputs[0], slope_between(0, 1), slope_between(N - 2, N - 1),
          inputs[N - 1]};
}

std::vector<std::vector<float>> stereo_input(float seconds) {
  return {fixture(seconds, 1.0f, 1u), fixture(seconds, 0.83f, 2u)};
}

std::vector<Audio> stereo_audio(float seconds) {
  return {Audio::from_vector(fixture(seconds, 1.0f, 1u), kSampleRate),
          Audio::from_vector(fixture(seconds, 0.83f, 2u), kSampleRate)};
}

/// Bytes held per input byte for the two presets the default run measures, plus headroom.
double preset_slope_bound(const std::string& name) {
  return name == "fieldRecording" ? 8.75 : 2.25;  // 7.0 and 1.0 at the short lengths
}

/// A preset with a stage of its own, measured at three lengths. The chain's fixed state and a
/// whole-input stage peak at different times, so where the fixed part is the larger the slope reads
/// the wrong line and a regression in the stage passes; the lengths start past that crossover and
/// the case checks the line is straight across them. The mastering chain carries about 10 MB of
/// fixed state, which a preset holding nothing beyond the chain's copies passes after about 23 s
/// in stereo and between 24 and 30 s in mono.
struct LinedPreset {
  const char* name;
  double slope_bound;  // measured plus headroom
  float lengths[3];
};

constexpr LinedPreset kLinedPresets[] = {
    {"pop", 2.25, {30.0f, 36.0f, 42.0f}},          // 2.0: the result beside a whole-input copy
    {"speech", 3.5, {20.0f, 25.0f, 30.0f}},        // 3.0
    {"podcast", 3.5, {20.0f, 25.0f, 30.0f}},       // 3.0
    {"audiobook", 8.75, {10.0f, 15.0f, 20.0f}},    // 8.0
    {"aiMusic", 8.75, {8.0f, 12.0f, 16.0f}},       // 8.0
    {"fieldRecording", 8.75, {2.0f, 3.0f, 4.0f}},  // 8.0; no chain state
    {"voiceMemo", 8.75, {2.0f, 3.0f, 4.0f}},       // 8.0
    {"vinyl", 8.75, {2.0f, 3.0f, 4.0f}},           // 8.0
    {"shellac78", 8.75, {2.0f, 3.0f, 4.0f}},       // 8.0
    {"tapeHiss", 3.5, {24.0f, 30.0f, 36.0f}},      // 3.0; mono reads 4.0 below 6 s, 0.6 MB over
};

// Segment slopes of a straight line agree within this fraction of the larger.
constexpr double kStraightLineTolerance = 0.05;
// The chain-only presets measured identical to pop's line; one extra whole-input copy at the
// single length adds 8.5 MB.
constexpr float kChainOnlyLength = 24.0f;
constexpr double kChainOnlyToleranceBytes = 1.0 * kMegabyte;

template <std::size_t N>
PeakLine stereo_preset_line(sonare::mastering::api::Preset preset, const float (&lengths)[N]) {
  return measure(lengths, stereo_input, [&](const std::vector<std::vector<float>>& in) {
    const auto result = sonare::mastering::api::master_audio_stereo(
        preset, in[0].data(), in[1].data(), in[0].size(), kSampleRate);
    REQUIRE(result.left.size() == in[0].size());
    return 2.0 * static_cast<double>(in[0].size() * sizeof(float));
  });
}

template <std::size_t N>
PeakLine mono_preset_line(sonare::mastering::api::Preset preset, const float (&lengths)[N]) {
  return measure(
      lengths, [](float seconds) { return fixture(seconds, 1.0f, 1u); },
      [&](const std::vector<float>& in) {
        const auto result =
            sonare::mastering::api::master_audio_mono(preset, in.data(), in.size(), kSampleRate);
        REQUIRE(result.samples.size() == in.size());
        return static_cast<double>(in.size() * sizeof(float));
      });
}

}  // namespace

TEST_CASE("an exciter preset and a repair preset hold a bounded heap per input byte",
          "[mastering][memory]") {
  if (!kLiveBytesMeasured) SKIP("live heap bytes are not measured on this platform");
  for (const char* name : {"pop", "fieldRecording"}) {
    CAPTURE(name);
    const PeakLine line =
        stereo_preset_line(sonare::mastering::api::preset_from_string(name), kShortPresetLengths);
    CAPTURE(line.slope, line.fixed_bytes / kMegabyte);
    CHECK(line.slope <= preset_slope_bound(name));
    CHECK(line.fixed_bytes <= kFixedBoundBytes);
  }
}

TEST_CASE("mastering presets hold a bounded heap per input byte", "[mastering][memory][.][slow]") {
  if (!kLiveBytesMeasured) SKIP("live heap bytes are not measured on this platform");
  using namespace sonare::mastering::api;
  PeakLine pop_stereo{};
  std::vector<std::string> lined;
  for (const LinedPreset& shape : kLinedPresets) {
    CAPTURE(shape.name);
    lined.emplace_back(shape.name);
    const auto preset = preset_from_string(shape.name);
    for (const bool stereo : {true, false}) {
      CAPTURE(stereo);
      const PeakLine line = stereo ? stereo_preset_line(preset, shape.lengths)
                                   : mono_preset_line(preset, shape.lengths);
      CAPTURE(line.slope, line.fixed_bytes / kMegabyte, line.first_slope, line.last_slope);
      CHECK(std::abs(line.first_slope - line.last_slope) <=
            kStraightLineTolerance * std::max(line.first_slope, line.last_slope));
      CHECK(line.slope * line.longest_input > line.fixed_bytes);
      CHECK(line.slope <= shape.slope_bound);
      CHECK(line.fixed_bytes <= kFixedBoundBytes);
      if (stereo && std::string(shape.name) == "pop") pop_stereo = line;
    }
  }
  REQUIRE(pop_stereo.slope > 0.0);
  for (const auto& name : preset_names()) {
    if (std::find(lined.begin(), lined.end(), name) != lined.end()) continue;
    CAPTURE(name);
    const auto input = stereo_input(kChainOnlyLength);
    PeakLiveBytesGuard guard;
    const auto result = master_audio_stereo(preset_from_string(name), input[0].data(),
                                            input[1].data(), input[0].size(), kSampleRate);
    const double peak = static_cast<double>(guard.peak());
    REQUIRE(result.left.size() == input[0].size());
    const double bytes = 2.0 * static_cast<double>(input[0].size() * sizeof(float));
    const double on_pop_line = pop_stereo.fixed_bytes + pop_stereo.slope * bytes;
    CAPTURE(peak / kMegabyte, on_pop_line / kMegabyte);
    CHECK(peak <= on_pop_line + kChainOnlyToleranceBytes);
  }
}

TEST_CASE("repair one-shots hold a bounded heap per input byte", "[mastering][repair][memory]") {
  if (!kLiveBytesMeasured) SKIP("live heap bytes are not measured on this platform");
  namespace repair = sonare::mastering::repair;
  struct Case {
    const char* name;
    double slope_bound;
    std::function<void(const Audio&)> mono;
    std::function<void(const Audio&, const Audio&)> stereo;
  };
  repair::DereverbClassicalConfig wpe;
  wpe.wpe_enabled = true;
  const std::vector<Case> cases = {
      {"dereverb", 6.5, [](const Audio& a) { (void)repair::dereverb_classical(a); },
       [](const Audio& l, const Audio& r) { (void)repair::dereverb_classical_stereo(l, r); }},
      {"dereverb_wpe", 6.5, [&](const Audio& a) { (void)repair::dereverb_classical(a, wpe); },
       [&](const Audio& l, const Audio& r) { (void)repair::dereverb_classical_stereo(l, r, wpe); }},
      {"denoise", 1.25, [](const Audio& a) { (void)repair::denoise_classical(a); },
       [](const Audio& l, const Audio& r) { (void)repair::denoise_classical_stereo(l, r); }},
      {"declick", 6.5, [](const Audio& a) { (void)repair::declick(a); },
       [](const Audio& l, const Audio& r) { (void)repair::declick_stereo(l, r); }},
      {"dehum", 2.0, [](const Audio& a) { (void)repair::dehum(a); },
       [](const Audio& l, const Audio& r) { (void)repair::dehum_stereo(l, r); }},
      {"decrackle", 3.0, [](const Audio& a) { (void)repair::decrackle(a); },
       [](const Audio& l, const Audio& r) { (void)repair::decrackle_stereo(l, r); }},
      {"declip", 1.25, [](const Audio& a) { (void)repair::declip(a); },
       [](const Audio& l, const Audio& r) { (void)repair::declip_stereo(l, r); }},
  };
  for (const auto& c : cases) {
    CAPTURE(c.name);
    const PeakLine mono = measure(
        kRepairLengths,
        [](float seconds) { return Audio::from_vector(fixture(seconds, 1.0f, 1u), kSampleRate); },
        [&](const Audio& audio) {
          c.mono(audio);
          return static_cast<double>(audio.size() * sizeof(float));
        });
    const PeakLine stereo =
        measure(kRepairLengths, stereo_audio, [&](const std::vector<Audio>& in) {
          c.stereo(in[0], in[1]);
          return 2.0 * static_cast<double>(in[0].size() * sizeof(float));
        });
    CAPTURE(mono.slope, mono.fixed_bytes / kMegabyte, stereo.slope, stereo.fixed_bytes / kMegabyte);
    CHECK(mono.slope <= c.slope_bound);
    CHECK(stereo.slope <= c.slope_bound);
    CHECK(mono.fixed_bytes <= kFixedBoundBytes);
    CHECK(stereo.fixed_bytes <= kFixedBoundBytes);
  }
}
