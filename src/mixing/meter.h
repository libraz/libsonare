#pragma once

/// @file meter.h
/// @brief Lightweight realtime meter snapshot processor.

#include <array>
#include <atomic>
#include <cstdint>

#include "rt/processor_base.h"

namespace sonare::mixing {

struct MeterSnapshot {
  std::array<float, 2> peak_db{{-120.0f, -120.0f}};
  std::array<float, 2> rms_db{{-120.0f, -120.0f}};
  float correlation = 0.0f;
  uint64_t seq = 0;
};

class MeterProcessor : public rt::ProcessorBase {
 public:
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  MeterSnapshot snapshot() const noexcept;

 private:
  std::array<MeterSnapshot, 2> snapshots_{};
  std::atomic<int> active_index_{0};
  uint64_t seq_ = 0;
};

}  // namespace sonare::mixing
