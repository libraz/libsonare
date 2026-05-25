#pragma once

/// @file bus.h
/// @brief Summing bus primitive for subgroup, aux and master buses.

#include <memory>
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
  void reset() override;
  int latency_samples() const noexcept override;
  int latency_samples_q8() const noexcept override;

  void sum_inputs(const std::vector<float* const*>& inputs, float* const* output, int num_channels,
                  int num_samples) const;
  void add_insert(std::unique_ptr<rt::ProcessorBase> processor);
  size_t num_inserts() const noexcept { return inserts_.size(); }
  void set_insert_sidechain(unsigned int insert_index, const float* const* channels,
                            int num_channels, int num_samples);
  void clear_insert_sidechains() noexcept;

  BusRole role() const noexcept { return role_; }
  int max_inputs() const noexcept { return max_inputs_; }

 private:
  struct InsertSidechain {
    const float* const* channels = nullptr;
    int num_channels = 0;
    int num_samples = 0;
    bool managed = false;
  };

  BusRole role_ = BusRole::Subgroup;
  int max_inputs_ = 0;
  std::vector<std::unique_ptr<rt::ProcessorBase>> inserts_;
  std::vector<InsertSidechain> insert_sidechains_;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
};

}  // namespace sonare::mixing
