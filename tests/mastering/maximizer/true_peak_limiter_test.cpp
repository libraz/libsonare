#include "mastering/maximizer/true_peak_limiter.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "mastering/maximizer/adaptive_release.h"
#include "metering/true_peak.h"
#include "rt/tail_budget.h"
#include "util/constants.h"
#include "util/db.h"

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

TEST_CASE("AdaptiveRelease reports the tail of its inner limiter",
          "[mastering][streaming][boundary]") {
  using sonare::mastering::maximizer::AdaptiveRelease;
  using sonare::mastering::maximizer::AdaptiveReleaseConfig;
  AdaptiveReleaseConfig config;
  config.ceiling_db = -12.0f;
  AdaptiveRelease defaults(config);
  check_tail(defaults, "AdaptiveRelease default");
  config.lookahead_ms = 5.0f;
  AdaptiveRelease lookahead(config);
  check_tail(lookahead, "AdaptiveRelease lookaheadMs=5");
}

TEST_CASE("TruePeakLimiter bounds the detector-excluded plane without letting it steer the gain",
          "[mastering][maximizer]") {
  using sonare::mastering::maximizer::TruePeakLimiter;
  using sonare::mastering::maximizer::TruePeakLimiterConfig;
  constexpr int kChannels = 4;
  constexpr int kExcluded = 3;
  constexpr int kBlock = 256;
  using sonare::constants::kTwoPi;

  for (const int rate : {44100, 48000}) {
    for (const bool detect_only : {false, true}) {
      CAPTURE(rate, detect_only);
      TruePeakLimiterConfig config;
      config.ceiling_db = -1.0f;
      config.apply_gain_at_input_rate = detect_only;
      const float ceiling = sonare::db_to_linear(config.ceiling_db);
      const auto run = [&](float lfe_amplitude, std::vector<float>* reductions) {
        TruePeakLimiter limiter(config);
        limiter.prepare(rate, kBlock);
        limiter.set_detector_excluded_channel(kExcluded);
        const int total = rate / 2 + limiter.latency_samples();
        std::vector<std::vector<float>> planes(kChannels, std::vector<float>(total, 0.0f));
        for (int i = 0; i < rate / 2; ++i) {
          const float t = static_cast<float>(i) / static_cast<float>(rate);
          for (int ch = 0; ch < kExcluded; ++ch) {
            planes[ch][i] = 1.5f * std::sin(kTwoPi * (330.0f + 110.0f * ch) * t);
          }
          planes[kExcluded][i] = lfe_amplitude * std::sin(kTwoPi * 60.0f * t);
        }
        for (int offset = 0; offset < total; offset += kBlock) {
          const int count = std::min(kBlock, total - offset);
          float* block[kChannels];
          for (int ch = 0; ch < kChannels; ++ch) block[ch] = planes[ch].data() + offset;
          limiter.process(block, kChannels, count);
          reductions->push_back(limiter.last_gain_reduction_db());
        }
        return planes;
      };
      std::vector<float> quiet_reductions;
      std::vector<float> hot_reductions;
      const auto quiet = run(0.0f, &quiet_reductions);
      const auto hot = run(4.0f, &hot_reductions);

      // The excluded plane steers neither the linked gain nor the reported reduction.
      for (int ch = 0; ch < kExcluded; ++ch) CHECK(hot[ch] == quiet[ch]);
      CHECK(hot_reductions == quiet_reductions);
      CHECK(*std::min_element(hot_reductions.begin(), hot_reductions.end()) < -1.0f);

      // Every plane, the excluded one included, reads at or below the ceiling; the
      // 0.1 dB allowance is the residue the limiter's own ceiling test accepts.
      const float allowed = ceiling * sonare::db_to_linear(0.1f);
      for (int ch = 0; ch < kChannels; ++ch) {
        CAPTURE(ch);
        CHECK(sonare::metering::true_peak(hot[ch].data(), hot[ch].size(), 4) <= allowed);
      }
    }
  }
}
