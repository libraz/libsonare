#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
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

namespace {

/// Left output of a stereo-speaker renderer fed a 1 kHz cosine at 0.5, with
/// `output_limiter.enabled` set to @p after from @p switch_block on.
std::vector<float> limiter_render(bool before, bool after, int switch_block, int blocks) {
  constexpr int kRate = 48000;
  constexpr int kFrames = 512;
  RendererConfig config;
  config.prepare.input_layout = InputLayout::Stereo;
  config.prepare.target_kind = TargetKind::Speakers;
  config.prepare.target_layout = ChannelLayout::Stereo;
  config.realtime.night_amount = 0.0f;
  config.realtime.limiter_ceiling_db = -12.0f;
  config.realtime.limiter_enabled = before;
  PlaybackRenderer renderer(config, nullptr, kRate, kFrames);
  std::vector<float> l(kFrames), r(kFrames), out_l(kFrames), out_r(kFrames), result;
  const float* in[2] = {l.data(), r.data()};
  float* out[2] = {out_l.data(), out_r.data()};
  for (int b = 0; b < blocks; ++b) {
    if (b == switch_block) {
      config.realtime.limiter_enabled = after;
      renderer.set_config(config);
    }
    for (int i = 0; i < kFrames; ++i) {
      const double t = static_cast<double>(b * kFrames + i) / kRate;
      l[static_cast<size_t>(i)] =
          static_cast<float>(0.5 * std::cos(sonare::constants::kTwoPiD * 1000.0 * t));
      r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
    }
    REQUIRE(renderer.process_planar(in, 2, out, 2, kFrames));
    result.insert(result.end(), out_l.begin(), out_l.end());
  }
  return result;
}

float max_step(const std::vector<float>& x, size_t begin, size_t end) {
  float step = 0.0f;
  for (size_t i = begin + 1; i < end; ++i) step = std::max(step, std::abs(x[i] - x[i - 1]));
  return step;
}

}  // namespace

TEST_CASE("toggling the output limiter crossfades between the limited and bypass output",
          "[playback][limiter]") {
  constexpr int kSwitchBlock = 20;
  constexpr int kBlocks = 26;
  constexpr size_t kSwitch = static_cast<size_t>(kSwitchBlock) * 512;
  const auto glide = static_cast<size_t>(std::lround(kOutputLimiterSwitchMs * 1e-3 * 48000));
  const std::vector<float> on = limiter_render(true, true, -1, kBlocks);
  const std::vector<float> off = limiter_render(false, false, -1, kBlocks);
  const size_t begin = kSwitch - 2000;
  const size_t end = kSwitch + 2000;
  // Neither steady state steps by more than the larger of the two signals' own steps.
  const float bound = 1.5f * std::max(max_step(on, begin, end), max_step(off, begin, end));
  for (const bool enable : {false, true}) {
    INFO((enable ? "off -> on" : "on -> off"));
    const std::vector<float> toggled = limiter_render(!enable, enable, kSwitchBlock, kBlocks);
    CHECK(max_step(toggled, begin, end) <= bound);
    // After the crossfade the output is the destination state's output exactly.
    const std::vector<float>& settled = enable ? on : off;
    for (size_t i = kSwitch + glide; i < toggled.size(); ++i) {
      INFO("sample " << i);
      REQUIRE(toggled[i] == settled[i]);
    }
    // Before it, nothing moved.
    const std::vector<float>& before = enable ? off : on;
    CHECK(std::equal(toggled.begin(), toggled.begin() + static_cast<std::ptrdiff_t>(kSwitch),
                     before.begin()));
  }
}

