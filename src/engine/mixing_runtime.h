#pragma once

/// @file mixing_runtime.h
/// @brief Engine adapter for realtime ChannelStrip processing and automation.

#include <cstdint>

#include "mixing/channel_strip.h"
#include "rt/processor_base.h"

namespace sonare::engine {

class MixingRuntime final : public rt::ProcessorBase {
 public:
  enum ParamId : unsigned int {
    kFaderDb = 1,
    kPan = 2,
    kWidth = 3,
  };

  bool bind(mixing::ChannelStrip* strip) noexcept;

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void process_at(float* const* channels, int num_channels, int num_samples,
                  int64_t timeline_sample) noexcept;
  void reset() override;
  int latency_samples() const noexcept override;
  int latency_samples_q8() const noexcept override;
  bool set_parameter(unsigned int param_id, float value) override;
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override;

  mixing::ChannelStrip* strip() const noexcept { return strip_; }

 private:
  mixing::ChannelStrip* strip_ = nullptr;
};

}  // namespace sonare::engine
