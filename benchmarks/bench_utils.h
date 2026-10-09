#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <utility>
#include <vector>

namespace sonare::bench_utils {

struct KernelComparisonResult {
  double vectorized_ms;
  double scalar_ms;
  double speedup;
  float max_abs_diff;
};

inline double median_ms(std::vector<double> samples) {
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

template <typename Input, typename VectorizedFn, typename ScalarFn>
KernelComparisonResult compare_kernels(const Input* input, int n, int runs, int iterations,
                                       VectorizedFn&& vectorized, ScalarFn&& scalar,
                                       volatile float* sink) {
  std::vector<float> vectorized_output(static_cast<size_t>(n), 0.0f);
  std::vector<float> scalar_output(static_cast<size_t>(n), 0.0f);

  vectorized(input, vectorized_output.data(), n);
  scalar(input, scalar_output.data(), n);

  float max_abs_diff = 0.0f;
  for (int i = 0; i < n; ++i) {
    max_abs_diff = std::max(max_abs_diff, std::abs(vectorized_output[static_cast<size_t>(i)] -
                                                   scalar_output[static_cast<size_t>(i)]));
  }

  const double vectorized_ms = bench(
      [&] {
        vectorized(input, vectorized_output.data(), n);
        *sink += vectorized_output[0];
      },
      runs, iterations);

  const double scalar_ms = bench(
      [&] {
        scalar(input, scalar_output.data(), n);
        *sink += scalar_output[0];
      },
      runs, iterations);

  return {vectorized_ms, scalar_ms, scalar_ms / std::max(vectorized_ms, 1.0e-12), max_abs_diff};
}

struct ThreeSizeIterationSchedule {
  int size_256;
  int size_1024;
  int size_4096;
};

inline ThreeSizeIterationSchedule three_size_iteration_schedule(int base_iterations) {
  return {base_iterations, std::max(1, base_iterations / 4), std::max(1, base_iterations / 16)};
}

inline void print_three_size_json_header(const char* benchmark, int runs,
                                         const ThreeSizeIterationSchedule& schedule) {
  std::printf("{\n");
  std::printf("  \"benchmark\": \"%s\",\n", benchmark);
  std::printf("  \"runs\": %d,\n", runs);
  std::printf("  \"iterations_per_run_256\": %d,\n", schedule.size_256);
  std::printf("  \"iterations_per_run_1024\": %d,\n", schedule.size_1024);
  std::printf("  \"iterations_per_run_4096\": %d,\n", schedule.size_4096);
}

inline void print_three_size_json_result(const char* operation, int size,
                                         const KernelComparisonResult& result, bool last) {
  std::printf("  \"%s_%d_eigen_ms\": %.6f,\n", operation, size, result.vectorized_ms);
  std::printf("  \"%s_%d_scalar_ms\": %.6f,\n", operation, size, result.scalar_ms);
  std::printf("  \"%s_%d_speedup_ratio\": %.6f,\n", operation, size, result.speedup);
  std::printf("  \"max_abs_diff_%d\": %.6e%s\n", size, static_cast<double>(result.max_abs_diff),
              last ? "" : ",");
}

}  // namespace sonare::bench_utils
