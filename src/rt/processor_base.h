#pragma once

/// @file processor_base.h
/// @brief Base interface for stateful mastering processors.

namespace sonare::rt {

class ProcessorBase {
 public:
  virtual ~ProcessorBase() = default;

  virtual void prepare(double sample_rate, int max_block_size) = 0;
  virtual void process(float* const* channels, int num_channels, int num_samples) = 0;
  virtual void reset() = 0;
  virtual int latency_samples() const noexcept { return 0; }
  virtual int latency_samples_q8() const noexcept { return latency_samples() << 8; }
  virtual float last_gain_reduction_db() const { return 0.0f; }
};

}  // namespace sonare::rt
