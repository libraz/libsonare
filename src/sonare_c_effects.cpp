#include "sonare_c.h"

#include <cstring>
#include <memory>

#include "core/audio.h"
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/pitch_shift.h"
#include "effects/time_stretch.h"
#include "effects/tts.h"
#include "sonare_c_internal.h"

using namespace sonare;
using namespace sonare_c_detail;

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
