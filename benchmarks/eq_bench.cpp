#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "mastering/eq/equalizer.h"
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

std::string format_result_json(int iterations, int runs, double static_ms, double dynamic_ms,
                               double linear_ms, int linear_latency_samples) {
  std::ostringstream json;
  json << "{\n";
  json << "  \"benchmark\": \"eq_processor\",\n";
  json << "  \"sample_rate\": " << kSampleRate << ",\n";
  json << "  \"channels\": " << kChannels << ",\n";
  json << "  \"block_samples\": " << kBlockSamples << ",\n";
  json << "  \"runs\": " << runs << ",\n";
  json << "  \"iterations_per_run\": " << iterations << ",\n";
  json.setf(std::ios::fixed);
  json.precision(6);
  json << "  \"static_24_band_ms\": " << static_ms << ",\n";
  json << "  \"dynamic_8_band_ms\": " << dynamic_ms << ",\n";
  json << "  \"linear_phase_4_band_ms\": " << linear_ms << ",\n";
  json << "  \"linear_phase_latency_samples\": " << linear_latency_samples << "\n";
  json << "}\n";
  return json.str();
}

std::vector<float> make_signal(int samples, float gain, float offset_hz) {
  std::vector<float> out(static_cast<size_t>(samples), 0.0f);
  for (int i = 0; i < samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
    out[static_cast<size_t>(i)] =
        gain * (0.55f * std::sin(2.0f * sonare::constants::kPi * (170.0f + offset_hz) * t) +
                0.30f * std::sin(2.0f * sonare::constants::kPi * (1300.0f + offset_hz) * t) +
                0.15f * std::sin(2.0f * sonare::constants::kPi * (7600.0f + offset_hz) * t));
  }
  return out;
}

void copy_block(const std::vector<std::vector<float>>& source,
                std::vector<std::vector<float>>& target, float* ptrs[kChannels]) {
  target = source;
  for (int ch = 0; ch < kChannels; ++ch) {
    ptrs[ch] = target[static_cast<size_t>(ch)].data();
  }
}

sonare::mastering::eq::EqBand make_peak(size_t index) {
  using sonare::mastering::eq::BiquadCoeffMode;
  using sonare::mastering::eq::EqBand;
  using sonare::mastering::eq::EqBandType;
  using sonare::mastering::eq::StereoPlacement;

  EqBand band{EqBandType::Peak,
              80.0f * std::pow(1.26f, static_cast<float>(index)),
              (index % 2 == 0 ? 1.8f : -1.6f),
              0.9f + 0.03f * static_cast<float>(index % 5),
              true,
              index % 3 == 0 ? BiquadCoeffMode::Vicanek : BiquadCoeffMode::Rbj};
  band.placement = index % 7 == 0 ? StereoPlacement::Mid : StereoPlacement::Stereo;
  band.proportional_q = true;
  return band;
}

}  // namespace

int main(int argc, char** argv) {
  using sonare::mastering::eq::EqBand;
  using sonare::mastering::eq::EqBandType;
  using sonare::mastering::eq::EqualizerProcessor;
  using sonare::mastering::eq::PhaseMode;

  const int iterations = argc > 1 ? std::max(1, std::atoi(argv[1])) : 2000;
  const int runs = argc > 2 ? std::max(1, std::atoi(argv[2])) : 7;

  std::vector<std::vector<float>> source(kChannels);
  source[0] = make_signal(kBlockSamples, 1.0f, 0.0f);
  source[1] = make_signal(kBlockSamples, 0.85f, 37.0f);

  EqualizerProcessor static_eq({kChannels, 0});
  static_eq.prepare(kSampleRate, kBlockSamples);
  for (size_t i = 0; i < EqualizerProcessor::kMaxBands; ++i) {
    static_eq.set_band(i, make_peak(i));
  }

  EqualizerProcessor dynamic_eq({kChannels, 0});
  dynamic_eq.prepare(kSampleRate, kBlockSamples);
  for (size_t i = 0; i < 8; ++i) {
    EqBand band = make_peak(i);
    band.dyn.enabled = true;
    band.dyn.threshold_db = -30.0f + static_cast<float>(i);
    band.dyn.ratio = 2.0f;
    band.dyn.range_db = i % 2 == 0 ? -4.0f : 3.0f;
    band.dyn.attack_ms = 3.0f;
    band.dyn.release_ms = 40.0f;
    dynamic_eq.set_band(i, band);
  }

  EqualizerProcessor linear_eq({kChannels, 0});
  linear_eq.prepare(kSampleRate, kBlockSamples);
  linear_eq.set_phase_mode(PhaseMode::LinearPhase);
  for (size_t i = 0; i < 4; ++i) {
    EqBand band{EqBandType::Peak, 240.0f * std::pow(2.0f, static_cast<float>(i)), 2.0f, 1.0f, true};
    linear_eq.set_band(i, band);
  }

  std::vector<std::vector<float>> work(kChannels);
  float* ptrs[kChannels] = {nullptr, nullptr};

  const double static_ms = bench(
      [&] {
        copy_block(source, work, ptrs);
        static_eq.process(ptrs, kChannels, kBlockSamples);
        g_sink += work[0][0] + work[1][0];
      },
      runs, iterations);

  const double dynamic_ms = bench(
      [&] {
        copy_block(source, work, ptrs);
        dynamic_eq.process(ptrs, kChannels, kBlockSamples);
        g_sink += work[0][0] + work[1][0];
      },
      runs, iterations);

  const double linear_ms = bench(
      [&] {
        copy_block(source, work, ptrs);
        linear_eq.process(ptrs, kChannels, kBlockSamples);
        g_sink += work[0][0] + work[1][0];
      },
      runs, iterations);

  const std::string json = format_result_json(iterations, runs, static_ms, dynamic_ms, linear_ms,
                                              linear_eq.latency_samples());
  std::printf("%s", json.c_str());
  if (argc > 3 && argv[3] != nullptr && argv[3][0] != '\0') {
    std::ofstream out(argv[3]);
    if (!out) {
      std::fprintf(stderr, "failed to open output file: %s\n", argv[3]);
      return 1;
    }
    out << json;
  }

  return 0;
}
