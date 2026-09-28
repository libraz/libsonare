#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "playback/front_end.h"
#include "playback/layout_convert.h"
#include "playback/renderer.h"
#include "util/constants.h"
#include "util/exception.h"

using sonare::ChannelLayout;
using namespace sonare::playback;

namespace {

constexpr int kRate = 48000;
constexpr int kBlock = 480;
constexpr int kWindow20Ms = 960;
constexpr int kWindow5Ms = 240;
constexpr int kAround200Ms = 9600;

using Planes = std::vector<std::vector<float>>;

/// A run of blocks with one input channel count.
struct Segment {
  int channels;
  int blocks;
};

/// Input sample of channel @p ch (of @p channels) at absolute frame @p n.
using Source = std::function<float(int channels, int ch, int n)>;

int total_frames(const std::vector<Segment>& segments) {
  int frames = 0;
  for (const Segment& s : segments) frames += s.blocks * kBlock;
  return frames;
}

Planes run(PlaybackRenderer& renderer, const std::vector<Segment>& segments, const Source& source) {
  const int out_count = renderer.output_channel_count();
  const int total = total_frames(segments);
  Planes out(static_cast<size_t>(out_count), std::vector<float>(static_cast<size_t>(total), 0.0f));
  Planes in(8, std::vector<float>(kBlock, 0.0f));
  const float* in_ptrs[8];
  for (int ch = 0; ch < 8; ++ch) in_ptrs[ch] = in[static_cast<size_t>(ch)].data();
  std::vector<float*> out_ptrs(static_cast<size_t>(out_count));
  int n = 0;
  for (const Segment& segment : segments) {
    for (int b = 0; b < segment.blocks; ++b, n += kBlock) {
      for (int ch = 0; ch < segment.channels; ++ch) {
        for (int i = 0; i < kBlock; ++i) {
          in[static_cast<size_t>(ch)][static_cast<size_t>(i)] = source(segment.channels, ch, n + i);
        }
      }
      for (int ch = 0; ch < out_count; ++ch) {
        out_ptrs[static_cast<size_t>(ch)] = out[static_cast<size_t>(ch)].data() + n;
      }
      REQUIRE(
          renderer.process_planar(in_ptrs, segment.channels, out_ptrs.data(), out_count, kBlock));
    }
  }
  return out;
}

float sine(int n) {
  return 0.25f * std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(n) /
                          static_cast<float>(kRate));
}

/// Seeded pink noise at -20 dBFS RMS (Kellet's refined filter).
std::vector<float> pink_noise(size_t frames, uint32_t seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> white(0.0f, 1.0f);
  float b[7] = {};
  std::vector<float> out(frames);
  double power = 0.0;
  for (float& s : out) {
    const float w = white(rng);
    b[0] = 0.99886f * b[0] + w * 0.0555179f;
    b[1] = 0.99332f * b[1] + w * 0.0750759f;
    b[2] = 0.96900f * b[2] + w * 0.1538520f;
    b[3] = 0.86650f * b[3] + w * 0.3104856f;
    b[4] = 0.55000f * b[4] + w * 0.5329522f;
    b[5] = -0.7616f * b[5] - w * 0.0168980f;
    s = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f;
    b[6] = w * 0.115926f;
    power += static_cast<double>(s) * s;
  }
  const double rms = std::sqrt(power / static_cast<double>(frames));
  const float gain = static_cast<float>(0.1 / rms);
  for (float& s : out) s *= gain;
  return out;
}

Planes noise_planes(int channels, size_t frames, uint32_t seed) {
  Planes planes;
  for (int ch = 0; ch < channels; ++ch) {
    planes.push_back(pink_noise(frames, seed + static_cast<uint32_t>(ch)));
  }
  return planes;
}

RendererConfig auto_config(TargetKind kind, ChannelLayout layout) {
  RendererConfig config;
  config.prepare.input_layout = InputLayout::Auto;
  config.prepare.target_kind = kind;
  if (kind == TargetKind::Speakers) config.prepare.target_layout = layout;
  return config;
}

RendererConfig quiet_path(RendererConfig config) {
  config.realtime.upmix_enabled = false;
  config.realtime.night_amount = 0.0f;
  config.realtime.limiter_enabled = false;
  config.realtime.dialogue_level_db = 0.0f;
  return config;
}

