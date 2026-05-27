#include <Eigen/Core>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "util/constants.h"

namespace {

constexpr int kSize1 = 1025;
constexpr int kSize2 = 4097;

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

void power_eigen(const std::complex<float>* data, float* out, int n) {
  Eigen::Map<const Eigen::ArrayXcf> in_map(data, n);
  Eigen::Map<Eigen::ArrayXf> out_map(out, n);
  out_map = in_map.abs2();
}

void power_scalar(const std::complex<float>* data, float* out, int n) {
  for (int i = 0; i < n; ++i) {
    const float re = data[i].real();
    const float im = data[i].imag();
    out[i] = re * re + im * im;
  }
}

std::vector<std::complex<float>> make_complex_signal(int n) {
  std::vector<std::complex<float>> data(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    const float phase = 0.01f * sonare::constants::kPi * static_cast<float>(i);
    data[static_cast<size_t>(i)] = std::complex<float>(std::cos(phase), std::sin(phase));
  }
  return data;
}

struct SizeResult {
  double eigen_ms;
  double scalar_ms;
  double speedup;
  float max_abs_diff;
};

SizeResult run_size(int n, int iterations, int runs) {
  const std::vector<std::complex<float>> data = make_complex_signal(n);
  std::vector<float> out_eigen(static_cast<size_t>(n), 0.0f);
  std::vector<float> out_scalar(static_cast<size_t>(n), 0.0f);

  power_eigen(data.data(), out_eigen.data(), n);
  power_scalar(data.data(), out_scalar.data(), n);

  float max_abs_diff = 0.0f;
  for (int i = 0; i < n; ++i) {
    max_abs_diff = std::max(max_abs_diff, std::abs(out_eigen[static_cast<size_t>(i)] -
                                                   out_scalar[static_cast<size_t>(i)]));
  }

  const double eigen_ms = bench(
      [&] {
        power_eigen(data.data(), out_eigen.data(), n);
        g_sink += out_eigen[0];
      },
      runs, iterations);

  const double scalar_ms = bench(
      [&] {
        power_scalar(data.data(), out_scalar.data(), n);
        g_sink += out_scalar[0];
      },
      runs, iterations);

  return SizeResult{eigen_ms, scalar_ms, scalar_ms / std::max(eigen_ms, 1.0e-12), max_abs_diff};
}

}  // namespace

int main(int argc, char** argv) {
  const int iterations = argc > 1 ? std::max(1, std::atoi(argv[1])) : 50000;
  const int runs = argc > 2 ? std::max(1, std::atoi(argv[2])) : 7;

  const SizeResult r1 = run_size(kSize1, iterations, runs);
  const SizeResult r2 = run_size(kSize2, std::max(1, iterations / 4), runs);

  std::printf("{\n");
  std::printf("  \"benchmark\": \"spectrum_power_eigen_vs_scalar\",\n");
  std::printf("  \"size_1\": %d,\n", kSize1);
  std::printf("  \"size_2\": %d,\n", kSize2);
  std::printf("  \"runs\": %d,\n", runs);
  std::printf("  \"iterations_per_run\": %d,\n", iterations);
  std::printf("  \"iterations_per_run_size_2\": %d,\n", std::max(1, iterations / 4));
  std::printf("  \"power_1025_eigen_ms\": %.6f,\n", r1.eigen_ms);
  std::printf("  \"power_1025_scalar_ms\": %.6f,\n", r1.scalar_ms);
  std::printf("  \"power_1025_speedup_ratio\": %.6f,\n", r1.speedup);
  std::printf("  \"max_abs_diff_1025\": %.6e,\n", static_cast<double>(r1.max_abs_diff));
  std::printf("  \"power_4097_eigen_ms\": %.6f,\n", r2.eigen_ms);
  std::printf("  \"power_4097_scalar_ms\": %.6f,\n", r2.scalar_ms);
  std::printf("  \"power_4097_speedup_ratio\": %.6f,\n", r2.speedup);
  std::printf("  \"max_abs_diff_4097\": %.6e\n", static_cast<double>(r2.max_abs_diff));
  std::printf("}\n");

  return 0;
}
