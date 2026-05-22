#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <vector>

#include "mastering/common/lookahead_buffer.h"
#include "mastering/common/partitioned_convolver.h"
#include "mastering/dynamics/brickwall_limiter.h"
#include "mastering/maximizer/true_peak_limiter.h"

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kBlockSamples = 256;
constexpr int kIrSamples = 512;
constexpr int kLookaheadSamples = 1024;
constexpr double kTpOverheadTarget = 1.5;
constexpr double kLookaheadSpeedupTarget = 10.0;

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
    out[static_cast<size_t>(i)] =
        gain * (0.7f * std::sin(2.0f * 3.14159265358979323846f * 440.0f * t) +
                0.3f * std::sin(2.0f * 3.14159265358979323846f * 2800.0f * t));
  }
  return out;
}

std::vector<float> make_ir(int samples) {
  std::vector<float> ir(static_cast<size_t>(samples), 0.0f);
  for (int i = 0; i < samples; ++i) {
    const float n = static_cast<float>(i);
    const float window = 0.5f - 0.5f * std::cos(2.0f * 3.14159265358979323846f * n /
                                                static_cast<float>(samples - 1));
    const float decay = std::exp(-n / 96.0f);
    ir[static_cast<size_t>(i)] = window * decay * std::sin(0.17f * n);
  }
  return ir;
}

void direct_fir_block(const float* input, const float* ir, int input_samples, int ir_samples,
                      float* output) {
  for (int n = 0; n < input_samples; ++n) {
    float acc = 0.0f;
    for (int k = 0; k < ir_samples; ++k) {
      const int idx = n - k;
      if (idx >= 0) acc += input[idx] * ir[k];
    }
    output[n] = acc;
  }
}

float naive_lookahead_peak(const float* input, int samples, int lookahead) {
  float acc = 0.0f;
  for (int i = 0; i < samples; ++i) {
    float peak = 0.0f;
    const int end = std::min(samples, i + lookahead + 1);
    for (int j = i; j < end; ++j) {
      peak = std::max(peak, std::abs(input[j]));
    }
    acc += peak;
  }
  return acc;
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
  float* tp_ptrs[kChannels] = {tp_block[0].data(), tp_block[1].data()};
  float* detect_only_ptrs[kChannels] = {detect_only_block[0].data(), detect_only_block[1].data()};
  float* fallback_tp_ptrs[kChannels] = {fallback_tp_block[0].data(), fallback_tp_block[1].data()};
  float* brickwall_ptrs[kChannels] = {brickwall_block[0].data(), brickwall_block[1].data()};

  sonare::mastering::maximizer::TruePeakLimiter true_peak({-1.0f, 1.0f, 50.0f, 4, false});
  true_peak.prepare(kSampleRate, kBlockSamples);
  sonare::mastering::maximizer::TruePeakLimiter detect_only({-1.0f, 1.0f, 50.0f, 4, true});
  detect_only.prepare(kSampleRate, kBlockSamples);
  sonare::mastering::maximizer::TruePeakLimiter fallback_true_peak({-1.0f, 1.0f, 50.0f, 8});
  fallback_true_peak.prepare(kSampleRate, kBlockSamples);
  sonare::mastering::dynamics::BrickwallLimiter brickwall;
  brickwall.prepare(kSampleRate, kBlockSamples);

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

  const std::vector<float> input = make_signal(kBlockSamples, 0.9f);
  const std::vector<float> ir = make_ir(kIrSamples);
  std::vector<float> direct_out(kBlockSamples, 0.0f);
  std::vector<float> partitioned_out(kBlockSamples, 0.0f);
  sonare::mastering::common::PartitionedConvolver convolver({kBlockSamples});
  convolver.set_impulse_response(ir);

  const double direct_fir_ms = bench(
      [&] {
        direct_fir_block(input.data(), ir.data(), kBlockSamples, kIrSamples, direct_out.data());
        g_sink += direct_out[0];
      },
      runs, iterations);

  const double partitioned_ms = bench(
      [&] {
        convolver.reset();
        convolver.process_block(input.data(), partitioned_out.data());
        g_sink += partitioned_out[0];
      },
      runs, iterations);

  const std::vector<float> lookahead_input = make_signal(kSampleRate / 4, 0.95f);
  sonare::mastering::common::LookaheadBuffer lookahead;
  lookahead.prepare(kLookaheadSamples);

  const double lookahead_ms = bench(
      [&] {
        lookahead.reset();
        for (float sample : lookahead_input) {
          g_sink += lookahead.process(sample) + lookahead.peak();
        }
      },
      runs, std::max(1, iterations / 16));

  const double naive_ms = bench(
      [&] {
        g_sink += naive_lookahead_peak(lookahead_input.data(),
                                       static_cast<int>(lookahead_input.size()), kLookaheadSamples);
      },
      runs, std::max(1, iterations / 16));

  const double tp_overhead = tp_ms / std::max(fallback_tp_ms, 1.0e-9);
  const double detect_only_overhead = detect_only_ms / std::max(fallback_tp_ms, 1.0e-9);
  const double tp_vs_brickwall = tp_ms / std::max(brickwall_ms, 1.0e-9);
  const double detect_only_vs_brickwall = detect_only_ms / std::max(brickwall_ms, 1.0e-9);
  const double lookahead_speedup = naive_ms / std::max(lookahead_ms, 1.0e-9);
  const bool tp_overhead_pass = tp_overhead < kTpOverheadTarget;
  const bool detect_only_overhead_pass = detect_only_overhead < kTpOverheadTarget;
  const bool partitioned_break_even_pass = partitioned_ms <= direct_fir_ms;
  const bool lookahead_pass = lookahead_speedup >= kLookaheadSpeedupTarget;

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
  std::printf("  \"direct_fir_512_ms\": %.6f,\n", direct_fir_ms);
  std::printf("  \"partitioned_convolver_512_ms\": %.6f,\n", partitioned_ms);
  std::printf("  \"partitioned_break_even_pass\": %s,\n",
              partitioned_break_even_pass ? "true" : "false");
  std::printf("  \"lookahead_o1_ms\": %.6f,\n", lookahead_ms);
  std::printf("  \"lookahead_naive_ms\": %.6f,\n", naive_ms);
  std::printf("  \"lookahead_speedup_ratio\": %.6f,\n", lookahead_speedup);
  std::printf("  \"lookahead_speedup_target\": %.3f,\n", kLookaheadSpeedupTarget);
  std::printf("  \"lookahead_speedup_pass\": %s\n", lookahead_pass ? "true" : "false");
  std::printf("}\n");

  return ((tp_overhead_pass || detect_only_overhead_pass) && lookahead_pass &&
          partitioned_break_even_pass)
             ? 0
             : 2;
}
