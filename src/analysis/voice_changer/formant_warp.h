#pragma once

/// @file formant_warp.h
/// @brief Lightweight formant-colour warp using LPC analysis context.

#include "core/audio.h"

namespace sonare::analysis::voice_changer {

struct FormantWarpConfig {
  float factor = 1.0f;
  int lpc_order = 12;
  float amount = 1.0f;
};

class FormantWarp {
 public:
  explicit FormantWarp(FormantWarpConfig config = {});

  Audio process(const Audio& audio) const;
  const FormantWarpConfig& config() const noexcept { return config_; }

 private:
  FormantWarpConfig config_{};
};

}  // namespace sonare::analysis::voice_changer