double window_energy(const std::vector<float>& plane, int start, int length) {
  double energy = 0.0;
  for (int i = start; i < start + length; ++i) {
    const double s = plane[static_cast<size_t>(i)];
    energy += s * s;
  }
  return energy;
}

double to_db(double energy) { return 10.0 * std::log10(energy + 1e-30); }

/// Onset (first sample over -40 dB of the window peak, over all planes) inside
/// [from, to).
int onset_in(const Planes& out, int from, int to) {
  float peak = 0.0f;
  for (const auto& plane : out) {
    for (int i = from; i < to; ++i) peak = std::max(peak, std::abs(plane[static_cast<size_t>(i)]));
  }
  for (int i = from; i < to; ++i) {
    for (const auto& plane : out) {
      if (std::abs(plane[static_cast<size_t>(i)]) > 0.01f * peak) return i;
    }
  }
  return -1;
}

float max_step(const std::vector<float>& plane, int from, int to) {
  float step = 0.0f;
  for (int i = std::max(from, 1); i < to; ++i) {
    step =
        std::max(step, std::abs(plane[static_cast<size_t>(i)] - plane[static_cast<size_t>(i - 1)]));
  }
  return step;
}

float seamless_error(TargetKind kind, ChannelLayout target, int from_channels, int to_channels,
                     int sine_plane_of_7_1, int out_plane) {
  PlaybackRenderer renderer(quiet_path(auto_config(kind, target)), nullptr, kRate, kBlock);
  const int latency = renderer.latency_samples();
  const std::vector<Segment> segments = {{from_channels, 30}, {to_channels, 30}};
  const Source source = [&](int channels, int ch, int n) {
    // Mono's only channel, or the chosen plane of the wider layout.
    const int plane = channels == 1 ? 0 : (channels == 8 ? sine_plane_of_7_1 : 0);
    return ch == plane ? sine(n) : 0.0f;
  };
  const Planes out = run(renderer, segments, source);
  CHECK(renderer.diagnostics().layout_switches >= 1u);
  float error = 0.0f;
  const auto& plane = out[static_cast<size_t>(out_plane)];
  for (size_t i = 0; i < plane.size(); ++i) {
    const int n = static_cast<int>(i) - latency;
    const float expected = n >= 0 ? sine(n) : 0.0f;
    error = std::max(error, std::abs(plane[i] - expected));
  }
  return error;
}

int drain_frames_of(const RendererConfig& config, ChannelLayout input) {
  FrontEnd front_end;
  front_end.prepare(kRate, kBlock, input, config.prepare, make_output_bus(config.prepare));
  return front_end.drain_frames();
}

Planes fixed_render(RendererConfig config, InputLayout layout, const Planes& signal, int offset,
                    int frames) {
  config.prepare.input_layout = layout;
  PlaybackRenderer renderer(config, nullptr, kRate, kBlock);
  const int channels = static_cast<int>(signal.size());
  const std::vector<Segment> segments = {{channels, frames / kBlock}};
  return run(renderer, segments, [&](int, int ch, int n) {
    return signal[static_cast<size_t>(ch)][static_cast<size_t>(offset + n)];
  });
}

InputLayout fixed_layout(int channels) {
  return channels == 2 ? InputLayout::Stereo : InputLayout::FivePointOne;
}

/// Output of a renderer that switches from the first signal to the second at
/// @p switch_blocks, and of the two references: renderers fixed to each
/// layout, fed that layout's signal from the start (warm at the switch).
struct SwitchRun {
  Planes out;
  Planes ref_from;
  Planes ref_to;
  int switch_at = 0;
  int latency = 0;
  int drain = 0;
};

SwitchRun switch_run(const RendererConfig& config, int from_channels, int to_channels,
                     int switch_blocks, int after_blocks) {
  SwitchRun r;
  r.switch_at = switch_blocks * kBlock;
  const int total = (switch_blocks + after_blocks) * kBlock;
  const Planes from_signal = noise_planes(from_channels, static_cast<size_t>(total), 11u);
  const Planes to_signal = noise_planes(to_channels, static_cast<size_t>(total), 31u);
  PlaybackRenderer renderer(config, nullptr, kRate, kBlock);
  r.latency = renderer.latency_samples();
  r.out = run(renderer, {{from_channels, switch_blocks}, {to_channels, after_blocks}},
              [&](int channels, int ch, int n) {
                const Planes& s = channels == from_channels ? from_signal : to_signal;
                return s[static_cast<size_t>(ch)][static_cast<size_t>(n)];
              });
  r.ref_from = fixed_render(config, fixed_layout(from_channels), from_signal, 0, total);
  r.ref_to = fixed_render(config, fixed_layout(to_channels), to_signal, 0, total);
  r.drain = drain_frames_of(
      config, from_channels == 2 ? ChannelLayout::Stereo : ChannelLayout::FivePointOne);
  return r;
}

