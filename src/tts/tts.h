#pragma once

/// @file tts.h
/// @brief Deterministic TTS-oriented audio utilities.

#include "core/audio.h"

namespace sonare::tts {

struct TtsQualityResult {
  float duration_sec = 0.0f;
  float peak_db = 0.0f;
  float rms_db = 0.0f;
  float silence_ratio = 0.0f;
  float clipping_ratio = 0.0f;
  float leading_silence_sec = 0.0f;
  float trailing_silence_sec = 0.0f;
};

TtsQualityResult analyze_tts_quality(const Audio& audio, float silence_threshold_db = -45.0f,
                                     int frame_length = 1024, int hop_length = 256);

Audio prepare_tts(const Audio& audio, float target_rms_db = -20.0f,
                  float silence_threshold_db = -45.0f, float peak_limit_db = -1.0f,
                  float fade_sec = 0.005f);

Audio compress_pauses(const Audio& audio, float max_pause_sec = 0.6f,
                      float silence_threshold_db = -45.0f);

}  // namespace sonare::tts

namespace sonare {

using tts::analyze_tts_quality;
using tts::compress_pauses;
using tts::prepare_tts;
using tts::TtsQualityResult;

}  // namespace sonare
