#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/api/presets.h"
#include "support/alloc_guard.h"
#include "util/constants.h"

// Peak live heap of an offline call, measured at two input lengths so the part that grows with the
// input (bytes held per input byte) and the part that does not are bounded separately. A preset's
// lengths both exceed the 64k-sample offline block, so every processor is prepared at one size.

namespace {

using sonare::test::kLiveBytesMeasured;
using sonare::test::PeakLiveBytesGuard;

constexpr int kSampleRate = 44100;
constexpr float kPresetLengths[2] = {2.0f, 4.0f};
// The shortest pair that still clears the 64k-sample block at this rate.
constexpr float kShortPresetLengths[2] = {1.6f, 2.2f};
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
  double slope;        // bytes held per input byte
  double fixed_bytes;  // bytes held independently of the input length
};

/// @p make_input builds the input for a length in seconds outside the measurement; @p run takes it
/// and returns its size in bytes. The peak covers the whole call, its result included.
template <typename MakeInput, typename Run>
PeakLine measure(const float (&lengths)[2], MakeInput make_input, Run run) {
  double peaks[2] = {};
  double inputs[2] = {};
  for (int i = 0; i < 2; ++i) {
    const auto input = make_input(lengths[i]);
    PeakLiveBytesGuard guard;
    inputs[i] = run(input);
    peaks[i] = static_cast<double>(guard.peak());
  }
  REQUIRE(peaks[0] > 0.0);
  const double slope = (peaks[1] - peaks[0]) / (inputs[1] - inputs[0]);
  return {slope, peaks[0] - slope * inputs[0]};
}

std::vector<std::vector<float>> stereo_input(float seconds) {
  return {fixture(seconds, 1.0f, 1u), fixture(seconds, 0.83f, 2u)};
}

/// Bytes held per input byte, mono or stereo, measured plus headroom. A preset not named here runs
/// no stage holding a whole-input spectrum and measures 1.0 (the result and the chain's copies).
/// aiMusic's dereverberation peak sits below its chain's fixed part at these lengths.
double preset_slope_bound(const std::string& name) {
  if (name == "aiMusic" || name == "fieldRecording" || name == "voiceMemo") return 13.0;  // 12.03
  if (name == "vinyl" || name == "shellac78") return 8.75;                                // 8.0
  if (name == "tapeHiss") return 4.5;                                                     // 4.0
  return 1.25;
}

PeakLine stereo_preset_line(sonare::mastering::api::Preset preset, const float (&lengths)[2]) {
  return measure(lengths, stereo_input, [&](const std::vector<std::vector<float>>& in) {
    const auto result = sonare::mastering::api::master_audio_stereo(
        preset, in[0].data(), in[1].data(), in[0].size(), kSampleRate);
    REQUIRE(result.left.size() == in[0].size());
    return 2.0 * static_cast<double>(in[0].size() * sizeof(float));
  });
}

PeakLine mono_preset_line(sonare::mastering::api::Preset preset, const float (&lengths)[2]) {
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
  for (const auto& name : preset_names()) {
    CAPTURE(name);
    const auto preset = preset_from_string(name);
    const PeakLine stereo = stereo_preset_line(preset, kPresetLengths);
    const PeakLine mono = mono_preset_line(preset, kPresetLengths);
    CAPTURE(stereo.slope, stereo.fixed_bytes / kMegabyte, mono.slope, mono.fixed_bytes / kMegabyte);
    CHECK(stereo.slope <= preset_slope_bound(name));
    CHECK(mono.slope <= preset_slope_bound(name));
    CHECK(stereo.fixed_bytes <= kFixedBoundBytes);
    CHECK(mono.fixed_bytes <= kFixedBoundBytes);
  }
}
