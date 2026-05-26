#pragma once

/// @file streaming_preview.h
/// @brief Streaming loudness normalization preview.

#include <string>
#include <vector>

#include "core/audio.h"

namespace sonare::mastering::maximizer {

struct StreamingPlatform {
  std::string name;
  float target_lufs = -14.0f;
  float ceiling_db = -1.0f;
};

struct StreamingPreviewResult {
  std::string name;
  float integrated_lufs = 0.0f;
  float true_peak_db = 0.0f;
  float normalization_gain_db = 0.0f;
  bool ceiling_risk = false;
};

std::vector<StreamingPreviewResult> streaming_preview(
    const Audio& audio, const std::vector<StreamingPlatform>& platforms = {
                            {"Spotify", -14.0f, -1.0f},
                            {"Apple Music", -16.0f, -1.0f},
                            {"YouTube", -14.0f, -1.0f},
                        });

std::string streaming_preview_to_json(const std::vector<StreamingPreviewResult>& results);

}  // namespace sonare::mastering::maximizer
