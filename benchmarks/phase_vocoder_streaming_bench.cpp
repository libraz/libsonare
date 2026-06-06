#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "effects/phase_vocoder.h"
#include "util/constants.h"

namespace {

constexpr int kSampleRate = 48000;
constexpr int kDurationSeconds = 1;
constexpr int kInputSamples = kSampleRate * kDurationSeconds;
constexpr float kRate = 0.75f;
constexpr double kThresholdMsPerSecondAudio = 250.0;

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
    for (int i = 0; i < iterations; ++i) {
      fn();
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    times.push_back(total_ms / static_cast<double>(iterations));
  }
  return median_ms(std::move(times));
}

std::vector<float> make_fixture() {
  std::vector<float> samples(kInputSamples);
  for (int i = 0; i < kInputSamples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
    samples[static_cast<size_t>(i)] = 0.55f * std::sin(sonare::constants::kTwoPi * 220.0f * t) +
                                      0.25f * std::sin(sonare::constants::kTwoPi * 440.0f * t) +
                                      0.10f * std::sin(sonare::constants::kTwoPi * 880.0f * t);
  }
  return samples;
}

}  // namespace

int main(int argc, char** argv) {
  const int iterations = argc > 1 ? std::max(1, std::atoi(argv[1])) : 20;
  const int runs = argc > 2 ? std::max(1, std::atoi(argv[2])) : 5;

  const std::vector<float> samples = make_fixture();
  sonare::StreamingPhaseVocoderConfig config;
  config.sample_rate = kSampleRate;
  config.n_fft = 2048;
  config.hop_length = 512;
  config.phase_lock = true;
  const size_t expected_output =
      static_cast<size_t>(std::ceil(static_cast<float>(samples.size()) / kRate));
  sonare::StreamingPhaseVocoder streamer(config);
  streamer.reserve(samples.size(), expected_output + static_cast<size_t>(config.n_fft));
  std::vector<float> output(expected_output + static_cast<size_t>(config.n_fft), 0.0f);

  const double ms_per_second_audio = bench(
      [&] {
        streamer.reset();
        size_t written = 0;
        written += streamer.process_into(samples.data(), samples.size() / 2, kRate,
                                         output.data() + written, output.size() - written);
        written += streamer.process_into(samples.data() + samples.size() / 2,
                                         samples.size() - samples.size() / 2, kRate,
                                         output.data() + written, output.size() - written);
        written += streamer.finalize_into(kRate, output.data() + written, output.size() - written);
        if (written > 0) g_sink += output[0];
      },
      runs, iterations);

  std::printf("{\n");
  std::printf("  \"benchmark\": \"phase_vocoder_streaming_prototype\",\n");
  std::printf("  \"sample_rate\": %d,\n", kSampleRate);
  std::printf("  \"input_seconds\": %d,\n", kDurationSeconds);
  std::printf("  \"n_fft\": %d,\n", config.n_fft);
  std::printf("  \"hop_length\": %d,\n", config.hop_length);
  std::printf("  \"rate\": %.3f,\n", kRate);
  std::printf("  \"phase_lock\": %s,\n", config.phase_lock ? "true" : "false");
  std::printf("  \"runs\": %d,\n", runs);
  std::printf("  \"iterations_per_run\": %d,\n", iterations);
  std::printf("  \"median_ms_per_second_audio\": %.6f,\n", ms_per_second_audio);
  std::printf("  \"threshold_ms_per_second_audio\": %.3f,\n", kThresholdMsPerSecondAudio);
  std::printf("  \"pass\": %s\n",
              ms_per_second_audio < kThresholdMsPerSecondAudio ? "true" : "false");
  std::printf("}\n");

  return ms_per_second_audio < kThresholdMsPerSecondAudio ? 0 : 2;
}
