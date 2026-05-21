#pragma once

#include "mastering/saturation/waveshaper.h"

namespace sonare::mastering::saturation {

struct TransformerConfig {
  float drive_db = 4.0f;
  float asymmetry = 0.1f;
  float mix = 1.0f;
};

class Transformer : public Waveshaper {
 public:
  explicit Transformer(TransformerConfig config = {});
  void set_config(const TransformerConfig& config);
  const TransformerConfig& transformer_config() const { return transformer_config_; }

 private:
  static WaveshaperConfig to_waveshaper(TransformerConfig config);
  static void validate_config(const TransformerConfig& config);
  TransformerConfig transformer_config_{};
};

}  // namespace sonare::mastering::saturation
