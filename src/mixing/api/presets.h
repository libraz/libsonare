#pragma once

/// @file presets.h
/// @brief Built-in mixer scene templates.

#include <string>
#include <vector>

#include "mixing/api/scene.h"

namespace sonare::mixing::api {

enum class ScenePreset {
  VocalReverbSend,
  DrumBusSubgroup,
  CommentaryDucking,
};

std::vector<std::string> scene_preset_names();
ScenePreset scene_preset_from_string(const std::string& name);
const char* scene_preset_to_string(ScenePreset preset) noexcept;
Scene scene_preset(ScenePreset preset);

}  // namespace sonare::mixing::api
