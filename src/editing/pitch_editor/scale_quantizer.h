#pragma once

/// @file scale_quantizer.h
/// @brief 12-TET scale quantizer for pitch correction targets.

#include <cstdint>

#include "util/constants.h"

namespace sonare::editing::pitch_editor {

struct ScaleQuantizerConfig {
  int root = 0;
  uint16_t mode_mask = 0b101010110101;  // major scale, C as bit 0
  float reference_midi = constants::kMidiA4;
};

inline bool valid_scale_args(int root, uint16_t mode_mask) noexcept {
  return root >= 0 && root <= 11 && mode_mask != 0 && (mode_mask & ~uint16_t{0x0FFF}) == 0;
}

class ScaleQuantizer {
 public:
  explicit ScaleQuantizer(ScaleQuantizerConfig config = {});

  /// Quantizes to the configured scale grid. Equidistant ties select the lower
  /// pitch. reference_midi may be fractional and shifts the entire 12-TET grid
  /// (69.25 anchors A4 at MIDI 69.25, for example).
  float quantize_midi(float midi) const noexcept;
  float correction_semitones(float midi) const noexcept { return quantize_midi(midi) - midi; }
  bool pitch_class_enabled(int pitch_class) const noexcept;

 private:
  ScaleQuantizerConfig config_{};
};

}  // namespace sonare::editing::pitch_editor
