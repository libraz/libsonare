/// @file sonare_c.cpp
/// @brief Implementation of C API.

#include "sonare_c.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <string>

#include "analysis/beat_analyzer.h"
#include "analysis/bpm_analyzer.h"
#include "analysis/chord_analyzer.h"
#include "analysis/dynamics_analyzer.h"
#include "analysis/key_analyzer.h"
#include "analysis/music_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "analysis/rhythm_analyzer.h"
#include "analysis/timbre_analyzer.h"
#include "core/audio.h"
#include "core/convert.h"
#include "core/resample.h"
#include "core/spectrum.h"
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/pitch_shift.h"
#include "effects/time_stretch.h"
#include "effects/tts.h"
#include "feature/chroma.h"
#include "feature/mel_spectrogram.h"
#include "feature/pitch.h"
#include "feature/spectral.h"
#include "quick.h"
#include "sonare.h"
#include "util/exception.h"

using namespace sonare;

namespace {

/// @brief Thread-local storage for the most recent detailed error message.
/// @details Populated by SONARE_C_CATCH whenever an exception is caught. Consumers query
///   it via sonare_last_error_message() when a C API call returns a non-OK code.
std::string& last_error_storage() {
  static thread_local std::string storage;
  return storage;
}

/// @brief Stores a detailed error message for retrieval by sonare_last_error_message.
void set_last_error(const char* msg) {
  // Defensive: treat NULL as empty so consumers never see a dangling pointer.
  last_error_storage().assign(msg != nullptr ? msg : "");
}

}  // namespace

// Standardized try/catch macros for C API functions.
// Every C API function that can throw should use these to ensure SonareException
// is properly mapped to the correct C error code instead of falling through to
// the catch(...) handler as SONARE_ERROR_UNKNOWN. The catch handlers also record
// the detailed message into thread-local storage so callers can retrieve it via
// sonare_last_error_message().
#define SONARE_C_TRY try {
#define SONARE_C_CATCH                                                 \
  }                                                                    \
  catch (const SonareException& e) {                                   \
    set_last_error(e.what());                                          \
    return map_sonare_exception(e);                                    \
  }                                                                    \
  catch (const std::bad_alloc& e) {                                    \
    set_last_error(e.what());                                          \
    return SONARE_ERROR_OUT_OF_MEMORY;                                 \
  }                                                                    \
  catch (const std::exception& e) {                                    \
    set_last_error(e.what());                                          \
    return SONARE_ERROR_UNKNOWN;                                       \
  }                                                                    \
  catch (...) {                                                        \
    set_last_error("Unknown C++ exception (non-std::exception type)"); \
    return SONARE_ERROR_UNKNOWN;                                       \
  }

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