/// Seam check: within 200 ms of the audible switch no 20 ms window of the
/// total energy falls 3 dB below the quieter reference. Returns the worst drop.
double seam_drop(TargetKind kind, int from_channels, int to_channels) {
  RendererConfig config = auto_config(kind, ChannelLayout::FivePointOne);
  config.prepare.room_preset = RoomPreset::None;
  config.realtime.upmix_enabled = true;
  config.realtime.night_amount = 0.0f;
  const SwitchRun r = switch_run(config, from_channels, to_channels, 30, 30);
  double worst = -1e9;
  const int centre = r.switch_at + r.latency;
  for (int start = centre - kAround200Ms; start + kWindow20Ms <= centre + kAround200Ms;
       start += kWindow20Ms) {
    double got = 0.0;
    double from = 0.0;
    double to = 0.0;
    for (size_t p = 0; p < r.out.size(); ++p) {
      got += window_energy(r.out[p], start, kWindow20Ms);
      from += window_energy(r.ref_from[p], start, kWindow20Ms);
      to += window_energy(r.ref_to[p], start, kWindow20Ms);
    }
    worst = std::max(worst, to_db(std::min(from, to)) - to_db(got));
  }
  return worst;
}

/// Steady check: from switch + latency + drain_frames + 1.5 s every 20 ms
/// window of every plane is within 1 dB of the reference. Returns the worst
/// deviation.
double steady_deviation(int from_channels, int to_channels) {
  RendererConfig config = auto_config(TargetKind::Headphones, ChannelLayout::Stereo);
  config.prepare.room_preset = RoomPreset::LivingRoom;
  config.realtime.upmix_enabled = true;
  config.realtime.night_amount = 1.0f;
  constexpr int kSettleFrames = 72000;  // 1.5 s
  constexpr int kCheckedWindows = 10;
  const int drain = drain_frames_of(
      config, from_channels == 2 ? ChannelLayout::Stereo : ChannelLayout::FivePointOne);
  const int after_frames = 1312 + drain + kSettleFrames + kCheckedWindows * kWindow20Ms;
  const SwitchRun r = switch_run(config, from_channels, to_channels, 30, after_frames / kBlock + 1);
  double worst = 0.0;
  int windows = 0;
  const int total = static_cast<int>(r.out[0].size());
  for (int start = r.switch_at + r.latency + r.drain + kSettleFrames; start + kWindow20Ms <= total;
       start += kWindow20Ms) {
    ++windows;
    for (size_t p = 0; p < r.out.size(); ++p) {
      const double diff = to_db(window_energy(r.out[p], start, kWindow20Ms)) -
                          to_db(window_energy(r.ref_to[p], start, kWindow20Ms));
      worst = std::max(worst, std::abs(diff));
    }
  }
  CHECK(windows >= kCheckedWindows);
  return worst;
}

}  // namespace

TEST_CASE("a layout switch leaves the sine seamless", "[playback][layout-switch]") {
  const int c_plane = static_cast<int>(sonare::SpeakerRole::C);
  const float stereo_to_51 =
      seamless_error(TargetKind::Speakers, ChannelLayout::FivePointOne, 2, 6, 0, 0);
  const float from_51_to_stereo =
      seamless_error(TargetKind::Speakers, ChannelLayout::FivePointOne, 6, 2, 0, 0);
  const float mono_to_71 =
      seamless_error(TargetKind::Speakers, ChannelLayout::SevenPointOne, 1, 8, c_plane, c_plane);
  const float from_71_to_mono =
      seamless_error(TargetKind::Speakers, ChannelLayout::SevenPointOne, 8, 1, c_plane, c_plane);
  CHECK(stereo_to_51 <= 1e-5f);
  CHECK(from_51_to_stereo <= 1e-5f);
  CHECK(mono_to_71 <= 1e-5f);
  CHECK(from_71_to_mono <= 1e-5f);
}

