#pragma once

/// @file fx_bus.h
/// @brief Aux FX bus with an ordered insert chain.

#include <memory>

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
  int latency_samples_q8() const noexcept override;
  int tail_samples() const noexcept override;

  void add_insert(std::unique_ptr<rt::ProcessorBase> processor);
  size_t num_inserts() const noexcept { return bus_.num_inserts(); }
  void set_insert_sidechain(unsigned int insert_index, const float* const* channels,
                            int num_channels, int num_samples);
  void clear_insert_sidechains() noexcept;
  BusProcessor& bus() noexcept { return bus_; }

 private:
  BusProcessor bus_;
};

}  // namespace sonare::mixing
