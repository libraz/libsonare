#include <Eigen/Core>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "util/constants.h"

namespace {

constexpr int kNMels = 128;
constexpr int kNBins = 1025;
constexpr int kNFrames = 100;

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

void apply_filterbank_eigen(const float* filterbank, const float* power, float* out, int n_mels,
                            int n_bins, int n_frames) {
  Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> fb_map(
      filterbank, n_mels, n_bins);
  Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> power_map(
      power, n_bins, n_frames);
  Eigen::Map<Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> result_map(
      out, n_mels, n_frames);
  result_map.noalias() = fb_map * power_map;
}

void apply_filterbank_scalar(const float* filterbank, const float* power, float* out, int n_mels,
                             int n_bins, int n_frames) {
  for (int m = 0; m < n_mels; ++m) {
    for (int f = 0; f < n_frames; ++f) {
      float acc = 0.0f;
      for (int k = 0; k < n_bins; ++k) {
        acc += filterbank[m * n_bins + k] * power[k * n_frames + f];
      }
      out[m * n_frames + f] = acc;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  const int iterations = argc > 1 ? std::max(1, std::atoi(argv[1])) : 200;
  const int runs = argc > 2 ? std::max(1, std::atoi(argv[2])) : 7;

  std::vector<float> filterbank(static_cast<size_t>(kNMels) * static_cast<size_t>(kNBins), 0.0f);
  for (int m = 0; m < kNMels; ++m) {
    const int center = m * 8;
    for (int k = 0; k < kNBins; ++k) {
      const float dist = std::abs(static_cast<float>(k - center));
      const float val = 1.0f - dist / 8.0f;
      filterbank[static_cast<size_t>(m * kNBins + k)] = std::max(0.0f, std::min(1.0f, val));
    }
  }

  std::vector<float> power(static_cast<size_t>(kNBins) * static_cast<size_t>(kNFrames), 0.0f);
  for (int k = 0; k < kNBins; ++k) {
    for (int f = 0; f < kNFrames; ++f) {
      const float phase = 0.01f * sonare::constants::kPi * static_cast<float>(k + f * 7);
      power[static_cast<size_t>(k * kNFrames + f)] = std::abs(std::sin(phase));
    }
  }

  std::vector<float> out_eigen(static_cast<size_t>(kNMels) * static_cast<size_t>(kNFrames), 0.0f);
  std::vector<float> out_scalar(static_cast<size_t>(kNMels) * static_cast<size_t>(kNFrames), 0.0f);

  apply_filterbank_eigen(filterbank.data(), power.data(), out_eigen.data(), kNMels, kNBins,
                         kNFrames);
  apply_filterbank_scalar(filterbank.data(), power.data(), out_scalar.data(), kNMels, kNBins,
                          kNFrames);

  float max_abs_diff = 0.0f;
  for (size_t i = 0; i < out_eigen.size(); ++i) {
    max_abs_diff = std::max(max_abs_diff, std::abs(out_eigen[i] - out_scalar[i]));
  }
  const bool correctness_pass = max_abs_diff <= 1e-3f;

  const double eigen_ms = bench(
      [&] {
        apply_filterbank_eigen(filterbank.data(), power.data(), out_eigen.data(), kNMels, kNBins,
                               kNFrames);
        g_sink += out_eigen[0];
      },
      runs, iterations);

  const double scalar_ms = bench(
      [&] {
        apply_filterbank_scalar(filterbank.data(), power.data(), out_scalar.data(), kNMels, kNBins,
                                kNFrames);
        g_sink += out_scalar[0];
      },
      runs, iterations);

  const double speedup = scalar_ms / std::max(eigen_ms, 1.0e-9);

  std::printf("{\n");
  std::printf("  \"benchmark\": \"filterbank_eigen_vs_scalar\",\n");
  std::printf("  \"n_mels\": %d,\n", kNMels);
  std::printf("  \"n_bins\": %d,\n", kNBins);
  std::printf("  \"n_frames\": %d,\n", kNFrames);
  std::printf("  \"runs\": %d,\n", runs);
  std::printf("  \"iterations_per_run\": %d,\n", iterations);
  std::printf("  \"filterbank_eigen_ms\": %.6f,\n", eigen_ms);
  std::printf("  \"filterbank_scalar_ms\": %.6f,\n", scalar_ms);
  std::printf("  \"eigen_speedup_ratio\": %.6f,\n", speedup);
  std::printf("  \"max_abs_diff\": %.6e,\n", static_cast<double>(max_abs_diff));
  std::printf("  \"correctness\": \"%s\"\n", correctness_pass ? "PASS" : "FAILED");
  std::printf("}\n");

  return 0;
}