TEST_CASE("an impulse in every layout section lands at its latency", "[playback][layout-switch]") {
  PlaybackRenderer renderer(auto_config(TargetKind::Speakers, ChannelLayout::FivePointOne), nullptr,
                            kRate, kBlock);
  const int latency = renderer.latency_samples();
  constexpr int kSectionBlocks = 20;
  const std::vector<Segment> segments = {{2, kSectionBlocks}, {6, kSectionBlocks},
                                         {1, kSectionBlocks}, {8, kSectionBlocks},
                                         {2, kSectionBlocks}, {2, kSectionBlocks}};
  std::vector<int> impulses;
  int start = 0;
  for (size_t s = 0; s + 1 < segments.size(); ++s) {
    const int length = segments[s].blocks * kBlock;
    impulses.push_back(start + length / 2);
    impulses.push_back(start + length - 1);  // right before the next switch
    start += length;
  }
  const Planes out = run(renderer, segments, [&](int, int ch, int n) {
    return ch == 0 && std::find(impulses.begin(), impulses.end(), n) != impulses.end() ? 0.3f
                                                                                       : 0.0f;
  });
  CHECK(renderer.diagnostics().layout_switches == 4u);
  for (int position : impulses) {
    INFO("impulse at " << position);
    const int onset = onset_in(out, position + latency - 400, position + latency + 400);
    CHECK(std::abs(onset - (position + latency)) <= 1);
  }
}

TEST_CASE("switching stereo and 5.1 leaves no gap on 5.1 speakers", "[playback][layout-switch]") {
  const double up = seam_drop(TargetKind::Speakers, 2, 6);
  const double down = seam_drop(TargetKind::Speakers, 6, 2);
  CHECK(up < 3.0);
  CHECK(down < 3.0);
}

TEST_CASE("switching stereo and 5.1 leaves no gap on headphones", "[playback][layout-switch]") {
  const double up = seam_drop(TargetKind::Headphones, 2, 6);
  const double down = seam_drop(TargetKind::Headphones, 6, 2);
  CHECK(up < 3.0);
  CHECK(down < 3.0);
}

TEST_CASE("after a switch the headphone output settles onto the fixed layout",
          "[playback][layout-switch][.][slow]") {
  const double up = steady_deviation(2, 6);
  const double down = steady_deviation(6, 2);
  CHECK(up <= 1.0);
  CHECK(down <= 1.0);
}

TEST_CASE("a switch back inside the drain truncates it without a click",
          "[playback][layout-switch]") {
  const RendererConfig config = auto_config(TargetKind::Speakers, ChannelLayout::FivePointOne);
  const int stereo_drain = drain_frames_of(config, ChannelLayout::Stereo);
  constexpr int kShortBlocks = 10;
  REQUIRE(kShortBlocks * kBlock < stereo_drain);
  const std::vector<Segment> segments = {{2, 30}, {6, kShortBlocks}, {2, 50}};
  const int last_start = (30 + kShortBlocks) * kBlock;

  // Timing after the truncation.
  {
    PlaybackRenderer renderer(config, nullptr, kRate, kBlock);
    const int latency = renderer.latency_samples();
    const int impulse = last_start + 20 * kBlock;
    const Planes out = run(renderer, segments, [&](int, int ch, int n) {
      return ch == 0 && n == impulse ? 0.3f : 0.0f;
    });
    const RendererDiagnostics d = renderer.diagnostics();
    CHECK(d.layout_switches == 2u);
    CHECK(d.truncated_drains == 1u);
    bool finite = true;
    for (const auto& plane : out) {
      for (float s : plane) finite = finite && std::isfinite(s);
    }
    CHECK(finite);
    const int onset = onset_in(out, impulse + latency - 400, impulse + latency + 400);
    CHECK(std::abs(onset - (impulse + latency)) <= 1);
  }

  // The same switches under a continuous sine: no step beyond the steady one.
  {
    PlaybackRenderer renderer(config, nullptr, kRate, kBlock);
    const int latency = renderer.latency_samples();
    const Planes out =
        run(renderer, segments, [](int, int ch, int n) { return ch == 0 ? sine(n) : 0.0f; });
    CHECK(renderer.diagnostics().truncated_drains == 1u);
    const float steady = max_step(out[0], latency + 2 * kBlock, 30 * kBlock + latency - kBlock);
    REQUIRE(steady > 0.0f);
    float worst = 0.0f;
    for (const auto& plane : out) {
      worst = std::max(worst, max_step(plane, latency, static_cast<int>(plane.size())));
    }
    CHECK(worst <= 1.5f * steady);
  }
}

