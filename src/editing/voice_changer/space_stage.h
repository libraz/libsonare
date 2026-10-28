#pragma once

/// @file space_stage.h
/// @brief Reverb and echo-family space stage.

#include "editing/voice_changer/streaming_reverb.h"
#include "editing/voice_changer/voice_transform_types.h"

namespace sonare::editing::voice_changer {

class SpaceStage {
 public:
  void prepare(double sr);
  void reset();
  void set_config(const StreamingReverbConfig&, const ReverbVariationConfig&);
  float process_sample(float x) noexcept;
};

}  // namespace sonare::editing::voice_changer
