#pragma once

#include "core/audio.h"

namespace sonare::mastering::repair {

struct DereverbClassicalConfig {
  float threshold = 0.05f;
  float attenuation = 0.5f;
  int n_fft = 1024;
  int hop_length = 256;
  float t60_sec = 0.4f;
  float late_delay_ms = 50.0f;
  float over_subtraction = 1.0f;
  float spectral_floor = 0.08f;
  bool wpe_enabled = false;
  int wpe_iterations = 2;
  int wpe_taps = 3;
  float wpe_strength = 0.7f;
};

Audio dereverb_classical(const Audio& audio, const DereverbClassicalConfig& config = {});

}  // namespace sonare::mastering::repair
