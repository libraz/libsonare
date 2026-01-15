/// @file sonare_c.cpp
/// @brief Implementation of C API.

#include "sonare_c.h"

#include <cstring>
#include <new>

#include "analysis/beat_analyzer.h"
#include "analysis/bpm_analyzer.h"
#include "analysis/key_analyzer.h"
#include "analysis/music_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "core/audio.h"
#include "quick.h"
#include "util/exception.h"

using namespace sonare;

// Internal wrapper structure
struct SonareAudio {
  Audio audio;
};

// Audio functions

/// @brief Minimum valid sample rate (8kHz - telephone quality)
constexpr int kMinSampleRate = 8000;
/// @brief Maximum valid sample rate (384kHz - high-res audio)
constexpr int kMaxSampleRate = 384000;
/// @brief Maximum buffer size (2GB / sizeof(float) = ~500M samples, ~6 hours at 22050Hz)
constexpr size_t kMaxBufferSize = 500000000;

SonareError sonare_audio_from_buffer(const float* data, size_t length, int sample_rate,
                                     SonareAudio** out) {
  if (data == nullptr || out == nullptr || length == 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (sample_rate < kMinSampleRate || sample_rate > kMaxSampleRate) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (length > kMaxBufferSize) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  try {
    *out = new SonareAudio{Audio::from_buffer(data, length, sample_rate)};
    return SONARE_OK;
  } catch (const std::bad_alloc&) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    return SONARE_ERROR_UNKNOWN;
  }
}

SonareError sonare_audio_from_memory(const uint8_t* data, size_t length, SonareAudio** out) {
  if (data == nullptr || out == nullptr || length == 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  try {
    *out = new SonareAudio{Audio::from_memory(data, length)};
    return SONARE_OK;
  } catch (const SonareException& e) {
    if (e.code() == ErrorCode::FileNotFound) {
      return SONARE_ERROR_FILE_NOT_FOUND;
    } else if (e.code() == ErrorCode::InvalidFormat) {
      return SONARE_ERROR_INVALID_FORMAT;
    } else if (e.code() == ErrorCode::DecodeFailed) {
      return SONARE_ERROR_DECODE_FAILED;
    }
    return SONARE_ERROR_UNKNOWN;
  } catch (const std::bad_alloc&) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    return SONARE_ERROR_UNKNOWN;
  }
}

#ifndef __EMSCRIPTEN__
SonareError sonare_audio_from_file(const char* path, SonareAudio** out) {
  if (path == nullptr || out == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  try {
    *out = new SonareAudio{Audio::from_file(path)};
    return SONARE_OK;
  } catch (const SonareException& e) {
    if (e.code() == ErrorCode::FileNotFound) {
      return SONARE_ERROR_FILE_NOT_FOUND;
    } else if (e.code() == ErrorCode::InvalidFormat) {
      return SONARE_ERROR_INVALID_FORMAT;
    } else if (e.code() == ErrorCode::DecodeFailed) {
      return SONARE_ERROR_DECODE_FAILED;
    }
    return SONARE_ERROR_UNKNOWN;
  } catch (const std::bad_alloc&) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    return SONARE_ERROR_UNKNOWN;
  }
}
#endif

void sonare_audio_free(SonareAudio* audio) { delete audio; }

const float* sonare_audio_data(const SonareAudio* audio) {
  if (audio == nullptr) {
    return nullptr;
  }
  return audio->audio.data();
}

size_t sonare_audio_length(const SonareAudio* audio) {
  if (audio == nullptr) {
    return 0;
  }
  return audio->audio.size();
}

int sonare_audio_sample_rate(const SonareAudio* audio) {
  if (audio == nullptr) {
    return 0;
  }
  return audio->audio.sample_rate();
}

float sonare_audio_duration(const SonareAudio* audio) {
  if (audio == nullptr) {
    return 0.0f;
  }
  return audio->audio.duration();
}

// Quick detection functions

