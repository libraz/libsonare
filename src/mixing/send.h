#pragma once

/// @file send.h
/// @brief Aux send gain primitive.

#include "mixing/gain.h"

namespace sonare::mixing {

enum class SendTiming {
  PreFader,
  PostFader,
};

struct SendConfig {
  float send_db = 0.0f;
  SendTiming timing = SendTiming::PostFader;
  float smoothing_ms = 5.0f;
};

class SendProcessor : public rt::ProcessorBase {
 public:
  explicit SendProcessor(SendConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_send_db(float send_db) noexcept;
  float send_db() const noexcept { return gain_.gain_db(); }

  SendTiming timing() const noexcept { return timing_; }
  void set_timing(SendTiming timing) noexcept { timing_ = timing; }

 private:
  GainProcessor gain_;
  SendTiming timing_ = SendTiming::PostFader;
};

}  // namespace sonare::mixing
