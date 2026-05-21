#include "mastering/saturation/tube.h"

#include <stdexcept>

namespace sonare::mastering::saturation {

Tube::Tube(TubeConfig config) : Waveshaper(to_waveshaper(config)), tube_config_(config) {
  validate_config(tube_config_);
}

void Tube::set_config(const TubeConfig& config) {
  validate_config(config);
  tube_config_ = config;
  Waveshaper::set_config(to_waveshaper(config));
}

WaveshaperConfig Tube::to_waveshaper(TubeConfig config) {
  return {config.drive_db, config.mix, 0.0f, config.bias, WaveshaperCurve::Asymmetric};
}

void Tube::validate_config(const TubeConfig& config) {
  if (config.mix < 0.0f || config.mix > 1.0f) {
    throw std::invalid_argument("tube mix must be in [0, 1]");
  }
}

}  // namespace sonare::mastering::saturation