TEST_CASE("an unsupported channel count is refused without advancing",
          "[playback][layout-switch]") {
  PlaybackRenderer renderer(auto_config(TargetKind::Speakers, ChannelLayout::Stereo), nullptr,
                            kRate, kBlock);
  const int latency = renderer.latency_samples();
  std::vector<std::vector<float>> in(8, std::vector<float>(kBlock, 0.0f));
  std::vector<std::vector<float>> out(2, std::vector<float>(kBlock, 0.0f));
  const float* in_ptrs[8];
  float* out_ptrs[2];
  for (int ch = 0; ch < 8; ++ch) in_ptrs[ch] = in[static_cast<size_t>(ch)].data();
  for (int ch = 0; ch < 2; ++ch) out_ptrs[ch] = out[static_cast<size_t>(ch)].data();

  // Impulse in the first block; the refused calls must not move time.
  in[0][10] = 0.3f;
  REQUIRE(renderer.process_planar(in_ptrs, 2, out_ptrs, 2, kBlock));
  std::vector<float> left(out[0]);
  in[0][10] = 0.0f;
  for (int channels : {0, 3, 4, 5, 7, 9}) {
    CHECK_FALSE(renderer.process_planar(in_ptrs, channels, out_ptrs, 2, kBlock));
  }
  CHECK_FALSE(renderer.process_planar(in_ptrs, 2, out_ptrs, 6, kBlock));
  CHECK_FALSE(renderer.process_planar(in_ptrs, 2, out_ptrs, 2, kBlock + 1));
  CHECK(renderer.process_planar(in_ptrs, 6, out_ptrs, 2, 0));
  CHECK(renderer.process_planar(in_ptrs, 3, out_ptrs, 2, 0));
  CHECK(renderer.input_channel_count() == 2);
  CHECK(renderer.diagnostics().layout_switches == 0u);

  for (int b = 0; b < 4; ++b) {
    REQUIRE(renderer.process_planar(in_ptrs, 2, out_ptrs, 2, kBlock));
    left.insert(left.end(), out[0].begin(), out[0].end());
  }
  const int onset = onset_in(Planes{left}, 0, static_cast<int>(left.size()));
  CHECK(onset == 10 + latency);

  // A fixed layout refuses any other count.
  RendererConfig fixed = auto_config(TargetKind::Speakers, ChannelLayout::Stereo);
  fixed.prepare.input_layout = InputLayout::FivePointOne;
  PlaybackRenderer fixed_renderer(fixed, nullptr, kRate, kBlock);
  CHECK_FALSE(fixed_renderer.process_planar(in_ptrs, 2, out_ptrs, 2, kBlock));
  CHECK(fixed_renderer.process_planar(in_ptrs, 6, out_ptrs, 2, kBlock));

  // auto with a channel map is refused at creation.
  sonare::ErrorCode code = sonare::ErrorCode::Ok;
  try {
    parse_renderer_config(R"({"input": {"layout": "auto", "channel_map": ["L", "R"]}})");
  } catch (const sonare::SonareException& e) {
    code = e.code();
  }
  CHECK(code == sonare::ErrorCode::InvalidParameter);
}

TEST_CASE("process refuses aliased buffers and replaces non-finite input",
          "[playback][layout-switch]") {
  RendererConfig config = auto_config(TargetKind::Speakers, ChannelLayout::Stereo);
  config.prepare.input_layout = InputLayout::Stereo;
  PlaybackRenderer renderer(config, nullptr, kRate, kBlock);
  std::vector<float> buffer(2 * kBlock, 0.0f);
  CHECK_FALSE(renderer.process_interleaved(buffer.data(), 2, buffer.data(), 2, kBlock));
  CHECK_FALSE(renderer.process_interleaved(buffer.data(), 2, buffer.data() + 1, 2, kBlock - 1));

  std::vector<float> in(2 * kBlock, 0.0f);
  std::vector<float> out(2 * kBlock, 1.0f);
  in[0] = std::numeric_limits<float>::quiet_NaN();
  in[3] = std::numeric_limits<float>::infinity();
  REQUIRE(renderer.process_interleaved(in.data(), 2, out.data(), 2, kBlock));
  CHECK(renderer.non_finite_discard_count() == 2u);
  CHECK(renderer.diagnostics().non_finite_discards == 2u);
  bool finite = true;
  for (float s : out) finite = finite && std::isfinite(s);
  CHECK(finite);
}

