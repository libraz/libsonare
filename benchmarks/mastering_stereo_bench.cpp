#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "bench_utils.h"
#include "mastering/stereo/mid_side.h"
#include "mastering/stereo/mono_maker.h"
#include "mastering/stereo/stereo_balance.h"
#include "util/constants.h"

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kBlockSamples = 256;

volatile float g_sink = 0.0f;

using sonare::bench_utils::bench;

}  // namespace

int main(int argc, char** argv) {
  const int iterations = argc > 1 ? std::max(1, std::atoi(argv[1])) : 5000;
  const int runs = argc > 2 ? std::max(1, std::atoi(argv[2])) : 7;

  // Pristine source signals (per channel, slightly different gains so the channels are not
  // identical and mid/side encode + balance + mono blending actually do work).
  std::vector<std::vector<float>> block(
      kChannels, sonare::bench_utils::make_two_tone_signal(kBlockSamples, kSampleRate, 1.2f));
  for (int ch = 1; ch < kChannels; ++ch) {
    block[static_cast<size_t>(ch)] =
        sonare::bench_utils::make_two_tone_signal(kBlockSamples, kSampleRate, 1.0f - ch * 0.1f);
  }

  // Mid/side buffers: non-aliasing input vs output.
  const std::vector<float> left_src = block[0];
  const std::vector<float> right_src = block[1];
  std::vector<float> mid_buf(static_cast<size_t>(kBlockSamples), 0.0f);
  std::vector<float> side_buf(static_cast<size_t>(kBlockSamples), 0.0f);
  std::vector<float> left_out(static_cast<size_t>(kBlockSamples), 0.0f);
  std::vector<float> right_out(static_cast<size_t>(kBlockSamples), 0.0f);

  // Pre-encoded mid/side as input to decode_buffer.
  std::vector<float> mid_src(static_cast<size_t>(kBlockSamples), 0.0f);
  std::vector<float> side_src(static_cast<size_t>(kBlockSamples), 0.0f);
  sonare::mastering::stereo::encode_buffer(left_src.data(), right_src.data(), mid_src.data(),
                                           side_src.data(), static_cast<size_t>(kBlockSamples));

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
