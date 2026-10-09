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

void tanh_eigen(const float* x, float* out, int n, float drive) {
  Eigen::Map<const Eigen::ArrayXf> in_map(x, n);
  Eigen::Map<Eigen::ArrayXf> out_map(out, n);
  out_map = (in_map * drive).tanh();
}

void tanh_scalar(const float* x, float* out, int n, float drive) {
  for (int i = 0; i < n; ++i) {
    out[i] = std::tanh(x[i] * drive);
  }
}

std::vector<float> make_input(int n) {
  std::vector<float> x(static_cast<size_t>(n), 0.0f);
  for (int i = 0; i < n; ++i) {
    const float phase = 0.013f * sonare::constants::kPi * static_cast<float>(i);
    x[static_cast<size_t>(i)] = 0.5f + 0.3f * std::sin(phase);
  }
  return x;
}

sonare::bench_utils::KernelComparisonResult run_size(int n, int iterations, int runs) {
  const float drive = 2.5f;
  const std::vector<float> x = make_input(n);
  return compare_kernels(
      x.data(), n, runs, iterations,
      [&](const float* input, float* output, int size) { tanh_eigen(input, output, size, drive); },
      [&](const float* input, float* output, int size) { tanh_scalar(input, output, size, drive); },
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

  print_three_size_json_header("transcendental_tanh_eigen_vs_scalar", runs, schedule);
  print_three_size_json_result("tanh", 256, r256, false);
  print_three_size_json_result("tanh", 1024, r1024, false);
  print_three_size_json_result("tanh", 4096, r4096, true);
  std::printf("}\n");

  return 0;
}