TEST_CASE("every front end drains below -60 dB within drain_frames", "[playback][layout-switch]") {
  struct Case {
    ChannelLayout input;
    TargetKind kind;
    ChannelLayout target;
    const char* name;
  };
  const Case cases[] = {
      {ChannelLayout::Stereo, TargetKind::Speakers, ChannelLayout::FivePointOne,
       "stereo upmix -> 5.1"},
      {ChannelLayout::Stereo, TargetKind::Headphones, ChannelLayout::Stereo,
       "stereo upmix -> headphones"},
      {ChannelLayout::FivePointOne, TargetKind::Speakers, ChannelLayout::Stereo,
       "5.1 -> stereo (LFE fold-down)"},
      {ChannelLayout::SevenPointOne, TargetKind::Speakers, ChannelLayout::Stereo,
       "7.1 -> stereo (LFE fold-down)"},
  };
  constexpr int kNoiseBlocks = 200;  // 2 s
  for (const Case& c : cases) {
    INFO(c.name);
    RendererConfig config = auto_config(c.kind, c.target);
    config.prepare.input_layout = c.input == ChannelLayout::Mono     ? InputLayout::Mono
                                  : c.input == ChannelLayout::Stereo ? InputLayout::Stereo
                                  : c.input == ChannelLayout::FivePointOne
                                      ? InputLayout::FivePointOne
                                      : InputLayout::SevenPointOne;
    config.realtime.upmix_lfe_from_upmix = true;
    const OutputBus bus = make_output_bus(config.prepare);
    FrontEnd front_end;
    front_end.prepare(kRate, kBlock, c.input, config.prepare, bus);
    front_end.apply_realtime(config.realtime, true);
    const int drain = front_end.drain_frames();
    const int in_count = sonare::channel_count(c.input);
    const int bus_count = bus.channel_count();
    const int noise_frames = kNoiseBlocks * kBlock;
    const int total = noise_frames + (drain / kBlock + 2) * kBlock;
    const Planes noise = noise_planes(in_count, static_cast<size_t>(noise_frames), 51u);

    Planes out(static_cast<size_t>(bus_count),
               std::vector<float>(static_cast<size_t>(total), 0.0f));
    std::vector<float*> bus_ptrs(static_cast<size_t>(bus_count));
    std::vector<const float*> in_ptrs(static_cast<size_t>(in_count));
    for (int start = 0; start < total; start += kBlock) {
      for (int p = 0; p < bus_count; ++p) {
        bus_ptrs[static_cast<size_t>(p)] = out[static_cast<size_t>(p)].data() + start;
      }
      if (start < noise_frames) {
        for (int ch = 0; ch < in_count; ++ch) {
          in_ptrs[static_cast<size_t>(ch)] = noise[static_cast<size_t>(ch)].data() + start;
        }
        front_end.process(in_ptrs.data(), bus_ptrs.data(), kBlock);
      } else {
        if (start == noise_frames) front_end.set_draining(true);
        front_end.process(nullptr, bus_ptrs.data(), kBlock);
      }
    }

    double worst = -1e9;
    int planes_checked = 0;
    for (int p = 0; p < bus_count; ++p) {
      const auto& plane = out[static_cast<size_t>(p)];
      // Steady level: mean 5 ms window energy over the last second of noise.
      const double steady = window_energy(plane, noise_frames - kRate, kRate) * kWindow5Ms / kRate;
      if (steady == 0.0) continue;
      ++planes_checked;
      const double tail = window_energy(plane, noise_frames + drain - kWindow5Ms, kWindow5Ms);
      const double below = to_db(tail) - to_db(steady);
      INFO("plane " << p);
      CHECK(below <= -60.0);
      worst = std::max(worst, below);
    }
    CHECK(planes_checked > 0);
  }
}
