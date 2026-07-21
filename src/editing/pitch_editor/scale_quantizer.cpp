#include "editing/pitch_editor/scale_quantizer.h"

#include <algorithm>
#include <cmath>

namespace sonare::editing::pitch_editor {

ScaleQuantizer::ScaleQuantizer(ScaleQuantizerConfig config) : config_(config) {}

float ScaleQuantizer::quantize_midi(float midi) const noexcept {
  // Non-finite input has no meaningful quantization and would make the int cast
  // below undefined; pass it through unchanged.
  if (!std::isfinite(midi)) {
    return midi;
  }
  // Beyond the representable MIDI range the candidate loop's int cast would
  // overflow. No scale context exists that far out, so snap chromatically.
  constexpr float kMaxQuantizableMidi = 2048.0f;
  if (std::abs(midi) > kMaxQuantizableMidi) {
    return std::round(midi);
  }

  // An empty scale (no pitch class enabled) is treated as an explicit chromatic
  // pass-through: snap to the nearest semitone. Documented here so the fallback
  // is intentional rather than a silent side effect of the candidate loop never
  // finding an enabled pitch class.
  if (config_.mode_mask == 0) {
    return std::round(midi);
  }

  float best = std::round(midi);
  float best_distance = 1.0e9f;
  const int center = static_cast<int>(std::round(midi));
  for (int candidate = center - 12; candidate <= center + 12; ++candidate) {
    const int pc = ((candidate % 12) + 12) % 12;
    if (!pitch_class_enabled(pc)) {
      continue;
    }
    const float distance = std::abs(static_cast<float>(candidate) - midi);
    if (distance < best_distance) {
      best_distance = distance;
      best = static_cast<float>(candidate);
    }
  }
  return best;
}

bool ScaleQuantizer::pitch_class_enabled(int pitch_class) const noexcept {
  const int normalized_pc = ((pitch_class % 12) + 12) % 12;
  const int normalized_root = ((config_.root % 12) + 12) % 12;
  const int relative = (normalized_pc - normalized_root + 12) % 12;
  return (config_.mode_mask & (1U << relative)) != 0;
}

}  // namespace sonare::editing::pitch_editor
