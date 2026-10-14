#include "mastering/maximizer/true_peak_limiter.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "rt/tail_budget.h"

namespace {

template <typename Processor>
void process_mono(Processor& processor, std::vector<float>& samples) {
  constexpr size_t kBlockSize = 256;
  for (size_t offset = 0; offset < samples.size(); offset += kBlockSize) {
    const int count = static_cast<int>(std::min(kBlockSize, samples.size() - offset));
    float* channel = samples.data() + offset;
    processor.process(&channel, 1, count);
  }
}

template <typename Processor>
void check_tail(Processor& processor, const std::string& name) {
  processor.prepare(48000, 256);
  std::vector<float> impulse(256, 0.0f);
  impulse.back() = 0.5f;
  process_mono(processor, impulse);
  const int latency = processor.latency_samples();
  std::vector<float> latency_drain(static_cast<size_t>(latency), 0.0f);
  if (!latency_drain.empty()) process_mono(processor, latency_drain);
  const int reported = processor.tail_samples();
  INFO(name);
  REQUIRE(reported >= 0);
  REQUIRE(reported < sonare::rt::TailBudget::kUnbounded);
  std::vector<float> drain(static_cast<size_t>(reported), 0.0f);
  if (!drain.empty()) process_mono(processor, drain);
  std::vector<float> post(8192, 0.0f);
  process_mono(processor, post);
  float peak = 0.0f;
  for (float sample : post) peak = std::max(peak, std::abs(sample));
  REQUIRE(peak <= 1.0e-7f);
  REQUIRE(processor.non_finite_discard_count() == 0u);
}

}  // namespace

TEST_CASE("TruePeakLimiter reports a finite streaming tail", "[mastering][streaming][boundary]") {
  sonare::mastering::maximizer::TruePeakLimiterConfig config;
  config.ceiling_db = -12.0f;
  config.lookahead_ms = 1.0f;
  config.release_ms = 50.0f;
  config.oversample_factor = 4;
  config.apply_gain_at_input_rate = false;
  sonare::mastering::maximizer::TruePeakLimiter processor(config);
  check_tail(processor, "TruePeakLimiter");
}
