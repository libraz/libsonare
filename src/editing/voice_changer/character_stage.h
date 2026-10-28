#pragma once

/// @file character_stage.h
/// @brief Megaphone, radio, BBD chorus and strobe colouration stage.

#include "editing/voice_changer/voice_transform_types.h"

namespace sonare::editing::voice_changer {

class CharacterStage {
 public:
  void prepare(double sr);
  void reset();
  void set_config(const CharacterConfig&);
  float process_sample(float x) noexcept;
};

}  // namespace sonare::editing::voice_changer
