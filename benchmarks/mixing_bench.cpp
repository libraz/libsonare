#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "mastering/dynamics/compressor.h"
#include "mixing/channel_strip.h"
#include "util/constants.h"

namespace {

constexpr int kSampleRate = 48000;
constexpr int kBlockSamples = 256;
constexpr int kChannels = 2;

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

std::vector<float> make_signal(int strip_index, int channel) {
  std::vector<float> out(static_cast<size_t>(kBlockSamples), 0.0f);
  const float base = 110.0f + static_cast<float>(strip_index * 23 + channel * 17);
  for (int i = 0; i < kBlockSamples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
    out[static_cast<size_t>(i)] =
        0.2f * std::sin(2.0f * sonare::constants::kPi * base * t) +
        0.05f * std::sin(2.0f * sonare::constants::kPi * (base * 4.0f) * t);
  }
  return out;
}

std::vector<std::unique_ptr<sonare::mixing::ChannelStrip>> make_strips(int count,
                                                                       bool with_insert) {
  std::vector<std::unique_ptr<sonare::mixing::ChannelStrip>> strips;
  strips.reserve(static_cast<size_t>(count));
  for (int index = 0; index < count; ++index) {
    auto strip = std::make_unique<sonare::mixing::ChannelStrip>();
    if (with_insert) {
      sonare::mastering::dynamics::CompressorConfig config;
      config.threshold_db = -20.0f;
      config.ratio = 1.5f;
      config.attack_ms = 10.0f;
      config.release_ms = 80.0f;
      strip->add_pre_insert(std::make_unique<sonare::mastering::dynamics::Compressor>(config));
    }
    strip->prepare(kSampleRate, kBlockSamples);
    strip->set_fader_db(-6.0f + 0.2f * static_cast<float>(index % 5));
    strip->set_pan((static_cast<float>(index % 7) - 3.0f) / 6.0f);
    strip->set_width(index % 2 == 0 ? 1.1f : 0.9f);
    strips.push_back(std::move(strip));
  }
  return strips;
}

double bench_mix(int strip_count, bool with_insert, int runs, int iterations) {
  auto strips = make_strips(strip_count, with_insert);
  std::vector<std::vector<std::vector<float>>> source(
      static_cast<size_t>(strip_count),
      std::vector<std::vector<float>>(kChannels, std::vector<float>{}));
  std::vector<std::vector<std::vector<float>>> work = source;

  for (int strip = 0; strip < strip_count; ++strip) {
    for (int ch = 0; ch < kChannels; ++ch) {
      source[static_cast<size_t>(strip)][static_cast<size_t>(ch)] = make_signal(strip, ch);
      work[static_cast<size_t>(strip)][static_cast<size_t>(ch)] =
          source[static_cast<size_t>(strip)][static_cast<size_t>(ch)];
    }
  }

  std::vector<float> mix_l(static_cast<size_t>(kBlockSamples), 0.0f);
  std::vector<float> mix_r(static_cast<size_t>(kBlockSamples), 0.0f);

  return bench(
      [&] {
        std::fill(mix_l.begin(), mix_l.end(), 0.0f);
        std::fill(mix_r.begin(), mix_r.end(), 0.0f);
        for (int strip = 0; strip < strip_count; ++strip) {
          auto& left = work[static_cast<size_t>(strip)][0];
          auto& right = work[static_cast<size_t>(strip)][1];
          left = source[static_cast<size_t>(strip)][0];
          right = source[static_cast<size_t>(strip)][1];
          float* channels[] = {left.data(), right.data()};
          strips[static_cast<size_t>(strip)]->process(channels, kChannels, kBlockSamples);
          const auto meter = strips[static_cast<size_t>(strip)]->meter_snapshot();
          for (int i = 0; i < kBlockSamples; ++i) {
            mix_l[static_cast<size_t>(i)] += left[static_cast<size_t>(i)];
            mix_r[static_cast<size_t>(i)] += right[static_cast<size_t>(i)];
          }
          g_sink += meter.peak_db[0];
        }
        g_sink += mix_l[0] + mix_r[0];
      },
      runs, iterations);
}

}  // namespace

int main(int argc, char** argv) {
  const int iterations = argc > 1 ? std::max(1, std::atoi(argv[1])) : 2000;
  const int runs = argc > 2 ? std::max(1, std::atoi(argv[2])) : 7;

  const double strips_8_ms = bench_mix(8, false, runs, iterations);
  const double strips_32_ms = bench_mix(32, false, runs, iterations);
  const double strips_32_insert_ms = bench_mix(32, true, runs, iterations);

  std::printf("{\n");
  std::printf("  \"benchmark\": \"mixing_channel_strip\",\n");
  std::printf("  \"sample_rate\": %d,\n", kSampleRate);
  std::printf("  \"block_samples\": %d,\n", kBlockSamples);
  std::printf("  \"runs\": %d,\n", runs);
  std::printf("  \"iterations_per_run\": %d,\n", iterations);
  std::printf("  \"strip_8_metered_ms\": %.6f,\n", strips_8_ms);
  std::printf("  \"strip_32_metered_ms\": %.6f,\n", strips_32_ms);
  std::printf("  \"strip_32_compressor_insert_metered_ms\": %.6f\n", strips_32_insert_ms);
  std::printf("}\n");
  return 0;
}
