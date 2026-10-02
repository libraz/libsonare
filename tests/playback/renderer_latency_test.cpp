#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "playback/hrtf_set.h"
#include "playback/renderer.h"
#include "playback/shrf_fixture.h"
#include "util/constants.h"

using sonare::ChannelLayout;
using sonare::SpeakerRole;
using namespace sonare::playback;

TEST_CASE("playback limiter diagnostics include the independent LFE limiter",
          "[playback][limiter]") {
  for (const InputLayout layout : {InputLayout::FivePointOne, InputLayout::SevenPointOne}) {
    RendererConfig config;
    config.prepare.input_layout = layout;
    config.prepare.target_kind = TargetKind::Speakers;
    config.prepare.target_layout = to_channel_layout(layout);
    config.prepare.bass_management.enabled = false;
    config.realtime.limiter_ceiling_db = -6.0f;
    constexpr int kFrames = 256;
    const int channels = sonare::channel_count(config.prepare.target_layout);
    for (const int driven_plane : {0, 3}) {
      INFO("channels " << channels << ", driven plane " << driven_plane);
      PlaybackRenderer renderer(config, nullptr, 48000, kFrames);
      std::vector<float> input(static_cast<size_t>(channels * kFrames), 0.0f);
      std::vector<float> output(input.size(), 0.0f);
      for (int i = 0; i < kFrames; ++i) {
        input[static_cast<size_t>(i * channels + driven_plane)] = 1.0f;
      }
      for (int block = 0; block < 32; ++block) {
        REQUIRE(
            renderer.process_interleaved(input.data(), channels, output.data(), channels, kFrames));
      }
      const float amplitude = output[static_cast<size_t>((kFrames - 1) * channels + driven_plane)];
      REQUIRE(amplitude > 0.0f);
      REQUIRE(amplitude <= std::pow(10.0f, -6.0f / 20.0f) + 1e-5f);
      const float reduction = renderer.diagnostics().limiter_gain_reduction_db;
      INFO("limited amplitude " << amplitude << ", gain reduction " << reduction);
      CHECK(reduction < -5.0f);
    }
  }
}

