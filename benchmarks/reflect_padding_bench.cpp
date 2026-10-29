#include <cstdio>
#include <cstdlib>
#include <vector>

#include "bench_utils.h"
#include "util/reflect_padding.h"

namespace {
volatile float sink = 0.0f;

std::vector<float> reflect_each_sample(const float* data, size_t size, int pad) {
  std::vector<float> output(size + 2 * static_cast<size_t>(pad), 0.0f);
  for (size_t i = 0; i < output.size(); ++i) {
    output[i] = data[sonare::reflect_index(static_cast<int64_t>(i) - pad, size)];
  }
  return output;
}
}  // namespace

int main(int argc, char** argv) {
  const double minimum_speedup = argc > 1 ? std::atof(argv[1]) : 0.0;
  bool accepted = true;
  for (size_t size : {size_t{48000}, size_t{1440000}}) {
    constexpr int kPad = 1024;
    std::vector<float> input(size);
    for (size_t i = 0; i < size; ++i) input[i] = static_cast<float>(i % 127);
    const auto expected = reflect_each_sample(input.data(), size, kPad);
    const auto actual = sonare::reflect_center_pad(input.data(), size, kPad);
    if (expected != actual) return 1;
    const double reference_ms = sonare::bench_utils::bench(
        [&] {
          const auto output = reflect_each_sample(input.data(), size, kPad);
          sink += output[size / 2];
        },
        7, 10);
    const double current_ms = sonare::bench_utils::bench(
        [&] {
          const auto output = sonare::reflect_center_pad(input.data(), size, kPad);
          sink += output[size / 2];
        },
        7, 10);
    const double speedup = reference_ms / current_ms;
    std::printf("samples=%zu pad=%d reference_ms=%.6f current_ms=%.6f speedup=%.2f exact=yes\n",
                size, kPad, reference_ms, current_ms, speedup);
    accepted = accepted && speedup >= minimum_speedup;
  }
  return accepted ? 0 : 1;
}