TEST_CASE("offline rendering keeps the opening of speakers that distance compensation delays",
          "[playback][latency]") {
  constexpr int kRate = 48000;
  constexpr size_t kFrames = 4800;
  constexpr size_t kBurst = 480;
  constexpr float kAmplitude = 0.1f;
  constexpr float kOnset = 0.005f;
  const double expected_delay = 4.0 / sonare::constants::kSoundSpeedMps * kRate;

  for (const bool with_distance : {false, true}) {
    for (const size_t burst_at : {size_t{0}, size_t{1000}}) {
      INFO((with_distance ? "L 1 m / R 5 m" : "no distance") << ", burst at " << burst_at);
      RendererConfig config;
      config.prepare.input_layout = InputLayout::Stereo;
      config.prepare.target_kind = TargetKind::Speakers;
      config.prepare.target_layout = ChannelLayout::Stereo;
      config.realtime.limiter_enabled = false;
      if (with_distance) {
        config.prepare.speakers[static_cast<size_t>(SpeakerRole::L)].has_distance = true;
        config.prepare.speakers[static_cast<size_t>(SpeakerRole::L)].distance_m = 1.0f;
        config.prepare.speakers[static_cast<size_t>(SpeakerRole::R)].has_distance = true;
        config.prepare.speakers[static_cast<size_t>(SpeakerRole::R)].distance_m = 5.0f;
      }
      std::vector<float> in(kFrames * 2, 0.0f);
      for (size_t i = 0; i < kBurst; ++i) {
        const float s = kAmplitude * std::sin(sonare::constants::kTwoPi * 1000.0f *
                                              static_cast<float>(i) / static_cast<float>(kRate));
        in[(burst_at + i) * 2] = s;
        in[(burst_at + i) * 2 + 1] = s;
      }
      int channels = 0;
      const std::vector<float> out =
          render_interleaved(in.data(), kFrames, 2, kRate, config, nullptr, &channels);
      REQUIRE(channels == 2);

      // 480 samples of a 1 kHz sine at 0.1: energy 2.4 per plane.
      const double input_energy = kBurst * kAmplitude * kAmplitude / 2.0;
      double energy[2] = {};
      int onset[2] = {-1, -1};
      for (size_t i = 0; i < kFrames; ++i) {
        for (size_t c = 0; c < 2; ++c) {
          const float s = out[i * 2 + c];
          energy[c] += static_cast<double>(s) * s;
          if (onset[c] < 0 && std::abs(s) > kOnset) onset[c] = static_cast<int>(i);
        }
      }
      for (size_t c = 0; c < 2; ++c) {
        INFO("plane " << c);
        CHECK(std::abs(energy[c] - input_energy) <= 1e-3 * input_energy);
      }
      // The far speaker plays at the input's own position; the near one waits for it.
      CHECK(std::abs(onset[1] - static_cast<int>(burst_at)) <= 2);
      const double delay = with_distance ? expected_delay : 0.0;
      CHECK(std::abs(onset[0] - onset[1] - delay) <= 2.0);
    }
  }
}

TEST_CASE("a dialogue level change ramps the centre plane linearly across one block",
          "[playback][latency]") {
  constexpr int kFrames = 512;
  constexpr int kChannels = 6;
  constexpr int kCentre = 2;
  constexpr float kLevel = 0.25f;
  RendererConfig config;
  config.prepare.input_layout = InputLayout::FivePointOne;
  config.prepare.target_kind = TargetKind::Speakers;
  config.prepare.target_layout = ChannelLayout::FivePointOne;
  config.realtime.limiter_enabled = false;
  PlaybackRenderer renderer(config, nullptr, 48000, kFrames);
  std::vector<float> in(static_cast<size_t>(kFrames * kChannels), kLevel);
  std::vector<float> out(in.size(), 0.0f);
  std::vector<std::vector<float>> planes(kChannels);
  constexpr int kSwitchBlock = 8;
  for (int b = 0; b < 16; ++b) {
    if (b == kSwitchBlock) {
      config.realtime.dialogue_level_db = 6.0f;
      renderer.set_config(config);
    }
    REQUIRE(renderer.process_interleaved(in.data(), kChannels, out.data(), kChannels, kFrames));
    for (int i = 0; i < kFrames; ++i) {
      for (int c = 0; c < kChannels; ++c) {
        planes[static_cast<size_t>(c)].push_back(out[static_cast<size_t>(i * kChannels + c)]);
      }
    }
  }

  const float gain = std::pow(10.0f, 6.0f / 20.0f);
  const std::vector<float>& centre = planes[kCentre];
  // The input is constant, so the ramp shows wherever the gain stage's own delay puts it.
  auto ramp_start = static_cast<size_t>(kSwitchBlock * kFrames);
  REQUIRE(centre[ramp_start] == kLevel);
  while (ramp_start < centre.size() && centre[ramp_start] == kLevel) ++ramp_start;
  REQUIRE(ramp_start + kFrames < centre.size());
  for (int k = 0; k < kFrames; ++k) {
    INFO("ramp sample " << k);
    const float expected = kLevel + kLevel * (gain - 1.0f) * static_cast<float>(k + 1) / kFrames;
    CHECK(std::abs(centre[ramp_start + static_cast<size_t>(k)] - expected) <= 1e-5f);
  }
  CHECK(centre.back() == kLevel * gain);
  // The other planes carry no dialogue level and stay untouched.
  for (int c = 0; c < kChannels; ++c) {
    if (c == kCentre) continue;
    INFO("plane " << c);
    CHECK(planes[static_cast<size_t>(c)].back() == kLevel);
  }
}
