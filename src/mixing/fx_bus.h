#pragma once

/// @file fx_bus.h
/// @brief Aux FX bus with an ordered insert chain.

#include <memory>
#include <string>

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

  /// Appends an insert. @p stereo_pair_only forwards the catalog channelPolicy
  /// to BusProcessor so the insert is front-pair-only on a surround bus.
  void add_insert(std::unique_ptr<rt::ProcessorBase> processor, bool stereo_pair_only = false);
  size_t num_inserts() const noexcept { return bus_.num_inserts(); }
  /// Audio-thread immediate insert-parameter change; forwards to BusProcessor.
  bool apply_insert_parameter(unsigned int insert_index, unsigned int param_id,
                              float value) noexcept {
    return bus_.apply_insert_parameter(insert_index, param_id, value);
  }
  /// Control-thread JSON-key -> param_id resolution; forwards to BusProcessor.
  int insert_parameter_id_for_key(unsigned int insert_index,
                                  const std::string& key) const noexcept {
    return bus_.insert_parameter_id_for_key(insert_index, key);
  }
  /// Toggles bypass for an insert; forwards to BusProcessor.
  bool set_insert_bypassed(unsigned int insert_index, bool bypassed,
                           bool reset_on_bypass = false) noexcept {
    return bus_.set_insert_bypassed(insert_index, bypassed, reset_on_bypass);
  }
  void set_insert_sidechain(unsigned int insert_index, const float* const* channels,
                            int num_channels, int num_samples);
  void clear_insert_sidechains() noexcept;
  BusProcessor& bus() noexcept { return bus_; }

 private:
  BusProcessor bus_;
};

}  // namespace sonare::mixing
