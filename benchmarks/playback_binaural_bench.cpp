// Per-block cost of the playback renderer on its heaviest target: 7.1 input
// under input.layout "auto" rendered to headphones with the living_room room,
// head yaw changing every block. A second case switches stereo <-> 7.1 every
// second with upmix on, so the drain blocks (the old front end still running,
// STFT included) are timed too.
//
// Each case runs several passes over the same signal on a fresh renderer; a
// block's cost is its median across passes, which keeps a structural spike (a
// drain block lands on the same index every pass) while discarding scheduler
// noise.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

#include "playback/config.h"
#include "playback/hrtf_set.h"
#include "playback/renderer.h"
#include "util/constants.h"

namespace {

using sonare::constants::kTwoPi;

constexpr int kSampleRate = 48000;
constexpr int kBlock = 128;
constexpr int kMaxInputChannels = 8;
constexpr int kBlocksPerSecond = kSampleRate / kBlock;
constexpr int kPasses = 5;
constexpr int kWarmupBlocks = 2 * kBlocksPerSecond;
constexpr int kSteadyBlocks = 5 * kBlocksPerSecond;
constexpr int kSwitchingBlocks = 10 * kBlocksPerSecond;
constexpr double kSteadyThresholdMs = 1.0;
constexpr double kSwitchingThresholdMs = 1.5;
constexpr float kNoiseAmplitude = 0.1f;  // -20 dBFS peak
constexpr float kYawDepthDeg = 60.0f;
constexpr float kYawRateHz = 0.25f;

constexpr char kConfig[] = R"({"input":{"layout":"auto"},"target":{"kind":"headphones"},)"
                           R"("room":{"preset":"living_room"},"upmix":{"enabled":true}})";

volatile float g_sink = 0.0f;

std::vector<uint8_t> read_file(const char* path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// One second of independent uniform noise per channel, cycled.
std::vector<std::vector<float>> make_noise() {
  std::vector<std::vector<float>> planes(kMaxInputChannels,
                                         std::vector<float>(static_cast<size_t>(kSampleRate)));
  uint32_t state = 0x12345678u;
  for (auto& plane : planes) {
    for (float& sample : plane) {
      state = state * 1664525u + 1013904223u;
      sample = kNoiseAmplitude * (static_cast<float>(state >> 8) / 8388608.0f - 1.0f);
    }
  }
  return planes;
}

struct Summary {
  double mean_ms = 0.0;
  double p99_ms = 0.0;
  double max_ms = 0.0;
};

Summary summarize(std::vector<double> ms) {
  Summary s;
  if (ms.empty()) return s;
  double total = 0.0;
  for (double v : ms) total += v;
  s.mean_ms = total / static_cast<double>(ms.size());
  std::sort(ms.begin(), ms.end());
  s.p99_ms = ms[static_cast<size_t>(0.99 * static_cast<double>(ms.size() - 1))];
  s.max_ms = ms.back();
  return s;
}

// Median across passes of each block's time.
std::vector<double> per_block_median(const std::vector<std::vector<double>>& passes) {
  const size_t blocks = passes.front().size();
  std::vector<double> out(blocks);
  std::vector<double> column(passes.size());
  for (size_t b = 0; b < blocks; ++b) {
    for (size_t p = 0; p < passes.size(); ++p) column[p] = passes[p][b];
    std::sort(column.begin(), column.end());
    out[b] = column[column.size() / 2];
  }
  return out;
}

// Renders warm-up plus @p blocks timed blocks. @p channels_for(block) picks the
// input channel count of each timed block.
template <typename ChannelsFor>
std::vector<double> run_pass(const sonare::playback::RendererConfig& config,
                             const sonare::playback::HrtfSet& hrtf,
                             const std::vector<std::vector<float>>& noise, int blocks,
                             ChannelsFor channels_for, int warmup_channels) {
  sonare::playback::PlaybackRenderer renderer(config, &hrtf, kSampleRate, kBlock);
  std::vector<std::vector<float>> out(2, std::vector<float>(kBlock));
  float* out_ptrs[2] = {out[0].data(), out[1].data()};
  const float* in_ptrs[kMaxInputChannels] = {};
  std::vector<double> times(static_cast<size_t>(blocks));
  for (int b = -kWarmupBlocks; b < blocks; ++b) {
    const int index = b + kWarmupBlocks;
    const size_t offset = static_cast<size_t>((index % kBlocksPerSecond) * kBlock);
    for (int ch = 0; ch < kMaxInputChannels; ++ch) {
      in_ptrs[ch] = noise[static_cast<size_t>(ch)].data() + offset;
    }
    const float t = static_cast<float>(index * kBlock) / static_cast<float>(kSampleRate);
    const int channels = b < 0 ? warmup_channels : channels_for(b);
    const auto t0 = std::chrono::steady_clock::now();
    renderer.set_head_orientation(kYawDepthDeg * std::sin(kTwoPi * kYawRateHz * t), 0.0f, 0.0f);
    const bool ok = renderer.process_planar(in_ptrs, channels, out_ptrs, 2, kBlock);
    const auto t1 = std::chrono::steady_clock::now();
    if (!ok) {
      std::fprintf(stderr, "process_planar rejected a block\n");
      std::exit(1);
    }
    g_sink += out[0][0] + out[1][kBlock - 1];
    if (b >= 0) {
      times[static_cast<size_t>(b)] = std::chrono::duration<double, std::milli>(t1 - t0).count();
    }
  }
  return times;
}

}  // namespace

int main() {
  const std::vector<uint8_t> shrf = read_file(SONARE_PLAYBACK_DEFAULT_SHRF);
  if (shrf.empty()) {
    std::fprintf(stderr, "cannot read %s\n", SONARE_PLAYBACK_DEFAULT_SHRF);
    return 1;
  }
  const auto hrtf = sonare::playback::HrtfSet::from_memory(shrf.data(), shrf.size());
  const auto config = sonare::playback::parse_renderer_config(kConfig);
  const auto noise = make_noise();

  // Steady 7.1, yaw moving every block.
  std::vector<std::vector<double>> steady_passes;
  for (int p = 0; p < kPasses; ++p) {
    steady_passes.push_back(run_pass(config, hrtf, noise, kSteadyBlocks, [](int) { return 8; }, 8));
  }
  const Summary steady = summarize(per_block_median(steady_passes));

  // Stereo <-> 7.1 every second; the drain window follows each switch.
  const auto switching_channels = [](int b) { return (b / kBlocksPerSecond) % 2 == 0 ? 8 : 2; };
  std::vector<std::vector<double>> switching_passes;
  for (int p = 0; p < kPasses; ++p) {
    switching_passes.push_back(
        run_pass(config, hrtf, noise, kSwitchingBlocks, switching_channels, 2));
  }
  const std::vector<double> switching_ms = per_block_median(switching_passes);
  const Summary switching = summarize(switching_ms);
  // The longest drain (stereo upmix) is ~234 ms at 48 kHz; time the first
  // 300 ms after every switch as the drain window.
  std::vector<double> drain_ms;
  const int drain_blocks = (300 * kBlocksPerSecond) / 1000;
  for (int b = 0; b < kSwitchingBlocks; ++b) {
    if (b % kBlocksPerSecond < drain_blocks)
      drain_ms.push_back(switching_ms[static_cast<size_t>(b)]);
  }
  const Summary drain = summarize(drain_ms);

  const bool pass = steady.mean_ms < kSteadyThresholdMs && switching.max_ms < kSwitchingThresholdMs;
  std::printf("{\n");
  std::printf("  \"benchmark\": \"playback_binaural_7_1_headphones_living_room\",\n");
  std::printf("  \"sample_rate\": %d,\n", kSampleRate);
  std::printf("  \"block_frames\": %d,\n", kBlock);
  std::printf("  \"passes\": %d,\n", kPasses);
  std::printf("  \"steady_blocks\": %d,\n", kSteadyBlocks);
  std::printf("  \"steady_mean_ms_per_block\": %.4f,\n", steady.mean_ms);
  std::printf("  \"steady_p99_ms_per_block\": %.4f,\n", steady.p99_ms);
  std::printf("  \"steady_max_ms_per_block\": %.4f,\n", steady.max_ms);
  std::printf("  \"steady_threshold_ms\": %.3f,\n", kSteadyThresholdMs);
  std::printf("  \"switching_blocks\": %d,\n", kSwitchingBlocks);
  std::printf("  \"switching_mean_ms_per_block\": %.4f,\n", switching.mean_ms);
  std::printf("  \"switching_p99_ms_per_block\": %.4f,\n", switching.p99_ms);
  std::printf("  \"switching_max_ms_per_block\": %.4f,\n", switching.max_ms);
  std::printf("  \"drain_window_mean_ms_per_block\": %.4f,\n", drain.mean_ms);
  std::printf("  \"drain_window_max_ms_per_block\": %.4f,\n", drain.max_ms);
  std::printf("  \"switching_threshold_ms\": %.3f,\n", kSwitchingThresholdMs);
  std::printf("  \"pass\": %s\n", pass ? "true" : "false");
  std::printf("}\n");
  return pass ? 0 : 2;
}
