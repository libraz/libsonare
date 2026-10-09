#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "bench_utils.h"
#include "rt/sliding_max.h"
#include "rt/true_peak_filter.h"
#include "util/constants.h"

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kBlockSamples = kSampleRate / 1000;
constexpr int kOversampleFactor = 4;
constexpr double kThresholdMs = 5.0;

volatile float g_sink = 0.0f;

using sonare::bench_utils::bench;

}  // namespace

int main(int argc, char** argv) {
  const int iterations = argc > 1 ? std::max(1, std::atoi(argv[1])) : 20000;
  const int runs = argc > 2 ? std::max(1, std::atoi(argv[2])) : 7;

  std::vector<std::vector<float>> input(kChannels, std::vector<float>(kBlockSamples, 0.0f));
  for (int ch = 0; ch < kChannels; ++ch) {
    for (int i = 0; i < kBlockSamples; ++i) {
      const float phase = static_cast<float>(i + ch * 11) / static_cast<float>(kBlockSamples);
      input[static_cast<size_t>(ch)][static_cast<size_t>(i)] =
          0.8f * std::sin(sonare::constants::kTwoPi * phase) +
          0.1f * std::sin(sonare::constants::kTwoPi * 7.0f * phase);
    }
  }

  const float* input_ptrs[kChannels] = {input[0].data(), input[1].data()};
  std::vector<std::vector<float>> upsampled(
      kChannels, std::vector<float>(kBlockSamples * kOversampleFactor, 0.0f));
  float* output_ptrs[kChannels] = {upsampled[0].data(), upsampled[1].data()};

  sonare::rt::TruePeakFilter filter(kChannels);
  sonare::rt::SlidingMax<float> sliding_max(kBlockSamples * kOversampleFactor);

  const double ms_per_1ms_block = bench(
      [&] {
        filter.upsample(input_ptrs, output_ptrs, kChannels, kBlockSamples);
        sliding_max.reset();
        for (const auto& channel : upsampled) {
          for (float sample : channel) {
            sliding_max.push(std::abs(sample));
          }
        }
        g_sink += sliding_max.max();
      },
      runs, iterations);

  std::printf("{\n");
  std::printf("  \"benchmark\": \"mastering_isp_4x_stereo_1ms\",\n");
  std::printf("  \"sample_rate\": %d,\n", kSampleRate);
  std::printf("  \"channels\": %d,\n", kChannels);
  std::printf("  \"block_samples\": %d,\n", kBlockSamples);
  std::printf("  \"oversample_factor\": %d,\n", kOversampleFactor);
  std::printf("  \"fir_taps_per_phase\": %d,\n", filter.latency_samples() * 2);
  std::printf("  \"runs\": %d,\n", runs);
  std::printf("  \"iterations_per_run\": %d,\n", iterations);
  std::printf("  \"median_ms_per_1ms_audio\": %.6f,\n", ms_per_1ms_block);
  std::printf("  \"threshold_ms_per_1ms_audio\": %.3f,\n", kThresholdMs);
  std::printf("  \"pass\": %s\n", ms_per_1ms_block < kThresholdMs ? "true" : "false");
  std::printf("}\n");

  return ms_per_1ms_block < kThresholdMs ? 0 : 2;
}
