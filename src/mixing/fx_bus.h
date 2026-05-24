#pragma once

/// @file fx_bus.h
/// @brief Aux FX bus with an ordered insert chain.

#include <memory>
#include <vector>

#include "mixing/bus.h"
#include "rt/processor_base.h"

namespace sonare::mixing {

class FxBus : public rt::ProcessorBase {
 public:
  explicit FxBus(int max_inputs = 0);

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  int latency_samples() const noexcept override;

  void add_insert(std::unique_ptr<rt::ProcessorBase> processor);
  size_t num_inserts() const noexcept { return inserts_.size(); }
  BusProcessor& bus() noexcept { return bus_; }

 private:
  BusProcessor bus_;
  std::vector<std::unique_ptr<rt::ProcessorBase>> inserts_;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
};

}  // namespace sonare::mixing
