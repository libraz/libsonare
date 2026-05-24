#pragma once

/// @file scale_quantizer.h
/// @brief 12-TET scale quantizer for pitch correction targets.

#include <cstdint>

namespace sonare::analysis::pitch_editor {

struct ScaleQuantizerConfig {
  int root = 0;
  uint16_t mode_mask = 0b101010110101;  // major scale, C as bit 0
  float reference_midi = 69.0f;
};

class ScaleQuantizer {
 public:
  explicit ScaleQuantizer(ScaleQuantizerConfig config = {});

  float quantize_midi(float midi) const noexcept;
  float correction_semitones(float midi) const noexcept { return quantize_midi(midi) - midi; }
  bool pitch_class_enabled(int pitch_class) const noexcept;

 private:
  ScaleQuantizerConfig config_{};
};

}  // namespace sonare::analysis::pitch_editor