namespace {

constexpr int kBlock = 256;
constexpr int kImpulseAt = 300;
constexpr float kImpulse = 0.1f;
/// Onset threshold: -40 dB of the peak.
constexpr float kOnsetRatio = 0.01f;

struct Target {
  TargetKind kind;
  ChannelLayout layout;
  const char* name;
};

constexpr Target kTargets[] = {
    {TargetKind::Speakers, ChannelLayout::Stereo, "stereo speakers"},
    {TargetKind::Speakers, ChannelLayout::FivePointOne, "5.1 speakers"},
    {TargetKind::Speakers, ChannelLayout::SevenPointOne, "7.1 speakers"},
    {TargetKind::Headphones, ChannelLayout::Stereo, "headphones"},
};

constexpr InputLayout kInputs[] = {InputLayout::Mono, InputLayout::Stereo,
                                   InputLayout::FivePointOne, InputLayout::SevenPointOne};

RendererConfig config_for(InputLayout input, const Target& target) {
  RendererConfig config;
  config.prepare.input_layout = input;
  config.prepare.target_kind = target.kind;
  if (target.kind == TargetKind::Speakers) config.prepare.target_layout = target.layout;
  return config;
}

const HrtfSet& fixture_hrtf() {
  static const HrtfSet set = [] {
    const std::vector<uint8_t> bytes = test::make_shrf_fixture();
    return HrtfSet::from_memory(bytes.data(), bytes.size());
  }();
  return set;
}

int input_channels(InputLayout input) { return sonare::channel_count(to_channel_layout(input)); }

/// Feeds silence for @p lead_frames (letting any ramp settle), then an impulse
/// on every input plane at kImpulseAt; returns the output after the lead.
std::vector<std::vector<float>> impulse_response(PlaybackRenderer& renderer, int in_channels,
                                                 int lead_frames, int frames) {
  const int out_count = renderer.output_channel_count();
  std::vector<std::vector<float>> in(static_cast<size_t>(in_channels),
                                     std::vector<float>(kBlock, 0.0f));
  std::vector<std::vector<float>> out(static_cast<size_t>(out_count),
                                      std::vector<float>(static_cast<size_t>(frames), 0.0f));
  std::vector<const float*> in_ptrs(static_cast<size_t>(in_channels));
  std::vector<float*> out_ptrs(static_cast<size_t>(out_count));
  std::vector<std::vector<float>> scratch(static_cast<size_t>(out_count),
                                          std::vector<float>(kBlock, 0.0f));
  for (int ch = 0; ch < in_channels; ++ch) in_ptrs[static_cast<size_t>(ch)] = in[ch].data();
  for (int ch = 0; ch < out_count; ++ch) out_ptrs[static_cast<size_t>(ch)] = scratch[ch].data();
  for (int start = -lead_frames; start < frames; start += kBlock) {
    for (int ch = 0; ch < in_channels; ++ch) {
      std::fill(in[static_cast<size_t>(ch)].begin(), in[static_cast<size_t>(ch)].end(), 0.0f);
      if (kImpulseAt >= start && kImpulseAt < start + kBlock) {
        in[static_cast<size_t>(ch)][static_cast<size_t>(kImpulseAt - start)] = kImpulse;
      }
    }
    REQUIRE(
        renderer.process_planar(in_ptrs.data(), in_channels, out_ptrs.data(), out_count, kBlock));
    for (int ch = 0; ch < out_count; ++ch) {
      for (int i = 0; i < kBlock; ++i) {
        const int t = start + i;
        if (t >= 0 && t < frames) {
          out[static_cast<size_t>(ch)][static_cast<size_t>(t)] =
              scratch[static_cast<size_t>(ch)][static_cast<size_t>(i)];
        }
      }
    }
  }
  return out;
}

float peak_of(const std::vector<float>& plane) {
  float peak = 0.0f;
  for (float s : plane) peak = std::max(peak, std::abs(s));
  return peak;
}

/// Onset delays (onset - impulse position) of every plane that carries the
/// impulse, i.e. whose peak clears -40 dB of the loudest plane. -1 marks a
/// plane without it.
std::vector<int> onset_delays(const std::vector<std::vector<float>>& out) {
  float global = 0.0f;
  for (const auto& plane : out) global = std::max(global, peak_of(plane));
  const float threshold = global * kOnsetRatio;
  std::vector<int> delays;
  for (const auto& plane : out) {
    int onset = -1;
    if (peak_of(plane) > threshold) {
      for (size_t i = 0; i < plane.size(); ++i) {
        if (std::abs(plane[i]) > threshold) {
          onset = static_cast<int>(i) - kImpulseAt;
          break;
        }
      }
    }
    delays.push_back(onset);
  }
  return delays;
}

void check_onsets(const std::vector<int>& delays, int latency) {
  int carried = 0;
  for (size_t p = 0; p < delays.size(); ++p) {
    if (delays[p] < 0) continue;
    INFO("plane " << p);
    ++carried;
    CHECK(std::abs(delays[p] - latency) <= 1);
  }
  CHECK(carried > 0);
}

}  // namespace

TEST_CASE("latency depends on the target only", "[playback][latency]") {
  for (int rate : {48000, 44100}) {
    for (const Target& target : kTargets) {
      const int stereo_speakers = rate == 48000 ? 240 + 48 : 221 + 44;
      const int expected =
          target.kind == TargetKind::Speakers && target.layout == ChannelLayout::Stereo
              ? stereo_speakers
              : 1024 + stereo_speakers;
      for (InputLayout input : kInputs) {
        INFO(target.name << " from " << input_layout_name(input) << " at " << rate);
        PlaybackRenderer renderer(config_for(input, target), &fixture_hrtf(), rate, 512);
        CHECK(renderer.latency_samples() == expected);
      }
      PlaybackRenderer automatic(config_for(InputLayout::Auto, target), &fixture_hrtf(), rate, 512);
      CHECK(automatic.latency_samples() == expected);
    }
  }
}

