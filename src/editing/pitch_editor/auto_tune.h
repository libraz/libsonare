#pragma once

/// @file auto_tune.h
/// @brief Offline auto-tune: key (detected or named) -> scale mask -> pYIN -> scale correction.

#include <optional>

#include "analysis/key_analyzer.h"
#include "core/audio.h"
#include "editing/pitch_editor/pitch_corrector.h"

namespace sonare::editing::pitch_editor {

/// @brief Corrected audio and the key it was tuned to.
struct AutoTuneResult {
  Audio audio;  ///< Same length as the input
  Key key;      ///< The detected key, or the named one with confidence 1
};

/// @brief Snaps the voiced pitch of a monophonic recording to a key's scale.
/// @details Pitch is tracked with pYIN at its default 2048-sample frame and
///          512-sample hop. The scale comes from @ref scale_mask_for_mode of the
///          key's mode, rooted at the key's root; @p config supplies the
///          correction knobs and its @c scale.root / @c scale.mode_mask are
///          overwritten.
/// @param audio Mono input
/// @param key The key to tune to, or empty to detect it with the default
///        @ref detect_key configuration at the analysis rate
/// @param config Correction knobs (strength, glide, vibrato bypass, reference)
AutoTuneResult auto_tune(const Audio& audio, const std::optional<Key>& key,
                         PitchCorrectionConfig config = {});

}  // namespace sonare::editing::pitch_editor
