#include "mastering/saturation/transformer.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

namespace {

template <typename Processor>
void process_mono(Processor& processor, std::vector<float>& samples, int block_size = 256) {
  for (size_t offset = 0; offset < samples.size(); offset += static_cast<size_t>(block_size)) {
    const int count = static_cast<int>(
        std::min<size_t>(static_cast<size_t>(block_size), samples.size() - offset));
    float* channels[] = {samples.data() + offset};
    processor.process(channels, 1, count);
  }
}

}  // namespace

TEST_CASE("Transformer reports remanence in its tail budget", "[mastering][saturation][boundary]") {
  using sonare::mastering::saturation::Transformer;
  using sonare::mastering::saturation::TransformerConfig;
  TransformerConfig config;
  config.drive_db = 0.0f;
  config.asymmetry = 0.0f;
  config.mix = 1.0f;
  Transformer transformer(config);
  transformer.prepare(48000.0, 256);

  std::vector<float> impulse(256, 0.0f);
  impulse.front() = 0.01f;
  process_mono(transformer, impulse);
  std::vector<float> continuation(8192, 0.0f);
  process_mono(transformer, continuation);
  int last_residual = -1;
  for (size_t i = 0; i < continuation.size(); ++i) {
    if (std::abs(continuation[i]) > 1.0e-5f) last_residual = static_cast<int>(i);
  }
  REQUIRE(last_residual > 0);
  const int reported_tail = transformer.tail_samples();
  REQUIRE(reported_tail < sonare::rt::TailBudget::kUnbounded);
  REQUIRE(reported_tail >= last_residual);
  REQUIRE(transformer.non_finite_discard_count() == 0u);
}

TEST_CASE("Transformer tail boundary follows mix and idle bias",
          "[mastering][saturation][boundary]") {
  using sonare::mastering::saturation::Transformer;
  using sonare::mastering::saturation::TransformerConfig;

  TransformerConfig biased_config;
  biased_config.asymmetry = 0.25f;
  Transformer biased(biased_config);
  biased.prepare(48000.0, 256);
  REQUIRE(biased.tail_samples() == sonare::rt::TailBudget::kUnbounded);

  biased_config.mix = 0.0f;
  Transformer bypassed(biased_config);
  bypassed.prepare(48000.0, 256);
  REQUIRE(bypassed.tail_samples() == 0);
}