SonareError sonare_detect_bpm(const float* samples, size_t length, int sample_rate,
                              float* out_bpm) {
  if (samples == nullptr || out_bpm == nullptr || length == 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (sample_rate < kMinSampleRate || sample_rate > kMaxSampleRate) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (length > kMaxBufferSize) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  try {
    *out_bpm = quick::detect_bpm(samples, length, sample_rate);
    return SONARE_OK;
  } catch (...) {
    return SONARE_ERROR_UNKNOWN;
  }
}

SonareError sonare_detect_key(const float* samples, size_t length, int sample_rate,
                              SonareKey* out_key) {
  if (samples == nullptr || out_key == nullptr || length == 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (sample_rate < kMinSampleRate || sample_rate > kMaxSampleRate) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (length > kMaxBufferSize) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  try {
    Key key = quick::detect_key(samples, length, sample_rate);
    out_key->root = static_cast<SonarePitchClass>(key.root);
    out_key->mode = static_cast<SonareMode>(key.mode);
    out_key->confidence = key.confidence;
    return SONARE_OK;
  } catch (...) {
    return SONARE_ERROR_UNKNOWN;
  }
}

SonareError sonare_detect_beats(const float* samples, size_t length, int sample_rate,
                                float** out_times, size_t* out_count) {
  if (samples == nullptr || out_times == nullptr || out_count == nullptr || length == 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (sample_rate < kMinSampleRate || sample_rate > kMaxSampleRate) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (length > kMaxBufferSize) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  try {
    std::vector<float> beats = quick::detect_beats(samples, length, sample_rate);
    *out_count = beats.size();
    if (beats.empty()) {
      *out_times = nullptr;
    } else {
      *out_times = new float[beats.size()];
      std::memcpy(*out_times, beats.data(), beats.size() * sizeof(float));
    }
    return SONARE_OK;
  } catch (const std::bad_alloc&) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    return SONARE_ERROR_UNKNOWN;
  }
}

SonareError sonare_detect_onsets(const float* samples, size_t length, int sample_rate,
                                 float** out_times, size_t* out_count) {
  if (samples == nullptr || out_times == nullptr || out_count == nullptr || length == 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (sample_rate < kMinSampleRate || sample_rate > kMaxSampleRate) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (length > kMaxBufferSize) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  try {
    std::vector<float> onsets = quick::detect_onsets(samples, length, sample_rate);
    *out_count = onsets.size();
    if (onsets.empty()) {
      *out_times = nullptr;
    } else {
      *out_times = new float[onsets.size()];
      std::memcpy(*out_times, onsets.data(), onsets.size() * sizeof(float));
    }
    return SONARE_OK;
  } catch (const std::bad_alloc&) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    return SONARE_ERROR_UNKNOWN;
  }
}

// Full analysis

SonareError sonare_analyze(const float* samples, size_t length, int sample_rate,
                           SonareAnalysisResult* out) {
  if (samples == nullptr || out == nullptr || length == 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (sample_rate < kMinSampleRate || sample_rate > kMaxSampleRate) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (length > kMaxBufferSize) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  try {
    AnalysisResult result = quick::analyze(samples, length, sample_rate);

    out->bpm = result.bpm;
    out->bpm_confidence = result.bpm_confidence;
    out->key.root = static_cast<SonarePitchClass>(result.key.root);
    out->key.mode = static_cast<SonareMode>(result.key.mode);
    out->key.confidence = result.key.confidence;
    out->time_signature.numerator = result.time_signature.numerator;
    out->time_signature.denominator = result.time_signature.denominator;
    out->time_signature.confidence = result.time_signature.confidence;

    // Copy beat times
    out->beat_count = result.beats.size();
    if (result.beats.empty()) {
      out->beat_times = nullptr;
    } else {
      out->beat_times = new float[result.beats.size()];
      for (size_t i = 0; i < result.beats.size(); ++i) {
        out->beat_times[i] = result.beats[i].time;
      }
    }

    return SONARE_OK;
  } catch (const std::bad_alloc&) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    return SONARE_ERROR_UNKNOWN;
  }
}

// Memory management

void sonare_free_floats(float* ptr) { delete[] ptr; }

void sonare_free_result(SonareAnalysisResult* result) {
  if (result != nullptr) {
    delete[] result->beat_times;
    result->beat_times = nullptr;
    result->beat_count = 0;
  }
}

// Error handling

const char* sonare_error_message(SonareError error) {
  switch (error) {
    case SONARE_OK:
      return "OK";
    case SONARE_ERROR_FILE_NOT_FOUND:
      return "File not found";
    case SONARE_ERROR_INVALID_FORMAT:
      return "Invalid format";
    case SONARE_ERROR_DECODE_FAILED:
      return "Decode failed";
    case SONARE_ERROR_INVALID_PARAMETER:
      return "Invalid parameter";
    case SONARE_ERROR_OUT_OF_MEMORY:
      return "Out of memory";
    default:
      return "Unknown error";
  }
}

// Version

const char* sonare_version(void) { return "1.0.0"; }
