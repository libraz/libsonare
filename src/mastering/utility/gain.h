#pragma once

/// @file gain.h
/// @brief Broadband level trim as an insert.
///
/// The one thing a chain cannot express by composing the other inserts: a
/// level with no tone, no width and no dynamics attached to it. Every family
/// beside this one changes the signal in some second way as well, so a chain
/// needing only a gain had to borrow a processor and neutralize the rest of it.

#include <vector>

#include "rt/processor_base.h"

namespace sonare::mastering::utility {

struct GainConfig {
  float level_db = 0.0f;
};

class Gain : public rt::ProcessorBase {
 public:
  explicit Gain(GainConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const GainConfig& config);
  const GainConfig& config() const { return config_; }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = levelDb
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=levelDb
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const GainConfig& config);

  GainConfig config_{};
  bool prepared_ = false;
};

}  // namespace sonare::mastering::utility
