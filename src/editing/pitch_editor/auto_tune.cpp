/// @file auto_tune.cpp
/// @brief Offline auto-tune built from the key detector, the scale table and the corrector.

#include "editing/pitch_editor/auto_tune.h"

#include <utility>

#include "analysis/analysis_rate.h"
#include "analysis/key_profiles.h"
#include "editing/pitch_editor/f0_provider.h"

namespace sonare::editing::pitch_editor {

AutoTuneResult auto_tune(const Audio& audio, const std::optional<Key>& key,
                         PitchCorrectionConfig config) {
  Key used = key ? *key : detect_key(analysis_rate_audio(audio));
  if (key) used.confidence = 1.0f;
  config.scale.root = static_cast<int>(used.root);
  config.scale.mode_mask = scale_mask_for_mode(used.mode);

  PyinF0Provider provider;
  const F0Track track = provider.detect(audio);
  Audio corrected = PitchCorrector(config).correct_to_scale_timevarying(audio, track);
  return {std::move(corrected), used};
}

}  // namespace sonare::editing::pitch_editor
