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

/// @brief Multi-channel counterpart preserving BS.1770 channel summing.
/// @details The normalization gain and the ceiling-risk flag are both derived
///          from the integrated loudness, so measuring a `0.5 * (L + R)` downmix
///          instead of the channel-summed program reads roughly 6 dB low on
///          decorrelated stereo and understates the risk by the same amount.
///          The reported true peak is the largest across the channels.
/// @param samples Pointer to `frames * channels` interleaved samples.
/// @param frames Number of sample frames.
/// @param channels Channel count; must be positive.
/// @param sample_rate Sample rate in Hz; must be positive.
/// @param platforms Platforms to preview; must not be empty.
std::vector<StreamingPreviewResult> streaming_preview_interleaved(
    const float* samples, std::size_t frames, int channels, int sample_rate,
    const std::vector<StreamingPlatform>& platforms = {
        {"Spotify", -14.0f, -1.0f},
        {"Apple Music", -16.0f, -1.0f},
        {"YouTube", -14.0f, -1.0f},
    });

std::string streaming_preview_to_json(const std::vector<StreamingPreviewResult>& results);

}  // namespace sonare::mastering::maximizer
