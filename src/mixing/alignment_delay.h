#pragma once

/// @file alignment_delay.h
/// @brief Integer-sample channel alignment delay.

#include <vector>

#include "rt/delay_line.h"
#include "rt/processor_base.h"

namespace sonare::mixing {

class AlignmentDelay : public rt::ProcessorBase {
 public:
  explicit AlignmentDelay(int delay_samples = 0);

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  int latency_samples() const noexcept override { return delay_samples_; }

  void set_delay_samples(int delay_samples);
  int delay_samples() const noexcept { return delay_samples_; }

 private:
  int delay_samples_ = 0;
  int prepared_channels_ = 0;
  std::vector<rt::DelayLine> delays_;
};

}  // namespace sonare::mixing
