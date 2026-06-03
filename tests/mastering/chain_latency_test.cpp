#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "util/constants.h"

namespace api = sonare::mastering::api;
namespace maximizer = sonare::mastering::maximizer;

namespace {

constexpr int kSampleRate = 48000;
using sonare::constants::kPi;

// Index of the maximum-magnitude sample.
size_t argmax_abs(const std::vector<float>& signal) {
  size_t best_index = 0;
  float best_value = -1.0f;
  for (size_t i = 0; i < signal.size(); ++i) {
    const float magnitude = std::abs(signal[i]);
    if (magnitude > best_value) {
      best_value = magnitude;
      best_index = i;
    }
  }
  return best_index;
}

// Build mostly-silent mono signal of length n with a single Hann-windowed sine
// burst centered at index `center`. Peak amplitude stays well below the limiter
// ceiling so the limiter applies no gain reduction (acts as a pure delay).
std::vector<float> make_centered_burst(int n, int center, int burst_len, float amplitude,
                                       float freq_hz) {
  std::vector<float> signal(static_cast<size_t>(n), 0.0f);
  const int half = burst_len / 2;
  const int start = center - half;
  for (int k = 0; k < burst_len; ++k) {
    const int idx = start + k;
    if (idx < 0 || idx >= n) continue;
    // Hann window peaks at k == half, aligning the envelope peak with `center`.
    const float window =
        0.5f *
        (1.0f - std::cos(2.0f * kPi * static_cast<float>(k) / static_cast<float>(burst_len - 1)));
    const float t = static_cast<float>(k) / static_cast<float>(kSampleRate);
    signal[static_cast<size_t>(idx)] = amplitude * window * std::sin(2.0f * kPi * freq_hz * t);
  }
  return signal;
}

}  // namespace

TEST_CASE("G4 latency compensation time-aligns the true-peak limiter output",
          "[mastering][chain][latency]") {
  constexpr int kN = 8192;
  constexpr int kCenter = 4000;
  constexpr int kBurstLen = 400;
  constexpr float kAmplitude = 0.5f;  // Below ceiling -> no gain reduction.
  constexpr float kFreqHz = 1000.0f;

  const auto input = make_centered_burst(kN, kCenter, kBurstLen, kAmplitude, kFreqHz);

  // Config: only the true-peak limiter enabled, with a high ceiling so a
  // 0.5-amplitude signal is not limited (the limiter acts as a pure delay).
  api::MasteringChainConfig config;
  config.maximizer.true_peak_limiter.enabled = true;
  config.maximizer.true_peak_limiter.config.ceiling_db = 0.0f;

  api::MasteringChain chain(config);
  const api::MonoChainResult result = chain.process_mono(input.data(), input.size(), kSampleRate);

  REQUIRE(result.samples.size() == static_cast<size_t>(kN));

  // Sanity: the limiter actually reports non-zero latency in this config,
  // otherwise the alignment assertion would prove nothing.
  maximizer::TruePeakLimiter limiter(config.maximizer.true_peak_limiter.config);
  limiter.prepare(static_cast<double>(kSampleRate), kN);
  const int latency = limiter.latency_samples();

  const size_t argmax_in = argmax_abs(input);
  const size_t argmax_out = argmax_abs(result.samples);
  const long delta = std::abs(static_cast<long>(argmax_out) - static_cast<long>(argmax_in));

  CAPTURE(argmax_in, argmax_out, latency, delta);

  CHECK(latency > 0);
  // G4 should remove the limiter's lookahead + FIR delay so the transient stays
  // put. Allow a tiny slack for FIR group-delay rounding.
  CHECK(delta <= 2);
}
