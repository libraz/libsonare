#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <vector>

#include "core/harmonic.h"
#include "util/sequence.h"

namespace {
volatile float sink = 0.0f;

struct Measurement {
  double median_ms;
  float checksum;
};

template <typename Run>
Measurement measure(Run&& run) {
  std::vector<double> samples;
  samples.reserve(7);
  float checksum = 0.0f;
  for (int repeat = 0; repeat < 7; ++repeat) {
    const auto start = std::chrono::steady_clock::now();
    checksum += run();
    const auto stop = std::chrono::steady_clock::now();
    samples.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
  }
  std::sort(samples.begin(), samples.end());
  sink += checksum;
  return {samples[samples.size() / 2], checksum};
}

std::vector<float> make_transition(int states) {
  std::vector<float> transition(static_cast<size_t>(states) * states);
  for (int p = 0; p < states; ++p) {
    for (int s = 0; s < states; ++s) {
      transition[static_cast<size_t>(p) * states + s] =
          0.01f + static_cast<float>((p * 17 + s * 29) % 97) / 100.0f;
    }
  }
  return transition;
}

void bench_viterbi(int states, int steps) {
  std::vector<float> emissions(static_cast<size_t>(states) * steps);
  for (size_t i = 0; i < emissions.size(); ++i) {
    emissions[i] = -static_cast<float>((i * 13 + i / 11) % 101) / 100.0f;
  }
  const std::vector<float> transition = make_transition(states);
  const Measurement result = measure([&] {
    const std::vector<int> path =
        sonare::viterbi(emissions.data(), states, steps, transition.data());
    long long checksum = 0;
    for (size_t i = 0; i < path.size(); ++i) checksum += static_cast<long long>(i + 1) * path[i];
    return static_cast<float>(checksum);
  });
  std::printf("viterbi states=%d steps=%d median_ms=%.3f checksum=%.0f\n", states, steps,
              result.median_ms, result.checksum);
}

void bench_harmonics() {
  constexpr int kBins = 1025;
  constexpr int kFrames = 256;
  const std::vector<float> freqs = [&] {
    std::vector<float> values(kBins);
    for (int i = 0; i < kBins; ++i) values[i] = static_cast<float>(i) * 24000.0f / (kBins - 1);
    return values;
  }();
  const std::vector<float> spectrum = [&] {
    std::vector<float> values(static_cast<size_t>(kBins) * kFrames);
    for (size_t i = 0; i < values.size(); ++i)
      values[i] = static_cast<float>((i * 19) % 251) / 251.0f;
    return values;
  }();
  const std::vector<float> harmonics{1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
  const Measurement interp = measure([&] {
    const std::vector<float> values =
        sonare::interp_harmonics(spectrum, kBins, kFrames, freqs, harmonics);
    return std::accumulate(values.begin(), values.end(), 0.0f);
  });
  const Measurement sal = measure([&] {
    const std::vector<float> values = sonare::salience(spectrum, kBins, kFrames, freqs, harmonics);
    return std::accumulate(values.begin(), values.end(), 0.0f);
  });
  std::printf(
      "harmonics bins=%d frames=%d count=%zu interp_ms=%.3f interp_checksum=%.6f salience_ms=%.3f "
      "salience_checksum=%.6f\n",
      kBins, kFrames, harmonics.size(), interp.median_ms, interp.checksum, sal.median_ms,
      sal.checksum);
}
}  // namespace

int main() {
  bench_viterbi(12, 500);
  bench_viterbi(12, 2000);
  bench_viterbi(60, 500);
  bench_viterbi(60, 2000);
  bench_harmonics();
}