namespace {

/// @brief Maps SonareException error code to C API error code.
SonareError map_sonare_exception(const SonareException& e) {
  switch (e.code()) {
    case ErrorCode::FileNotFound:
      return SONARE_ERROR_FILE_NOT_FOUND;
    case ErrorCode::InvalidFormat:
      return SONARE_ERROR_INVALID_FORMAT;
    case ErrorCode::DecodeFailed:
      return SONARE_ERROR_DECODE_FAILED;
    case ErrorCode::InvalidParameter:
      return SONARE_ERROR_INVALID_PARAMETER;
    case ErrorCode::OutOfMemory:
      return SONARE_ERROR_OUT_OF_MEMORY;
    default:
      return SONARE_ERROR_UNKNOWN;
  }
}

/// @brief Validates common audio buffer parameters for C API functions.
SonareError validate_audio_params(const float* samples, size_t length, int sample_rate) {
  if (samples == nullptr || length == 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (sample_rate < kMinSampleRate || sample_rate > kMaxSampleRate)
    return SONARE_ERROR_INVALID_PARAMETER;
  if (length > kMaxBufferSize) return SONARE_ERROR_INVALID_PARAMETER;
  return SONARE_OK;
}

template <typename T>
T* release_array(std::unique_ptr<T[]>& ptr) {
  return ptr.release();
}

SonareGrooveType to_c_groove_type(const std::string& groove) {
  if (groove == "swing") return SONARE_GROOVE_SWING;
  if (groove == "shuffle") return SONARE_GROOVE_SHUFFLE;
  return SONARE_GROOVE_STRAIGHT;
}

SonareChordQuality to_c_chord_quality(ChordQuality quality) {
  switch (quality) {
    case ChordQuality::Major:
      return SONARE_CHORD_MAJOR;
    case ChordQuality::Minor:
      return SONARE_CHORD_MINOR;
    case ChordQuality::Diminished:
      return SONARE_CHORD_DIMINISHED;
    case ChordQuality::Augmented:
      return SONARE_CHORD_AUGMENTED;
    case ChordQuality::Dominant7:
      return SONARE_CHORD_DOMINANT7;
    case ChordQuality::Major7:
      return SONARE_CHORD_MAJOR7;
    case ChordQuality::Minor7:
      return SONARE_CHORD_MINOR7;
    case ChordQuality::Sus2:
      return SONARE_CHORD_SUS2;
    case ChordQuality::Sus4:
      return SONARE_CHORD_SUS4;
    case ChordQuality::Unknown:
      return SONARE_CHORD_UNKNOWN;
  }
  return SONARE_CHORD_UNKNOWN;
}

}  // namespace

SonareError sonare_audio_from_buffer(const float* data, size_t length, int sample_rate,
                                     SonareAudio** out) {
  if (out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(data, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  *out = new SonareAudio{Audio::from_buffer(data, length, sample_rate)};
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_from_memory(const uint8_t* data, size_t length, SonareAudio** out) {
  if (data == nullptr || out == nullptr || length == 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  SONARE_C_TRY
  *out = new SonareAudio{Audio::from_memory(data, length)};
  return SONARE_OK;
  SONARE_C_CATCH
}

#ifndef __EMSCRIPTEN__
SonareError sonare_audio_from_file(const char* path, SonareAudio** out) {
  if (path == nullptr || out == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  SONARE_C_TRY
  *out = new SonareAudio{Audio::from_file(path)};
  return SONARE_OK;
  SONARE_C_CATCH
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

SonareError sonare_audio_detect_bpm(const SonareAudio* audio, float* out_bpm) {
  if (audio == nullptr || out_bpm == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  *out_bpm =
      quick::detect_bpm(audio->audio.data(), audio->audio.size(), audio->audio.sample_rate());
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_detect_key(const SonareAudio* audio, SonareKey* out_key) {
  if (audio == nullptr || out_key == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  Key key = quick::detect_key(audio->audio.data(), audio->audio.size(), audio->audio.sample_rate());
  out_key->root = static_cast<SonarePitchClass>(key.root);
  out_key->mode = static_cast<SonareMode>(key.mode);
  out_key->confidence = key.confidence;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_detect_beats(const SonareAudio* audio, float** out_times,
                                      size_t* out_count) {
  if (audio == nullptr || out_times == nullptr || out_count == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  SONARE_C_TRY
  std::vector<float> beats =
      quick::detect_beats(audio->audio.data(), audio->audio.size(), audio->audio.sample_rate());
  *out_count = beats.size();
  if (beats.empty()) {
    *out_times = nullptr;
  } else {
    *out_times = new float[beats.size()];
    std::memcpy(*out_times, beats.data(), beats.size() * sizeof(float));
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_detect_onsets(const SonareAudio* audio, float** out_times,
                                       size_t* out_count) {
  if (audio == nullptr || out_times == nullptr || out_count == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  SONARE_C_TRY
  std::vector<float> onsets =
      quick::detect_onsets(audio->audio.data(), audio->audio.size(), audio->audio.sample_rate());
  *out_count = onsets.size();
  if (onsets.empty()) {
    *out_times = nullptr;
  } else {
    *out_times = new float[onsets.size()];
    std::memcpy(*out_times, onsets.data(), onsets.size() * sizeof(float));
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_analyze(const SonareAudio* audio, SonareAnalysisResult* out) {
  if (audio == nullptr || out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  out->beat_times = nullptr;

  SONARE_C_TRY
  AnalysisResult result =
      quick::analyze(audio->audio.data(), audio->audio.size(), audio->audio.sample_rate());

  out->bpm = result.bpm;
  out->bpm_confidence = result.bpm_confidence;
  out->key.root = static_cast<SonarePitchClass>(result.key.root);
  out->key.mode = static_cast<SonareMode>(result.key.mode);
  out->key.confidence = result.key.confidence;
  out->time_signature.numerator = result.time_signature.numerator;
  out->time_signature.denominator = result.time_signature.denominator;
  out->time_signature.confidence = result.time_signature.confidence;
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
  SONARE_C_CATCH
}

// Quick detection functions

SonareError sonare_detect_bpm(const float* samples, size_t length, int sample_rate,
                              float* out_bpm) {
  if (out_bpm == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  *out_bpm = quick::detect_bpm(samples, length, sample_rate);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_detect_key(const float* samples, size_t length, int sample_rate,
                              SonareKey* out_key) {
  if (out_key == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Key key = quick::detect_key(samples, length, sample_rate);
  out_key->root = static_cast<SonarePitchClass>(key.root);
  out_key->mode = static_cast<SonareMode>(key.mode);
  out_key->confidence = key.confidence;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_detect_beats(const float* samples, size_t length, int sample_rate,
                                float** out_times, size_t* out_count) {
  if (out_times == nullptr || out_count == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  std::vector<float> beats = quick::detect_beats(samples, length, sample_rate);
  *out_count = beats.size();
  if (beats.empty()) {
    *out_times = nullptr;
  } else {
    *out_times = new float[beats.size()];
    std::memcpy(*out_times, beats.data(), beats.size() * sizeof(float));
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_detect_onsets(const float* samples, size_t length, int sample_rate,
                                 float** out_times, size_t* out_count) {
  if (out_times == nullptr || out_count == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  std::vector<float> onsets = quick::detect_onsets(samples, length, sample_rate);
  *out_count = onsets.size();
  if (onsets.empty()) {
    *out_times = nullptr;
  } else {
    *out_times = new float[onsets.size()];
    std::memcpy(*out_times, onsets.data(), onsets.size() * sizeof(float));
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

// Full analysis

SonareError sonare_analyze(const float* samples, size_t length, int sample_rate,
                           SonareAnalysisResult* out) {
  if (out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  out->beat_times = nullptr;

  SONARE_C_TRY
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
  SONARE_C_CATCH
}

// Memory management

void sonare_free_floats(float* ptr) { delete[] ptr; }

void sonare_free_ints(int* ptr) { delete[] ptr; }

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

const char* sonare_last_error_message(void) {
  // c_str() guarantees a valid NUL-terminated pointer even when the string is empty.
  return last_error_storage().c_str();
}

// Version

const char* sonare_version(void) { return SONARE_VERSION_STRING; }

int sonare_has_ffmpeg_support(void) {
#ifdef SONARE_WITH_FFMPEG
  return 1;
#else
  return 0;
#endif
}

// ============================================================================
// Effects
// ============================================================================

SonareError sonare_hpss(const float* samples, size_t length, int sample_rate, int kernel_harmonic,
                        int kernel_percussive, SonareHpssResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  out->harmonic = nullptr;
  out->percussive = nullptr;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  HpssConfig config;
  config.kernel_size_harmonic = kernel_harmonic;
  config.kernel_size_percussive = kernel_percussive;
  HpssAudioResult result = hpss(audio, config);

  out->length = result.harmonic.size();
  out->sample_rate = result.harmonic.sample_rate();
  std::unique_ptr<float[]> harmonic(new float[out->length]);
  std::unique_ptr<float[]> percussive(new float[out->length]);
  std::memcpy(harmonic.get(), result.harmonic.data(), out->length * sizeof(float));
  std::memcpy(percussive.get(), result.percussive.data(), out->length * sizeof(float));
  out->harmonic = release_array(harmonic);
  out->percussive = release_array(percussive);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_harmonic(const float* samples, size_t length, int sample_rate, float** out,
                            size_t* out_length) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  Audio result = harmonic(audio);
  *out_length = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_percussive(const float* samples, size_t length, int sample_rate, float** out,
                              size_t* out_length) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  Audio result = percussive(audio);
  *out_length = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_time_stretch(const float* samples, size_t length, int sample_rate, float rate,
                                float** out, size_t* out_length) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  Audio result = time_stretch(audio, rate);
  *out_length = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_pitch_shift(const float* samples, size_t length, int sample_rate,
                               float semitones, float** out, size_t* out_length) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  Audio result = pitch_shift(audio, semitones);
  *out_length = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_normalize(const float* samples, size_t length, int sample_rate, float target_db,
                             float** out, size_t* out_length) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  Audio result = normalize(audio, target_db);
  *out_length = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_trim(const float* samples, size_t length, int sample_rate, float threshold_db,
                        float** out, size_t* out_length) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  Audio result = trim(audio, threshold_db);
  *out_length = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_analyze_tts_quality(const float* samples, size_t length, int sample_rate,
                                       float silence_threshold_db, SonareTtsQualityResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  TtsQualityResult result = analyze_tts_quality(audio, silence_threshold_db);
  out->duration_sec = result.duration_sec;
  out->peak_db = result.peak_db;
  out->rms_db = result.rms_db;
  out->silence_ratio = result.silence_ratio;
  out->clipping_ratio = result.clipping_ratio;
  out->leading_silence_sec = result.leading_silence_sec;
  out->trailing_silence_sec = result.trailing_silence_sec;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_prepare_tts(const float* samples, size_t length, int sample_rate,
                               float target_rms_db, float silence_threshold_db, float peak_limit_db,
                               float fade_sec, float** out, size_t* out_length) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  Audio result = prepare_tts(audio, target_rms_db, silence_threshold_db, peak_limit_db, fade_sec);
  *out_length = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_compress_pauses(const float* samples, size_t length, int sample_rate,
                                   float max_pause_sec, float silence_threshold_db, float** out,
                                   size_t* out_length) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  Audio result = compress_pauses(audio, max_pause_sec, silence_threshold_db);
  *out_length = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_analyze_bpm(const float* samples, size_t length, int sample_rate, float bpm_min,
                               float bpm_max, float start_bpm, int n_fft, int hop_length,
                               int max_candidates, SonareBpmAnalysisResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (bpm_min <= 0.0f || bpm_max <= bpm_min || n_fft <= 0 || hop_length <= 0 ||
      max_candidates < 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  out->candidates = nullptr;
  out->candidate_count = 0;
  out->autocorrelation = nullptr;
  out->autocorrelation_count = 0;
  out->tempogram = nullptr;
  out->tempogram_count = 0;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  BpmConfig config;
  config.bpm_min = bpm_min;
  config.bpm_max = bpm_max;
  config.start_bpm = start_bpm;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  BpmAnalyzer analyzer(audio, config);
  out->bpm = analyzer.bpm();
  out->confidence = analyzer.confidence();

  std::vector<BpmCandidate> candidates = analyzer.candidates(max_candidates);
  out->candidate_count = candidates.size();
  if (!candidates.empty()) {
    std::unique_ptr<SonareBpmCandidate[]> cands(new SonareBpmCandidate[candidates.size()]);
    for (size_t i = 0; i < candidates.size(); ++i) {
      cands[i].bpm = candidates[i].bpm;
      cands[i].confidence = candidates[i].confidence;
    }
    out->candidates = release_array(cands);
  }

  const std::vector<float>& autocorr = analyzer.autocorrelation();
  out->autocorrelation_count = autocorr.size();
  if (!autocorr.empty()) {
    std::unique_ptr<float[]> data(new float[autocorr.size()]);
    std::memcpy(data.get(), autocorr.data(), autocorr.size() * sizeof(float));
    out->autocorrelation = release_array(data);
  }

  const std::vector<float>& tempogram = analyzer.tempogram();
  out->tempogram_count = tempogram.size();
  if (!tempogram.empty()) {
    std::unique_ptr<float[]> data(new float[tempogram.size()]);
    std::memcpy(data.get(), tempogram.data(), tempogram.size() * sizeof(float));
    out->tempogram = release_array(data);
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_analyze_rhythm(const float* samples, size_t length, int sample_rate,
                                  float bpm_min, float bpm_max, float start_bpm, int n_fft,
                                  int hop_length, SonareRhythmResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (bpm_min <= 0.0f || bpm_max <= bpm_min || n_fft <= 0 || hop_length <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  out->beat_intervals = nullptr;
  out->beat_interval_count = 0;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  RhythmConfig config;
  config.bpm_min = bpm_min;
  config.bpm_max = bpm_max;
  config.start_bpm = start_bpm;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  RhythmAnalyzer analyzer(audio, config);
  RhythmFeatures features = analyzer.features();
  out->bpm = analyzer.bpm();
  out->time_signature.numerator = features.time_signature.numerator;
  out->time_signature.denominator = features.time_signature.denominator;
  out->time_signature.confidence = features.time_signature.confidence;
  out->groove_type = to_c_groove_type(features.groove_type);
  out->syncopation = features.syncopation;
  out->pattern_regularity = features.pattern_regularity;
  out->tempo_stability = features.tempo_stability;

  const std::vector<float>& intervals = analyzer.beat_intervals();
  out->beat_interval_count = intervals.size();
  if (!intervals.empty()) {
    std::unique_ptr<float[]> data(new float[intervals.size()]);
    std::memcpy(data.get(), intervals.data(), intervals.size() * sizeof(float));
    out->beat_intervals = release_array(data);
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_analyze_dynamics(const float* samples, size_t length, int sample_rate,
                                    float window_sec, int hop_length, float compression_threshold,
                                    SonareDynamicsResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (window_sec <= 0.0f || hop_length <= 0 || compression_threshold < 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  out->loudness_times = nullptr;
  out->loudness_rms_db = nullptr;
  out->loudness_count = 0;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  DynamicsConfig config;
  config.window_sec = window_sec;
  config.hop_length = hop_length;
  config.compression_threshold = compression_threshold;

  DynamicsAnalyzer analyzer(audio, config);
  const Dynamics& dynamics = analyzer.dynamics();
  out->dynamic_range_db = dynamics.dynamic_range_db;
  out->peak_db = dynamics.peak_db;
  out->rms_db = dynamics.rms_db;
  out->crest_factor = dynamics.crest_factor;
  out->loudness_range_db = dynamics.loudness_range_db;
  out->is_compressed = dynamics.is_compressed ? 1 : 0;

  const LoudnessCurve& curve = analyzer.loudness_curve();
  size_t count = std::min(curve.times.size(), curve.rms_db.size());
  out->loudness_count = count;
  if (count > 0) {
    std::unique_ptr<float[]> times(new float[count]);
    std::unique_ptr<float[]> rms_db(new float[count]);
    std::memcpy(times.get(), curve.times.data(), count * sizeof(float));
    std::memcpy(rms_db.get(), curve.rms_db.data(), count * sizeof(float));
    out->loudness_times = release_array(times);
    out->loudness_rms_db = release_array(rms_db);
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_analyze_timbre(const float* samples, size_t length, int sample_rate, int n_fft,
                                  int hop_length, int n_mels, int n_mfcc, float window_sec,
                                  SonareTimbreResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (n_fft <= 0 || hop_length <= 0 || n_mels <= 0 || n_mfcc <= 0 || window_sec <= 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  out->spectral_centroid = nullptr;
  out->spectral_centroid_count = 0;
  out->spectral_flatness = nullptr;
  out->spectral_flatness_count = 0;
  out->spectral_rolloff = nullptr;
  out->spectral_rolloff_count = 0;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  TimbreConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;
  config.n_mfcc = n_mfcc;
  config.window_sec = window_sec;

  TimbreAnalyzer analyzer(audio, config);
  const Timbre& timbre = analyzer.timbre();
  out->brightness = timbre.brightness;
  out->warmth = timbre.warmth;
  out->density = timbre.density;
  out->roughness = timbre.roughness;
  out->complexity = timbre.complexity;

  const std::vector<float>& centroid = analyzer.spectral_centroid();
  out->spectral_centroid_count = centroid.size();
  if (!centroid.empty()) {
    std::unique_ptr<float[]> data(new float[centroid.size()]);
    std::memcpy(data.get(), centroid.data(), centroid.size() * sizeof(float));
    out->spectral_centroid = release_array(data);
  }

  const std::vector<float>& flatness = analyzer.spectral_flatness();
  out->spectral_flatness_count = flatness.size();
  if (!flatness.empty()) {
    std::unique_ptr<float[]> data(new float[flatness.size()]);
    std::memcpy(data.get(), flatness.data(), flatness.size() * sizeof(float));
    out->spectral_flatness = release_array(data);
  }

  const std::vector<float>& rolloff = analyzer.spectral_rolloff();
  out->spectral_rolloff_count = rolloff.size();
  if (!rolloff.empty()) {
    std::unique_ptr<float[]> data(new float[rolloff.size()]);
    std::memcpy(data.get(), rolloff.data(), rolloff.size() * sizeof(float));
    out->spectral_rolloff = release_array(data);
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_detect_chords(const float* samples, size_t length, int sample_rate,
                                 float min_duration, float smoothing_window, float threshold,
                                 int use_triads_only, int n_fft, int hop_length, int use_beat_sync,
                                 SonareChordAnalysisResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (min_duration < 0.0f || smoothing_window <= 0.0f || threshold < 0.0f || n_fft <= 0 ||
      hop_length <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  out->chords = nullptr;
  out->chord_count = 0;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  ChordConfig config;
  config.min_duration = min_duration;
  config.smoothing_window = smoothing_window;
  config.threshold = threshold;
  config.use_triads_only = use_triads_only != 0;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.use_beat_sync = use_beat_sync != 0;

  std::vector<Chord> chords = detect_chords(audio, config);
  out->chord_count = chords.size();
  if (!chords.empty()) {
    std::unique_ptr<SonareChord[]> data(new SonareChord[chords.size()]);
    for (size_t i = 0; i < chords.size(); ++i) {
      data[i].root = static_cast<SonarePitchClass>(chords[i].root);
      data[i].quality = to_c_chord_quality(chords[i].quality);
      data[i].start = chords[i].start;
      data[i].end = chords[i].end;
      data[i].confidence = chords[i].confidence;
    }
    out->chords = release_array(data);
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

// ============================================================================
// Features - Spectrogram
// ============================================================================

SonareError sonare_stft(const float* samples, size_t length, int sample_rate, int n_fft,
                        int hop_length, SonareStftResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  out->magnitude = nullptr;
  out->power = nullptr;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  Spectrogram spec = Spectrogram::compute(audio, config);

  out->n_bins = spec.n_bins();
  out->n_frames = spec.n_frames();
  out->n_fft = spec.n_fft();
  out->hop_length = spec.hop_length();
  out->sample_rate = spec.sample_rate();

  size_t total = static_cast<size_t>(spec.n_bins()) * spec.n_frames();
  const std::vector<float>& mag = spec.magnitude();
  const std::vector<float>& pow = spec.power();

  std::unique_ptr<float[]> magnitude(new float[total]);
  std::unique_ptr<float[]> power(new float[total]);
  std::memcpy(magnitude.get(), mag.data(), total * sizeof(float));
  std::memcpy(power.get(), pow.data(), total * sizeof(float));
  out->magnitude = release_array(magnitude);
  out->power = release_array(power);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stft_db(const float* samples, size_t length, int sample_rate, int n_fft,
                           int hop_length, int* out_n_bins, int* out_n_frames, float** out_db) {
  if (!out_n_bins || !out_n_frames || !out_db) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  Spectrogram spec = Spectrogram::compute(audio, config);

  *out_n_bins = spec.n_bins();
  *out_n_frames = spec.n_frames();
  std::vector<float> db = spec.to_db();
  *out_db = new float[db.size()];
  std::memcpy(*out_db, db.data(), db.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

// ============================================================================
// Features - Mel
// ============================================================================

SonareError sonare_mel_spectrogram(const float* samples, size_t length, int sample_rate, int n_fft,
                                   int hop_length, int n_mels, SonareMelResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  out->power = nullptr;
  out->db = nullptr;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;
  MelSpectrogram mel = MelSpectrogram::compute(audio, config);

  out->n_mels = mel.n_mels();
  out->n_frames = mel.n_frames();
  out->sample_rate = mel.sample_rate();
  out->hop_length = mel.hop_length();

  size_t total = static_cast<size_t>(mel.n_mels()) * mel.n_frames();
  std::unique_ptr<float[]> power(new float[total]);
  std::memcpy(power.get(), mel.power_data(), total * sizeof(float));

  std::vector<float> db = mel.to_db();
  std::unique_ptr<float[]> db_out(new float[total]);
  std::memcpy(db_out.get(), db.data(), total * sizeof(float));
  out->power = release_array(power);
  out->db = release_array(db_out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mfcc(const float* samples, size_t length, int sample_rate, int n_fft,
                        int hop_length, int n_mels, int n_mfcc, SonareMfccResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  out->coefficients = nullptr;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;
  MelSpectrogram mel = MelSpectrogram::compute(audio, config);
  std::vector<float> mfcc_data = mel.mfcc(n_mfcc);

  out->n_mfcc = n_mfcc;
  out->n_frames = mel.n_frames();
  out->coefficients = new float[mfcc_data.size()];
  std::memcpy(out->coefficients, mfcc_data.data(), mfcc_data.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

// ============================================================================
// Features - Chroma
// ============================================================================

SonareError sonare_chroma(const float* samples, size_t length, int sample_rate, int n_fft,
                          int hop_length, SonareChromaResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  out->features = nullptr;
  out->mean_energy = nullptr;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  ChromaConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  Chroma chroma_result = Chroma::compute(audio, config);

  out->n_chroma = chroma_result.n_chroma();
  out->n_frames = chroma_result.n_frames();
  out->sample_rate = chroma_result.sample_rate();
  out->hop_length = chroma_result.hop_length();

  size_t total = static_cast<size_t>(chroma_result.n_chroma()) * chroma_result.n_frames();
  out->features = new float[total];
  std::memcpy(out->features, chroma_result.data(), total * sizeof(float));

  auto mean = chroma_result.mean_energy();
  out->mean_energy = new float[chroma_result.n_chroma()];
  for (int i = 0; i < chroma_result.n_chroma(); ++i) {
    out->mean_energy[i] = mean[static_cast<size_t>(i)];
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

// ============================================================================
// Features - Spectral
// ============================================================================

SonareError sonare_spectral_centroid(const float* samples, size_t length, int sample_rate,
                                     int n_fft, int hop_length, float** out, size_t* out_count) {
  if (!out || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  Spectrogram spec = Spectrogram::compute(audio, config);
  std::vector<float> result = spectral_centroid(spec, sample_rate);
  *out_count = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_spectral_bandwidth(const float* samples, size_t length, int sample_rate,
                                      int n_fft, int hop_length, float** out, size_t* out_count) {
  if (!out || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  Spectrogram spec = Spectrogram::compute(audio, config);
  std::vector<float> result = spectral_bandwidth(spec, sample_rate);
  *out_count = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_spectral_rolloff(const float* samples, size_t length, int sample_rate, int n_fft,
                                    int hop_length, float roll_percent, float** out,
                                    size_t* out_count) {
  if (!out || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  Spectrogram spec = Spectrogram::compute(audio, config);
  std::vector<float> result = spectral_rolloff(spec, sample_rate, roll_percent);
  *out_count = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_spectral_flatness(const float* samples, size_t length, int sample_rate,
                                     int n_fft, int hop_length, float** out, size_t* out_count) {
  if (!out || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  Spectrogram spec = Spectrogram::compute(audio, config);
  std::vector<float> result = spectral_flatness(spec);
  *out_count = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_zero_crossing_rate(const float* samples, size_t length, int sample_rate,
                                      int frame_length, int hop_length, float** out,
                                      size_t* out_count) {
  if (!out || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  std::vector<float> result = zero_crossing_rate(audio, frame_length, hop_length);
  *out_count = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_rms_energy(const float* samples, size_t length, int sample_rate,
                              int frame_length, int hop_length, float** out, size_t* out_count) {
  if (!out || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  std::vector<float> result = rms_energy(audio, frame_length, hop_length);
  *out_count = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

// ============================================================================
// Features - Pitch
// ============================================================================

namespace {

/// @brief Fills a SonarePitchResult from a PitchResult.
SonareError fill_pitch_result(const PitchResult& result, SonarePitchResult* out) {
  out->n_frames = result.n_frames();
  out->median_f0 = result.median_f0();
  out->mean_f0 = result.mean_f0();

  size_t n = static_cast<size_t>(result.n_frames());
  out->f0 = new float[n];
  out->voiced_prob = new float[n];
  out->voiced_flag = new int[n];

  std::memcpy(out->f0, result.f0.data(), n * sizeof(float));
  std::memcpy(out->voiced_prob, result.voiced_prob.data(), n * sizeof(float));
  for (size_t i = 0; i < n; ++i) {
    out->voiced_flag[i] = result.voiced_flag[i] ? 1 : 0;
  }
  return SONARE_OK;
}

}  // namespace

SonareError sonare_pitch_yin(const float* samples, size_t length, int sample_rate, int frame_length,
                             int hop_length, float fmin, float fmax, float threshold,
                             SonarePitchResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  PitchConfig config;
  config.frame_length = frame_length;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.fmax = fmax;
  config.threshold = threshold;
  PitchResult result = yin_track(audio, config);
  return fill_pitch_result(result, out);
  SONARE_C_CATCH
}

SonareError sonare_pitch_pyin(const float* samples, size_t length, int sample_rate,
                              int frame_length, int hop_length, float fmin, float fmax,
                              float threshold, SonarePitchResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  PitchConfig config;
  config.frame_length = frame_length;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.fmax = fmax;
  config.threshold = threshold;
  PitchResult result = pyin(audio, config);
  return fill_pitch_result(result, out);
  SONARE_C_CATCH
}

// ============================================================================
// Core - Conversion
// ============================================================================

float sonare_hz_to_mel(float hz) { return hz_to_mel(hz); }

float sonare_mel_to_hz(float mel) { return mel_to_hz(mel); }

float sonare_hz_to_midi(float hz) { return hz_to_midi(hz); }

float sonare_midi_to_hz(float midi) { return midi_to_hz(midi); }

const char* sonare_hz_to_note(float hz) {
  static thread_local char buf[16];
  std::string note = hz_to_note(hz);
  std::strncpy(buf, note.c_str(), sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  return buf;
}

float sonare_note_to_hz(const char* note) {
  if (!note) return 0.0f;
  return note_to_hz(std::string(note));
}

float sonare_frames_to_time(int frames, int sr, int hop_length) {
  return frames_to_time(frames, sr, hop_length);
}

int sonare_time_to_frames(float time, int sr, int hop_length) {
  return time_to_frames(time, sr, hop_length);
}

// ============================================================================
// Core - Resample
// ============================================================================

SonareError sonare_resample(const float* samples, size_t length, int src_sr, int target_sr,
                            float** out, size_t* out_length) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, src_sr);
  if (err != SONARE_OK) return err;
  if (target_sr < kMinSampleRate || target_sr > kMaxSampleRate) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  SONARE_C_TRY
  std::vector<float> result = resample(samples, length, src_sr, target_sr);
  *out_length = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

// ============================================================================
// Memory management for result types
// ============================================================================

void sonare_free_stft_result(SonareStftResult* r) {
  if (r) {
    delete[] r->magnitude;
    delete[] r->power;
    r->magnitude = nullptr;
    r->power = nullptr;
  }
}

void sonare_free_mel_result(SonareMelResult* r) {
  if (r) {
    delete[] r->power;
    delete[] r->db;
    r->power = nullptr;
    r->db = nullptr;
  }
}

void sonare_free_mfcc_result(SonareMfccResult* r) {
  if (r) {
    delete[] r->coefficients;
    r->coefficients = nullptr;
  }
}

void sonare_free_chroma_result(SonareChromaResult* r) {
  if (r) {
    delete[] r->features;
    delete[] r->mean_energy;
    r->features = nullptr;
    r->mean_energy = nullptr;
  }
}

void sonare_free_pitch_result(SonarePitchResult* r) {
  if (r) {
    delete[] r->f0;
    delete[] r->voiced_prob;
    delete[] r->voiced_flag;
    r->f0 = nullptr;
    r->voiced_prob = nullptr;
    r->voiced_flag = nullptr;
  }
}

void sonare_free_hpss_result(SonareHpssResult* r) {
  if (r) {
    delete[] r->harmonic;
    delete[] r->percussive;
    r->harmonic = nullptr;
    r->percussive = nullptr;
  }
}

void sonare_free_bpm_analysis_result(SonareBpmAnalysisResult* r) {
  if (r) {
    delete[] r->candidates;
    delete[] r->autocorrelation;
    delete[] r->tempogram;
    r->candidates = nullptr;
    r->candidate_count = 0;
    r->autocorrelation = nullptr;
    r->autocorrelation_count = 0;
    r->tempogram = nullptr;
    r->tempogram_count = 0;
  }
}

void sonare_free_rhythm_result(SonareRhythmResult* r) {
  if (r) {
    delete[] r->beat_intervals;
    r->beat_intervals = nullptr;
    r->beat_interval_count = 0;
  }
}

void sonare_free_dynamics_result(SonareDynamicsResult* r) {
  if (r) {
    delete[] r->loudness_times;
    delete[] r->loudness_rms_db;
    r->loudness_times = nullptr;
    r->loudness_rms_db = nullptr;
    r->loudness_count = 0;
  }
}

void sonare_free_timbre_result(SonareTimbreResult* r) {
  if (r) {
    delete[] r->spectral_centroid;
    delete[] r->spectral_flatness;
    delete[] r->spectral_rolloff;
    r->spectral_centroid = nullptr;
    r->spectral_centroid_count = 0;
    r->spectral_flatness = nullptr;
    r->spectral_flatness_count = 0;
    r->spectral_rolloff = nullptr;
    r->spectral_rolloff_count = 0;
  }
}

void sonare_free_chord_analysis_result(SonareChordAnalysisResult* r) {
  if (r) {
    delete[] r->chords;
    r->chords = nullptr;
    r->chord_count = 0;
  }
}
