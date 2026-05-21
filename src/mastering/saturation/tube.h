#pragma once

#include "mastering/saturation/waveshaper.h"

namespace sonare::mastering::saturation {

struct TubeConfig {
  float drive_db = 6.0f;
  float bias = 0.15f;
  float mix = 1.0f;
};

class Tube : public Waveshaper {
 public:
  explicit Tube(TubeConfig config = {});
  void set_config(const TubeConfig& config);
  const TubeConfig& tube_config() const { return tube_config_; }

 private:
  static WaveshaperConfig to_waveshaper(TubeConfig config);
  static void validate_config(const TubeConfig& config);
  TubeConfig tube_config_{};
};

}  // namespace sonare::mastering::saturation
