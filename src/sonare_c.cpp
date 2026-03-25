/// @file sonare_c.cpp
/// @brief Implementation of C API.

#include "sonare_c.h"

#include <cstring>
#include <new>
#include <string>

#include "analysis/beat_analyzer.h"
#include "analysis/bpm_analyzer.h"
#include "analysis/key_analyzer.h"
#include "analysis/music_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "core/audio.h"
#include "core/convert.h"
#include "core/resample.h"
#include "core/spectrum.h"
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/pitch_shift.h"
#include "effects/time_stretch.h"
#include "feature/chroma.h"
#include "feature/mel_spectrogram.h"
#include "feature/pitch.h"
#include "feature/spectral.h"
#include "quick.h"
#include "sonare.h"
#include "util/exception.h"

using namespace sonare;

// Standardized try/catch macros for C API functions.
// Every C API function that can throw should use these to ensure SonareException
// is properly mapped to the correct C error code instead of falling through to
// the catch(...) handler as SONARE_ERROR_UNKNOWN.
#define SONARE_C_TRY try {
#define SONARE_C_CATCH                 \
  }                                    \
  catch (const SonareException& e) {   \
    return map_sonare_exception(e);    \
  }                                    \
  catch (const std::bad_alloc&) {      \
    return SONARE_ERROR_OUT_OF_MEMORY; \
  }                                    \
  catch (...) {                        \
    return SONARE_ERROR_UNKNOWN;       \
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

// Version

const char* sonare_version(void) { return SONARE_VERSION_STRING; }

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
  out->harmonic = new float[out->length];
  out->percussive = new float[out->length];
  std::memcpy(out->harmonic, result.harmonic.data(), out->length * sizeof(float));
  std::memcpy(out->percussive, result.percussive.data(), out->length * sizeof(float));
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

  out->magnitude = new float[total];
  out->power = new float[total];
  std::memcpy(out->magnitude, mag.data(), total * sizeof(float));
  std::memcpy(out->power, pow.data(), total * sizeof(float));
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
  out->power = new float[total];
  std::memcpy(out->power, mel.power_data(), total * sizeof(float));

  std::vector<float> db = mel.to_db();
  out->db = new float[total];
  std::memcpy(out->db, db.data(), total * sizeof(float));
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

  try {
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
  } catch (const SonareException& e) {
    return map_sonare_exception(e);
  } catch (const std::bad_alloc&) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    return SONARE_ERROR_UNKNOWN;
  }
}

SonareError sonare_spectral_bandwidth(const float* samples, size_t length, int sample_rate,
                                      int n_fft, int hop_length, float** out, size_t* out_count) {
  if (!out || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  try {
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
  } catch (const SonareException& e) {
    return map_sonare_exception(e);
  } catch (const std::bad_alloc&) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    return SONARE_ERROR_UNKNOWN;
  }
}

SonareError sonare_spectral_rolloff(const float* samples, size_t length, int sample_rate, int n_fft,
                                    int hop_length, float roll_percent, float** out,
                                    size_t* out_count) {
  if (!out || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  try {
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
  } catch (const SonareException& e) {
    return map_sonare_exception(e);
  } catch (const std::bad_alloc&) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    return SONARE_ERROR_UNKNOWN;
  }
}

SonareError sonare_spectral_flatness(const float* samples, size_t length, int sample_rate,
                                     int n_fft, int hop_length, float** out, size_t* out_count) {
  if (!out || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  try {
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
  } catch (const SonareException& e) {
    return map_sonare_exception(e);
  } catch (const std::bad_alloc&) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    return SONARE_ERROR_UNKNOWN;
  }
}

SonareError sonare_zero_crossing_rate(const float* samples, size_t length, int sample_rate,
                                      int frame_length, int hop_length, float** out,
                                      size_t* out_count) {
  if (!out || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  try {
    Audio audio = Audio::from_buffer(samples, length, sample_rate);
    std::vector<float> result = zero_crossing_rate(audio, frame_length, hop_length);
    *out_count = result.size();
    *out = new float[result.size()];
    std::memcpy(*out, result.data(), result.size() * sizeof(float));
    return SONARE_OK;
  } catch (const SonareException& e) {
    return map_sonare_exception(e);
  } catch (const std::bad_alloc&) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    return SONARE_ERROR_UNKNOWN;
  }
}

SonareError sonare_rms_energy(const float* samples, size_t length, int sample_rate,
                              int frame_length, int hop_length, float** out, size_t* out_count) {
  if (!out || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  try {
    Audio audio = Audio::from_buffer(samples, length, sample_rate);
    std::vector<float> result = rms_energy(audio, frame_length, hop_length);
    *out_count = result.size();
    *out = new float[result.size()];
    std::memcpy(*out, result.data(), result.size() * sizeof(float));
    return SONARE_OK;
  } catch (const SonareException& e) {
    return map_sonare_exception(e);
  } catch (const std::bad_alloc&) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    return SONARE_ERROR_UNKNOWN;
  }
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

  try {
    Audio audio = Audio::from_buffer(samples, length, sample_rate);
    PitchConfig config;
    config.frame_length = frame_length;
    config.hop_length = hop_length;
    config.fmin = fmin;
    config.fmax = fmax;
    config.threshold = threshold;
    PitchResult result = yin_track(audio, config);
    return fill_pitch_result(result, out);
  } catch (const SonareException& e) {
    return map_sonare_exception(e);
  } catch (const std::bad_alloc&) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    return SONARE_ERROR_UNKNOWN;
  }
}

SonareError sonare_pitch_pyin(const float* samples, size_t length, int sample_rate,
                              int frame_length, int hop_length, float fmin, float fmax,
                              float threshold, SonarePitchResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  try {
    Audio audio = Audio::from_buffer(samples, length, sample_rate);
    PitchConfig config;
    config.frame_length = frame_length;
    config.hop_length = hop_length;
    config.fmin = fmin;
    config.fmax = fmax;
    config.threshold = threshold;
    PitchResult result = pyin(audio, config);
    return fill_pitch_result(result, out);
  } catch (const SonareException& e) {
    return map_sonare_exception(e);
  } catch (const std::bad_alloc&) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    return SONARE_ERROR_UNKNOWN;
  }
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

  try {
    std::vector<float> result = resample(samples, length, src_sr, target_sr);
    *out_length = result.size();
    *out = new float[result.size()];
    std::memcpy(*out, result.data(), result.size() * sizeof(float));
    return SONARE_OK;
  } catch (const SonareException& e) {
    return map_sonare_exception(e);
  } catch (const std::bad_alloc&) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    return SONARE_ERROR_UNKNOWN;
  }
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
