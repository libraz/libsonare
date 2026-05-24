#pragma once

/// @file bus.h
/// @brief Summing bus primitive for subgroup, aux and master buses.

#include <vector>

#include "rt/processor_base.h"

namespace sonare::mixing {

enum class BusRole {
  Subgroup,
  Aux,
  Master,
};

class BusProcessor : public rt::ProcessorBase {
 public:
  explicit BusProcessor(BusRole role = BusRole::Subgroup, int max_inputs = 0);

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override {}

  void sum_inputs(const std::vector<float* const*>& inputs, float* const* output, int num_channels,
                  int num_samples) const;

  BusRole role() const noexcept { return role_; }
  int max_inputs() const noexcept { return max_inputs_; }

 private:
  BusRole role_ = BusRole::Subgroup;
  int max_inputs_ = 0;
};

}  // namespace sonare::mixing
