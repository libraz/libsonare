#include "analysis/pitch_editor/scale_quantizer.h"

#include <algorithm>
#include <cmath>

namespace sonare::analysis::pitch_editor {

ScaleQuantizer::ScaleQuantizer(ScaleQuantizerConfig config) : config_(config) {}

float ScaleQuantizer::quantize_midi(float midi) const noexcept {
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

}  // namespace sonare::analysis::pitch_editor
