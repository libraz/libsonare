#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <vector>

#include "mastering/dynamics/brickwall_limiter.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "mastering/spectral/air_band.h"
#include "util/constants.h"

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kBlockSamples = 256;
constexpr double kTpOverheadTarget = 1.5;

volatile float g_sink = 0.0f;

double median_ms(std::vector<double> samples) {
  std::sort(samples.begin(), samples.end());
  const size_t n = samples.size();
  if (n == 0) return 0.0;
  if ((n % 2) == 1) return samples[n / 2];
  return (samples[n / 2 - 1] + samples[n / 2]) * 0.5;
}

template <typename Fn>
double bench(Fn&& fn, int runs, int iterations) {
  std::vector<double> times;
  times.reserve(static_cast<size_t>(runs));
  for (int run = 0; run < runs; ++run) {
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) fn();
    const auto t1 = std::chrono::steady_clock::now();
    const double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    times.push_back(total_ms / static_cast<double>(iterations));
  }
  return median_ms(std::move(times));
}

std::vector<float> make_signal(int samples, float gain = 0.8f) {
  std::vector<float> out(static_cast<size_t>(samples), 0.0f);
  for (int i = 0; i < samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
    out[static_cast<size_t>(i)] = gain * (0.7f * std::sin(sonare::constants::kTwoPi * 440.0f * t) +
                                          0.3f * std::sin(sonare::constants::kTwoPi * 2800.0f * t));
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  const int iterations = argc > 1 ? std::max(1, std::atoi(argv[1])) : 2000;
  const int runs = argc > 2 ? std::max(1, std::atoi(argv[2])) : 7;

  std::vector<std::vector<float>> block(kChannels, make_signal(kBlockSamples, 1.2f));
  for (int ch = 1; ch < kChannels; ++ch) {
    block[static_cast<size_t>(ch)] = make_signal(kBlockSamples, 1.0f - ch * 0.1f);
  }

  std::vector<std::vector<float>> tp_block = block;
  std::vector<std::vector<float>> detect_only_block = block;
  std::vector<std::vector<float>> fallback_tp_block = block;
  std::vector<std::vector<float>> brickwall_block = block;
  std::vector<std::vector<float>> compressor_block = block;
  std::vector<std::vector<float>> air_block = block;
  float* tp_ptrs[kChannels] = {tp_block[0].data(), tp_block[1].data()};
  float* detect_only_ptrs[kChannels] = {detect_only_block[0].data(), detect_only_block[1].data()};
  float* fallback_tp_ptrs[kChannels] = {fallback_tp_block[0].data(), fallback_tp_block[1].data()};
  float* brickwall_ptrs[kChannels] = {brickwall_block[0].data(), brickwall_block[1].data()};
  float* compressor_ptrs[kChannels] = {compressor_block[0].data(), compressor_block[1].data()};
  float* air_ptrs[kChannels] = {air_block[0].data(), air_block[1].data()};

  sonare::mastering::maximizer::TruePeakLimiter true_peak({-1.0f, 1.0f, 50.0f, 4, false});
  true_peak.prepare(kSampleRate, kBlockSamples);
  sonare::mastering::maximizer::TruePeakLimiter detect_only({-1.0f, 1.0f, 50.0f, 4, true});
  detect_only.prepare(kSampleRate, kBlockSamples);
  sonare::mastering::maximizer::TruePeakLimiter fallback_true_peak({-1.0f, 1.0f, 50.0f, 8});
  fallback_true_peak.prepare(kSampleRate, kBlockSamples);
  sonare::mastering::dynamics::BrickwallLimiter brickwall;
  brickwall.prepare(kSampleRate, kBlockSamples);
  sonare::mastering::dynamics::CompressorConfig compressor_config;
  compressor_config.threshold_db = -24.0f;
  compressor_config.ratio = 4.0f;
  compressor_config.pdr_time_ms = 150.0f;
  compressor_config.pdr_release_scale = 4.0f;
  sonare::mastering::dynamics::Compressor compressor(compressor_config);
  compressor.prepare(kSampleRate, kBlockSamples);
  sonare::mastering::spectral::AirBand air;
  air.prepare(kSampleRate, kBlockSamples);

  const double tp_ms = bench(
      [&] {
        tp_block = block;
        tp_ptrs[0] = tp_block[0].data();
        tp_ptrs[1] = tp_block[1].data();
        true_peak.process(tp_ptrs, kChannels, kBlockSamples);
        g_sink += tp_block[0][0];
      },
      runs, iterations);

  const double detect_only_ms = bench(
      [&] {
        detect_only_block = block;
        detect_only_ptrs[0] = detect_only_block[0].data();
        detect_only_ptrs[1] = detect_only_block[1].data();
        detect_only.process(detect_only_ptrs, kChannels, kBlockSamples);
        g_sink += detect_only_block[0][0];
      },
      runs, iterations);

  const double fallback_tp_ms = bench(
      [&] {
        fallback_tp_block = block;
        fallback_tp_ptrs[0] = fallback_tp_block[0].data();
        fallback_tp_ptrs[1] = fallback_tp_block[1].data();
        fallback_true_peak.process(fallback_tp_ptrs, kChannels, kBlockSamples);
        g_sink += fallback_tp_block[0][0];
      },
      runs, iterations);

  const double brickwall_ms = bench(
      [&] {
        brickwall_block = block;
        brickwall_ptrs[0] = brickwall_block[0].data();
        brickwall_ptrs[1] = brickwall_block[1].data();
        brickwall.process(brickwall_ptrs, kChannels, kBlockSamples);
        g_sink += brickwall_block[0][0];
      },
      runs, iterations);

  const double compressor_ms = bench(
      [&] {
        compressor_block = block;
        compressor_ptrs[0] = compressor_block[0].data();
        compressor_ptrs[1] = compressor_block[1].data();
        compressor.process(compressor_ptrs, kChannels, kBlockSamples);
        g_sink += compressor_block[0][0];
      },
      runs, iterations);

  const double air_ms = bench(
      [&] {
        air_block = block;
        air_ptrs[0] = air_block[0].data();
        air_ptrs[1] = air_block[1].data();
        air.process(air_ptrs, kChannels, kBlockSamples);
        g_sink += air_block[0][0];
      },
      runs, iterations);

  const double tp_overhead = tp_ms / std::max(fallback_tp_ms, 1.0e-9);
  const double detect_only_overhead = detect_only_ms / std::max(fallback_tp_ms, 1.0e-9);
  const double tp_vs_brickwall = tp_ms / std::max(brickwall_ms, 1.0e-9);
  const double detect_only_vs_brickwall = detect_only_ms / std::max(brickwall_ms, 1.0e-9);
  const bool tp_overhead_pass = tp_overhead < kTpOverheadTarget;
  const bool detect_only_overhead_pass = detect_only_overhead < kTpOverheadTarget;

  std::printf("{\n");
  std::printf("  \"benchmark\": \"mastering_support\",\n");
  std::printf("  \"sample_rate\": %d,\n", kSampleRate);
  std::printf("  \"block_samples\": %d,\n", kBlockSamples);
  std::printf("  \"runs\": %d,\n", runs);
  std::printf("  \"iterations_per_run\": %d,\n", iterations);
  std::printf("  \"true_peak_limiter_ms\": %.6f,\n", tp_ms);
  std::printf("  \"true_peak_detect_only_ms\": %.6f,\n", detect_only_ms);
  std::printf("  \"fallback_true_peak_limiter_ms\": %.6f,\n", fallback_tp_ms);
  std::printf("  \"brickwall_limiter_ms\": %.6f,\n", brickwall_ms);
  std::printf("  \"true_peak_overhead_ratio\": %.6f,\n", tp_overhead);
  std::printf("  \"true_peak_detect_only_overhead_ratio\": %.6f,\n", detect_only_overhead);
  std::printf("  \"true_peak_vs_brickwall_ratio\": %.6f,\n", tp_vs_brickwall);
  std::printf("  \"true_peak_detect_only_vs_brickwall_ratio\": %.6f,\n", detect_only_vs_brickwall);
  std::printf("  \"true_peak_overhead_target\": %.3f,\n", kTpOverheadTarget);
  std::printf("  \"true_peak_overhead_pass\": %s,\n", tp_overhead_pass ? "true" : "false");
  std::printf("  \"true_peak_detect_only_overhead_pass\": %s,\n",
              detect_only_overhead_pass ? "true" : "false");
  std::printf("  \"pdr_compressor_ms\": %.6f,\n", compressor_ms);
  std::printf("  \"air_band_ms\": %.6f\n", air_ms);
  std::printf("}\n");

  return (tp_overhead_pass || detect_only_overhead_pass) ? 0 : 2;
}
