#include <cstring>
#include <memory>

#include "core/audio.h"
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/pitch_shift.h"
#include "effects/time_stretch.h"
#include "sonare_c.h"
#include "sonare_c_internal.h"

using namespace sonare;
using namespace sonare_c_detail;

SonareError sonare_hpss(const float* samples, size_t length, int sample_rate, int kernel_harmonic,
                        int kernel_percussive, SonareHpssResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;

  out->harmonic = nullptr;
  out->percussive = nullptr;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
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
  });
}

SonareError sonare_harmonic(const float* samples, size_t length, int sample_rate, float** out,
                            size_t* out_length) {
  return run_mono_offline(samples, length, sample_rate, out, out_length,
                          [](const Audio& a) { return harmonic(a); });
}

SonareError sonare_percussive(const float* samples, size_t length, int sample_rate, float** out,
                              size_t* out_length) {
  return run_mono_offline(samples, length, sample_rate, out, out_length,
                          [](const Audio& a) { return percussive(a); });
}

SonareError sonare_time_stretch(const float* samples, size_t length, int sample_rate, float rate,
                                float** out, size_t* out_length) {
  return run_mono_offline(samples, length, sample_rate, out, out_length,
                          [rate](const Audio& a) { return time_stretch(a, rate); });
}

SonareError sonare_pitch_shift(const float* samples, size_t length, int sample_rate,
                               float semitones, float** out, size_t* out_length) {
  return run_mono_offline(samples, length, sample_rate, out, out_length,
                          [semitones](const Audio& a) { return pitch_shift(a, semitones); });
}

SonareError sonare_normalize(const float* samples, size_t length, int sample_rate, float target_db,
                             float** out, size_t* out_length) {
  return run_mono_offline(samples, length, sample_rate, out, out_length,
                          [target_db](const Audio& a) { return normalize(a, target_db); });
}

SonareError sonare_trim(const float* samples, size_t length, int sample_rate, float threshold_db,
                        float** out, size_t* out_length) {
  return run_mono_offline(samples, length, sample_rate, out, out_length,
                          [threshold_db](const Audio& a) { return trim(a, threshold_db); });
}
