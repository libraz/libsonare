#pragma once

/// @file gain_processor.h
/// @brief Trivial pass-through and gain graph processors shared by the bindings.

#include <vector>

#include "rt/processor_base.h"
#include "util/db.h"

namespace sonare::rt {

/// @brief Pass-through graph node: forwards its input unchanged.
class PassProcessor final : public ProcessorBase {
 public:
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
};

/// @brief Fixed-gain graph node. Parameter 0 is the gain in dB (realtime-safe).
class GainProcessor final : public ProcessorBase {
 public:
  explicit GainProcessor(float gain_db) : gain_(db_to_linear(gain_db)) {}
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    if (!channels || num_channels <= 0 || num_samples <= 0) return;
    for (int ch = 0; ch < num_channels; ++ch) {
      if (!channels[ch]) continue;
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] *= gain_;
      }
    }
  }
  void reset() override {}
  bool set_parameter(unsigned int, float value) override {
    // GainProcessor exposes a single gain parameter. The graph/automation layer
    // routes by node binding and forwards its own external param_id (which is not
    // a node-local index), so every id maps to this one gain control.
    gain_ = db_to_linear(value);
    return true;
  }
  // Automatable parameters: 0=gainDb
  std::vector<ParamDescriptor> parameter_descriptors() const override { return {{"gainDb", 0}}; }
  bool parameter_is_realtime_safe(unsigned int) const noexcept override { return true; }

 private:
  float gain_ = 1.0f;
};

}  // namespace sonare::rt
