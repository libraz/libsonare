#include <Eigen/Core>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "util/constants.h"

namespace {

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

// Mirrors the pattern in util/math_utils.cpp::power_to_db and
// mastering/match/reference_spectrum.cpp:
//   out[i] = 10 * log10(max(amin, x[i])) - log_ref
void log10_eigen(const float* x, float* out, int n, float amin, float log_ref) {
  Eigen::Map<const Eigen::ArrayXf> in_map(x, n);
  Eigen::Map<Eigen::ArrayXf> out_map(out, n);
  out_map = (in_map.max(amin)).log10() * 10.0f - log_ref;
}

void log10_scalar(const float* x, float* out, int n, float amin, float log_ref) {
  for (int i = 0; i < n; ++i) {
    out[i] = 10.0f * std::log10(std::max(amin, x[i])) - log_ref;
  }
}

std::vector<float> make_input(int n) {
  // Positive-only data resembling magnitude/power spectrum entries.
  std::vector<float> x(static_cast<size_t>(n), 0.0f);
  for (int i = 0; i < n; ++i) {
    const float phase = 0.001f * sonare::constants::kPi * static_cast<float>(i);
    x[static_cast<size_t>(i)] = std::abs(std::sin(phase)) + 1.0e-6f;
  }
  return x;
}

struct SizeResult {
  double eigen_ms;
  double scalar_ms;
  double speedup;
  float max_abs_diff;
  int iterations;
};

SizeResult run_size(int n, int iterations, int runs) {
  const float amin = 1.0e-10f;
  const float log_ref = 0.0f;
  const std::vector<float> x = make_input(n);
  std::vector<float> out_eigen(static_cast<size_t>(n), 0.0f);
  std::vector<float> out_scalar(static_cast<size_t>(n), 0.0f);

  log10_eigen(x.data(), out_eigen.data(), n, amin, log_ref);
  log10_scalar(x.data(), out_scalar.data(), n, amin, log_ref);

  float max_abs_diff = 0.0f;
  for (int i = 0; i < n; ++i) {
    max_abs_diff = std::max(max_abs_diff, std::abs(out_eigen[static_cast<size_t>(i)] -
                                                   out_scalar[static_cast<size_t>(i)]));
  }

  const double eigen_ms = bench(
      [&] {
        log10_eigen(x.data(), out_eigen.data(), n, amin, log_ref);
        g_sink += out_eigen[0];
      },
      runs, iterations);

  const double scalar_ms = bench(
      [&] {
        log10_scalar(x.data(), out_scalar.data(), n, amin, log_ref);
        g_sink += out_scalar[0];
      },
      runs, iterations);

  return SizeResult{eigen_ms, scalar_ms, scalar_ms / std::max(eigen_ms, 1.0e-12), max_abs_diff,
                    iterations};
}

}  // namespace

int main(int argc, char** argv) {
  const int base_iter = argc > 1 ? std::max(1, std::atoi(argv[1])) : 10000;
  const int runs = argc > 2 ? std::max(1, std::atoi(argv[2])) : 7;

  // Per-size iteration counts so each size's total measured time is similar (~0.5s).
  // base_iter is for N=256; scale roughly inversely with N.
  const int iter_256 = base_iter;
  const int iter_1024 = std::max(1, base_iter / 4);
  const int iter_4096 = std::max(1, base_iter / 16);

  const SizeResult r256 = run_size(256, iter_256, runs);
  const SizeResult r1024 = run_size(1024, iter_1024, runs);
  const SizeResult r4096 = run_size(4096, iter_4096, runs);

  std::printf("{\n");
  std::printf("  \"benchmark\": \"log10_eigen_vs_scalar\",\n");
  std::printf("  \"runs\": %d,\n", runs);
  std::printf("  \"iterations_per_run_256\": %d,\n", iter_256);
  std::printf("  \"iterations_per_run_1024\": %d,\n", iter_1024);
  std::printf("  \"iterations_per_run_4096\": %d,\n", iter_4096);
  std::printf("  \"log10_256_eigen_ms\": %.6f,\n", r256.eigen_ms);
  std::printf("  \"log10_256_scalar_ms\": %.6f,\n", r256.scalar_ms);
  std::printf("  \"log10_256_speedup_ratio\": %.6f,\n", r256.speedup);
  std::printf("  \"max_abs_diff_256\": %.6e,\n", static_cast<double>(r256.max_abs_diff));
  std::printf("  \"log10_1024_eigen_ms\": %.6f,\n", r1024.eigen_ms);
  std::printf("  \"log10_1024_scalar_ms\": %.6f,\n", r1024.scalar_ms);
  std::printf("  \"log10_1024_speedup_ratio\": %.6f,\n", r1024.speedup);
  std::printf("  \"max_abs_diff_1024\": %.6e,\n", static_cast<double>(r1024.max_abs_diff));
  std::printf("  \"log10_4096_eigen_ms\": %.6f,\n", r4096.eigen_ms);
  std::printf("  \"log10_4096_scalar_ms\": %.6f,\n", r4096.scalar_ms);
  std::printf("  \"log10_4096_speedup_ratio\": %.6f,\n", r4096.speedup);
  std::printf("  \"max_abs_diff_4096\": %.6e\n", static_cast<double>(r4096.max_abs_diff));
  std::printf("}\n");

  return 0;
}
