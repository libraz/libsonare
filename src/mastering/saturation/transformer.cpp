#include "mastering/saturation/transformer.h"

#include <stdexcept>

namespace sonare::mastering::saturation {

Transformer::Transformer(TransformerConfig config)
    : Waveshaper(to_waveshaper(config)), transformer_config_(config) {
  validate_config(transformer_config_);
}

void Transformer::set_config(const TransformerConfig& config) {
  validate_config(config);
  transformer_config_ = config;
  Waveshaper::set_config(to_waveshaper(config));
}

WaveshaperConfig Transformer::to_waveshaper(TransformerConfig config) {
  return {config.drive_db, config.mix, 0.0f, config.asymmetry, WaveshaperCurve::Asymmetric};
}

void Transformer::validate_config(const TransformerConfig& config) {
  if (config.mix < 0.0f || config.mix > 1.0f) {
    throw std::invalid_argument("transformer mix must be in [0, 1]");
  }
}

}  // namespace sonare::mastering::saturation
