#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "mastering/stereo/mid_side.h"
#include "mastering/stereo/mono_maker.h"
#include "mastering/stereo/stereo_balance.h"
#include "util/constants.h"

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kBlockSamples = 256;

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
        gain * (0.7f * std::sin(2.0f * sonare::constants::kPi * 440.0f * t) +
                0.3f * std::sin(2.0f * sonare::constants::kPi * 2800.0f * t));
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  const int iterations = argc > 1 ? std::max(1, std::atoi(argv[1])) : 5000;
  const int runs = argc > 2 ? std::max(1, std::atoi(argv[2])) : 7;

  // Pristine source signals (per channel, slightly different gains so the channels are not
  // identical and mid/side encode + balance + mono blending actually do work).
  std::vector<std::vector<float>> block(kChannels, make_signal(kBlockSamples, 1.2f));
  for (int ch = 1; ch < kChannels; ++ch) {
    block[static_cast<size_t>(ch)] = make_signal(kBlockSamples, 1.0f - ch * 0.1f);
  }

  // Mid/side buffers: non-aliasing input vs output.
  const std::vector<float> left_src = block[0];
  const std::vector<float> right_src = block[1];
  std::vector<float> mid_buf(static_cast<size_t>(kBlockSamples), 0.0f);
  std::vector<float> side_buf(static_cast<size_t>(kBlockSamples), 0.0f);
  std::vector<float> left_out(static_cast<size_t>(kBlockSamples), 0.0f);
  std::vector<float> right_out(static_cast<size_t>(kBlockSamples), 0.0f);

  // Pre-encoded mid/side as input to decode_buffer.
  const std::vector<float> mid_src = [&] {
    std::vector<float> m(static_cast<size_t>(kBlockSamples), 0.0f);
    std::vector<float> s(static_cast<size_t>(kBlockSamples), 0.0f);
    sonare::mastering::stereo::encode_buffer(left_src.data(), right_src.data(), m.data(), s.data(),
                                             static_cast<size_t>(kBlockSamples));
    return m;
  }();
  const std::vector<float> side_src = [&] {
    std::vector<float> m(static_cast<size_t>(kBlockSamples), 0.0f);
    std::vector<float> s(static_cast<size_t>(kBlockSamples), 0.0f);
    sonare::mastering::stereo::encode_buffer(left_src.data(), right_src.data(), m.data(), s.data(),
                                             static_cast<size_t>(kBlockSamples));
    return s;
  }();

  const double encode_ms = bench(
      [&] {
        sonare::mastering::stereo::encode_buffer(left_src.data(), right_src.data(), mid_buf.data(),
                                                 side_buf.data(),
                                                 static_cast<size_t>(kBlockSamples));
        g_sink += mid_buf[0] + side_buf[0];
      },
      runs, iterations);

  const double decode_ms = bench(
      [&] {
        sonare::mastering::stereo::decode_buffer(mid_src.data(), side_src.data(), left_out.data(),
                                                 right_out.data(),
                                                 static_cast<size_t>(kBlockSamples));
        g_sink += left_out[0] + right_out[0];
      },
      runs, iterations);

  // Stereo balance: balance=0.3, constant_power=true.
  std::vector<std::vector<float>> balance_block = block;
  float* balance_ptrs[kChannels] = {balance_block[0].data(), balance_block[1].data()};
  sonare::mastering::stereo::StereoBalance balance({0.3f, true});
  balance.prepare(kSampleRate, kBlockSamples);

  const double balance_ms = bench(
      [&] {
        balance_block = block;
        balance_ptrs[0] = balance_block[0].data();
        balance_ptrs[1] = balance_block[1].data();
        balance.process(balance_ptrs, kChannels, kBlockSamples);
        g_sink += balance_block[0][0];
      },
      runs, iterations);

  // Mono maker: amount=0.5.
  std::vector<std::vector<float>> mono_block = block;
  float* mono_ptrs[kChannels] = {mono_block[0].data(), mono_block[1].data()};
  sonare::mastering::stereo::MonoMaker mono({0.5f});
  mono.prepare(kSampleRate, kBlockSamples);

  const double mono_ms = bench(
      [&] {
        mono_block = block;
        mono_ptrs[0] = mono_block[0].data();
        mono_ptrs[1] = mono_block[1].data();
        mono.process(mono_ptrs, kChannels, kBlockSamples);
        g_sink += mono_block[0][0];
      },
      runs, iterations);

  std::printf("{\n");
  std::printf("  \"benchmark\": \"mastering_stateless_stereo\",\n");
  std::printf("  \"sample_rate\": %d,\n", kSampleRate);
  std::printf("  \"block_samples\": %d,\n", kBlockSamples);
  std::printf("  \"runs\": %d,\n", runs);
  std::printf("  \"iterations_per_run\": %d,\n", iterations);
  std::printf("  \"mid_side_encode_buffer_ms\": %.6f,\n", encode_ms);
  std::printf("  \"mid_side_decode_buffer_ms\": %.6f,\n", decode_ms);
  std::printf("  \"stereo_balance_ms\": %.6f,\n", balance_ms);
  std::printf("  \"mono_maker_ms\": %.6f\n", mono_ms);
  std::printf("}\n");

  return 0;
}
