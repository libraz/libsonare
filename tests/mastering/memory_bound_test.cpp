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

std::vector<Audio> stereo_audio(float seconds) {
  return {Audio::from_vector(fixture(seconds, 1.0f, 1u), kSampleRate),
          Audio::from_vector(fixture(seconds, 0.83f, 2u), kSampleRate)};
}

/// Bytes held per input byte, mono or stereo, measured plus headroom at the lengths below. A preset
/// not named here holds the result beside one whole-input copy in the chain and measures 2.0.
double preset_slope_bound(const std::string& name) {
  if (name == "aiMusic" || name == "fieldRecording" || name == "voiceMemo" || name == "vinyl" ||
      name == "shellac78" || name == "audiobook") {
    return 8.75;  // 8.01
  }
  if (name == "tapeHiss" || name == "speech" || name == "podcast") return 3.5;  // 3.0
  return 2.25;
}

/// The chain's fixed state and a whole-input stage peak at different times, so where the fixed part
/// is the larger the slope reads the wrong line and a regression in that stage passes. The
/// mastering chain carries about 10 MB of fixed state: a preset whose only whole-input holder is
/// the chain passes it beyond about 23 s, one with a spectrum stage much sooner. The case checks it
/// is passed.
const float (&preset_lengths(const std::string& name))[2] {
  static constexpr float kNoChainState[2] = {10.0f, 20.0f};
  static constexpr float kChainState[2] = {24.0f, 48.0f};
  const bool short_enough = name == "aiMusic" || name == "fieldRecording" || name == "voiceMemo" ||
                            name == "vinyl" || name == "shellac78" || name == "tapeHiss";
  return short_enough ? kNoChainState : kChainState;
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
    const auto& lengths = preset_lengths(name);
    const PeakLine stereo = stereo_preset_line(preset, lengths);
    const PeakLine mono = mono_preset_line(preset, lengths);
    CAPTURE(stereo.slope, stereo.fixed_bytes / kMegabyte, mono.slope, mono.fixed_bytes / kMegabyte);
    // The slope is only a measurement where the variable part dominates the longer run.
    const double long_frames = static_cast<double>(kSampleRate) * lengths[1];
    CHECK(stereo.slope * 2.0 * long_frames * sizeof(float) > stereo.fixed_bytes);
    CHECK(mono.slope * long_frames * sizeof(float) > mono.fixed_bytes);
    CHECK(stereo.slope <= preset_slope_bound(name));
    CHECK(mono.slope <= preset_slope_bound(name));
    CHECK(stereo.fixed_bytes <= kFixedBoundBytes);
    CHECK(mono.fixed_bytes <= kFixedBoundBytes);
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
