#pragma once

#include "util/parameter_checks.h"

namespace sonare::mastering::api {

inline constexpr const char* kMasterAudioSelectorRelation = "{give_either} {0} or {1}";
inline constexpr const char* kMasterAudioPresetSubject = "preset";
inline constexpr const char* kMasterAudioOverridesSubject = "overrides";

/// @brief A named preset or an explicit overrides object must supply the chain.
inline void require_master_audio_selector(
    bool has_preset, bool has_overrides,
    const char* overrides_subject = kMasterAudioOverridesSubject) {
  if (!has_preset && !has_overrides) {
    util::check::refuse_relation({kMasterAudioPresetSubject, overrides_subject},
                                 kMasterAudioSelectorRelation);
  }
}

}  // namespace sonare::mastering::api
