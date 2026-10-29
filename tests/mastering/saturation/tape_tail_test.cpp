#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "mastering/saturation/tape.h"
#include "rt/tail_budget.h"

namespace {

using sonare::mastering::saturation::Tape;
using sonare::mastering::saturation::TapeConfig;

void process_mono(Tape& tape, std::vector<float>& samples, int block_size = 256) {
  for (size_t offset = 0; offset < samples.size(); offset += static_cast<size_t>(block_size)) {
    const int count = static_cast<int>(
        std::min<size_t>(static_cast<size_t>(block_size), samples.size() - offset));
    float* channels[] = {samples.data() + offset};
    tape.process(channels, 1, count);
  }
}

float peak(const std::vector<float>& samples, size_t begin, size_t end) {
  float result = 0.0f;
  for (size_t i = begin; i < std::min(end, samples.size()); ++i) {
    result = std::max(result, std::abs(samples[i]));
  }
  return result;
}

void require_tail_covers_impulse(TapeConfig config, double sample_rate, float amplitude) {
  constexpr int kBlock = 256;
  Tape tape(config);
  tape.prepare(sample_rate, kBlock, 1);
  const int reported_tail = tape.tail_samples();
  const int latency = tape.latency_samples();
  INFO("rate=" << sample_rate << ", factor=" << config.oversample_factor << ", amplitude="
               << amplitude << ", latency=" << latency << ", tail=" << reported_tail);
  REQUIRE(reported_tail >= 0);
  REQUIRE(reported_tail < sonare::rt::TailBudget::kUnbounded);
  REQUIRE(reported_tail < static_cast<int>(2.0 * sample_rate));

  std::vector<float> impulse{amplitude};
  process_mono(tape, impulse);
  const size_t boundary = static_cast<size_t>(latency + reported_tail);
  std::vector<float> continuation(boundary + static_cast<size_t>(sample_rate), 0.0f);
  process_mono(tape, continuation);
  REQUIRE(std::all_of(continuation.begin(), continuation.end(),
                      [](float sample) { return std::isfinite(sample); }));
  const float audible_tail = peak(continuation, 0, boundary);
  const float after_reported_tail = peak(continuation, boundary, continuation.size());
  INFO("audible tail peak=" << audible_tail
                            << ", residual after latency+tail=" << after_reported_tail);
  REQUIRE(audible_tail > sonare::rt::kTailFloor);
  REQUIRE(after_reported_tail <= sonare::rt::kTailFloor);
}

}  // namespace

TEST_CASE("Tape tail covers Jiles-Atherton remanence across rates and oversampling",
          "[tape-tail]") {
  const std::array<double, 2> rates{44100.0, 48000.0};
  const std::array<int, 3> factors{1, 2, 4};
  const std::array<float, 2> levels{0.01f, 1.0f};
  for (double rate : rates) {
    for (int factor : factors) {
      for (float amplitude : levels) {
        DYNAMIC_SECTION("rate=" << rate << " factor=" << factor << " amplitude=" << amplitude) {
          TapeConfig config;
          config.oversample_factor = factor;
          require_tail_covers_impulse(config, rate, amplitude);
        }
      }
    }
  }
}

TEST_CASE("Tape bias produces an unbounded nonzero idle tail", "[tape-tail]") {
  for (float bias : {-0.1f, 0.1f}) {
    DYNAMIC_SECTION("bias=" << bias) {
      TapeConfig config;
      config.bias = bias;
      Tape tape(config);
      tape.prepare(48000.0, 256, 1);
      REQUIRE(tape.tail_samples() == sonare::rt::TailBudget::kUnbounded);

      std::vector<float> idle(48000, 0.0f);
      process_mono(tape, idle);
      REQUIRE(std::all_of(idle.begin(), idle.end(),
                          [](float sample) { return std::isfinite(sample); }));
      const float late_level = peak(idle, idle.size() - 1024, idle.size());
      INFO("late biased output peak=" << late_level);
      REQUIRE(late_level > sonare::rt::kTailFloor);
    }
  }
}

TEST_CASE("Tape returns to a finite tail after bias is removed", "[tape-tail]") {
  Tape parameter_path;
  parameter_path.prepare(48000.0, 256, 1);
  REQUIRE(parameter_path.set_parameter(6, 0.1f));
  REQUIRE(parameter_path.tail_samples() == sonare::rt::TailBudget::kUnbounded);
  REQUIRE(parameter_path.set_parameter(6, 0.0f));
  CHECK(parameter_path.tail_samples() < sonare::rt::TailBudget::kUnbounded);

  TapeConfig config;
  config.bias = -0.1f;
  Tape config_path(config);
  config_path.prepare(48000.0, 256, 1);
  REQUIRE(config_path.tail_samples() == sonare::rt::TailBudget::kUnbounded);
  config.bias = 0.0f;
  config_path.set_config(config);
  CHECK(config_path.tail_samples() < sonare::rt::TailBudget::kUnbounded);
}

TEST_CASE("Tape tail includes output and head-bump gain", "[tape-tail]") {
  TapeConfig config;
  config.drive_db = 0.0f;
  config.output_gain_db = 12.0f;
  config.head_bump_db = 6.0f;
  config.oversample_factor = 2;
  require_tail_covers_impulse(config, 48000.0, 0.01f);
}
