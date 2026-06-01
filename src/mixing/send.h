#pragma once

/// @file send.h
/// @brief Aux send gain primitive.

#include <atomic>

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

  // timing_ is read on the audio thread (mix_send_at) and may be written by the
  // control thread (set_timing); an atomic load/store with relaxed ordering
  // avoids a data race. The value is a self-contained enum tag, so no ordering
  // relative to other state is required.
  SendTiming timing() const noexcept { return timing_.load(std::memory_order_relaxed); }
  void set_timing(SendTiming timing) noexcept { timing_.store(timing, std::memory_order_relaxed); }

 private:
  GainProcessor gain_;
  std::atomic<SendTiming> timing_{SendTiming::PostFader};
};

}  // namespace sonare::mixing