TEST_CASE("every output plane starts at the reported latency", "[playback][latency]") {
  for (const Target& target : kTargets) {
    for (InputLayout input : kInputs) {
      INFO(target.name << " from " << input_layout_name(input));
      PlaybackRenderer renderer(config_for(input, target), &fixture_hrtf(), 48000, kBlock);
      const int latency = renderer.latency_samples();
      const auto out = impulse_response(renderer, input_channels(input), 0, latency + 1024);
      check_onsets(onset_delays(out), latency);
    }
  }
}

TEST_CASE("realtime changes move neither the reported nor the measured latency",
          "[playback][latency]") {
  const std::vector<std::pair<const char*, std::function<void(RealtimeConfig&)>>> toggles = {
      {"upmix off", [](RealtimeConfig& rt) { rt.upmix_enabled = false; }},
      {"night 1", [](RealtimeConfig& rt) { rt.night_amount = 1.0f; }},
      {"limiter off", [](RealtimeConfig& rt) { rt.limiter_enabled = false; }},
      {"room off", [](RealtimeConfig& rt) { rt.room_enabled = false; }},
      {"dialogue +6", [](RealtimeConfig& rt) { rt.dialogue_level_db = 6.0f; }},
  };
  const Target targets[] = {kTargets[1], kTargets[3]};
  for (const Target& target : targets) {
    for (InputLayout input : {InputLayout::Stereo, InputLayout::FivePointOne}) {
      const RendererConfig base = config_for(input, target);
      PlaybackRenderer renderer(base, &fixture_hrtf(), 48000, kBlock);
      const int latency = renderer.latency_samples();
      for (const auto& toggle : toggles) {
        INFO(target.name << " from " << input_layout_name(input) << ": " << toggle.first);
        for (bool on : {true, false}) {
          RendererConfig changed = base;
          if (on) toggle.second(changed.realtime);
          renderer.set_config(changed);
          renderer.reset();
          const auto out = impulse_response(renderer, input_channels(input), 2048, latency + 512);
          CHECK(renderer.latency_samples() == latency);
          check_onsets(onset_delays(out), latency);
        }
      }
    }
  }
}

TEST_CASE("the room preset does not move the latency", "[playback][latency]") {
  for (RoomPreset preset : {RoomPreset::None, RoomPreset::LivingRoom, RoomPreset::ScreeningRoom}) {
    RendererConfig config = config_for(InputLayout::FivePointOne, kTargets[3]);
    config.prepare.room_preset = preset;
    PlaybackRenderer renderer(config, &fixture_hrtf(), 48000, kBlock);
    CHECK(renderer.latency_samples() == 1312);
    check_onsets(onset_delays(impulse_response(renderer, 6, 0, 1312 + 512)), 1312);
  }
}

TEST_CASE("distance compensation adds the largest distance delay", "[playback][latency]") {
  RendererConfig config = config_for(InputLayout::FivePointOne, kTargets[1]);
  for (SpeakerRole role :
       {SpeakerRole::L, SpeakerRole::R, SpeakerRole::C, SpeakerRole::Ls, SpeakerRole::Rs}) {
    SpeakerPrepare& speaker = config.prepare.speakers[static_cast<size_t>(role)];
    speaker.has_distance = true;
    speaker.distance_m = role == SpeakerRole::L ? 2.0f : 3.0f;
  }
  PlaybackRenderer renderer(config, &fixture_hrtf(), 48000, kBlock);
  const int distance =
      static_cast<int>(std::lround(1.0 / sonare::constants::kSoundSpeedMps * 48000.0));
  CHECK(renderer.latency_samples() == 1312 + distance);
  const auto delays = onset_delays(impulse_response(renderer, 6, 0, 1312 + distance + 512));
  // The nearest speaker waits for the farthest; the others play at the base latency.
  CHECK(std::abs(delays[0] - renderer.latency_samples()) <= 1);
  for (size_t p = 1; p < delays.size(); ++p) CHECK(std::abs(delays[p] - 1312) <= 1);
}
