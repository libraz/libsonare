#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "bench_utils.h"
#include "util/constants.h"

namespace {

volatile float g_sink = 0.0f;

using sonare::bench_utils::compare_kernels;
using sonare::bench_utils::print_three_size_json_header;
using sonare::bench_utils::print_three_size_json_result;
using sonare::bench_utils::three_size_iteration_schedule;

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

sonare::bench_utils::KernelComparisonResult run_size(int n, int iterations, int runs) {
  const float amin = 1.0e-10f;
  const float log_ref = 0.0f;
  const std::vector<float> x = make_input(n);
  return compare_kernels(
      x.data(), n, runs, iterations,
      [&](const float* input, float* output, int size) {
        log10_eigen(input, output, size, amin, log_ref);
      },
      [&](const float* input, float* output, int size) {
        log10_scalar(input, output, size, amin, log_ref);
      },
      &g_sink);
}

}  // namespace

int main(int argc, char** argv) {
  const int base_iter = argc > 1 ? std::max(1, std::atoi(argv[1])) : 10000;
  const int runs = argc > 2 ? std::max(1, std::atoi(argv[2])) : 7;

  // Per-size iteration counts so each size's total measured time is similar (~0.5s).
  // base_iter is for N=256; scale roughly inversely with N.
  const auto schedule = three_size_iteration_schedule(base_iter);

  const auto r256 = run_size(256, schedule.size_256, runs);
  const auto r1024 = run_size(1024, schedule.size_1024, runs);
  const auto r4096 = run_size(4096, schedule.size_4096, runs);

  print_three_size_json_header("log10_eigen_vs_scalar", runs, schedule);
  print_three_size_json_result("log10", 256, r256, false);
  print_three_size_json_result("log10", 1024, r1024, false);
  print_three_size_json_result("log10", 4096, r4096, true);
  std::printf("}\n");

  return 0;
}
