#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "effects/decompose.h"
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/phase_vocoder.h"
#include "effects/pitch_shift.h"
#include "effects/remix.h"
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

SonareError sonare_decompose(const float* s, int n_features, int n_frames, int n_components,
                             int n_iter, float beta, float** out_w, size_t* out_w_length,
                             float** out_h, size_t* out_h_length) {
  if (!out_w || !out_w_length || !out_h || !out_h_length) return SONARE_ERROR_INVALID_PARAMETER;
  *out_w = nullptr;
  *out_w_length = 0;
  *out_h = nullptr;
  *out_h_length = 0;
  if (!s || n_features <= 0 || n_frames <= 0 || n_components <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // Reject dims whose product would overflow size_t before the core indexes
  // n_features * n_frames elements of the caller-owned buffer.
  if (static_cast<size_t>(n_features) > SIZE_MAX / static_cast<size_t>(n_frames)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  SONARE_C_TRY
  DecomposeResult result = decompose(s, n_features, n_frames, n_components, n_iter, "mu", beta);

  std::unique_ptr<float[]> w(new float[result.W.size()]);
  std::memcpy(w.get(), result.W.data(), result.W.size() * sizeof(float));
  std::unique_ptr<float[]> h(new float[result.H.size()]);
  std::memcpy(h.get(), result.H.data(), result.H.size() * sizeof(float));

  *out_w_length = result.W.size();
  *out_h_length = result.H.size();
  *out_w = release_array(w);
  *out_h = release_array(h);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_nn_filter(const float* s, int n_features, int n_frames, const char* aggregate,
                             int k, int width, float** out, size_t* out_length) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  *out_length = 0;
  if (!s || n_features <= 0 || n_frames <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  // Reject dims whose product would overflow size_t before the core indexes
  // n_features * n_frames elements of the caller-owned buffer.
  if (static_cast<size_t>(n_features) > SIZE_MAX / static_cast<size_t>(n_frames)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  SONARE_C_TRY
  std::string agg = aggregate ? aggregate : "mean";
  std::vector<float> result = nn_filter(s, n_features, n_frames, agg, k, width);

  std::unique_ptr<float[]> data(new float[result.size()]);
  std::memcpy(data.get(), result.data(), result.size() * sizeof(float));
  *out_length = result.size();
  *out = release_array(data);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_remix(const float* samples, size_t length, int sample_rate, const int* intervals,
                         size_t interval_count, int align_zeros, float** out, size_t* out_length) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  *out_length = 0;
  if (interval_count > 0 && !intervals) return SONARE_ERROR_INVALID_PARAMETER;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    std::vector<std::pair<int, int>> pairs;
    pairs.reserve(interval_count);
    for (size_t i = 0; i < interval_count; ++i) {
      pairs.emplace_back(intervals[2 * i], intervals[2 * i + 1]);
    }
    std::vector<float> result = remix(audio.data(), audio.size(), pairs, align_zeros != 0);

    std::unique_ptr<float[]> data(new float[result.size()]);
    std::memcpy(data.get(), result.data(), result.size() * sizeof(float));
    *out_length = result.size();
    *out = release_array(data);
    return SONARE_OK;
  });
}

SonareError sonare_hpss_with_residual(const float* samples, size_t length, int sample_rate,
                                      int kernel_harmonic, int kernel_percussive,
                                      float** out_harmonic, float** out_percussive,
                                      float** out_residual, size_t* out_length,
                                      int* out_sample_rate) {
  if (!out_harmonic || !out_percussive || !out_residual || !out_length || !out_sample_rate) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out_harmonic = nullptr;
  *out_percussive = nullptr;
  *out_residual = nullptr;
  *out_length = 0;
  *out_sample_rate = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    HpssConfig config;
    config.kernel_size_harmonic = kernel_harmonic;
    config.kernel_size_percussive = kernel_percussive;
    HpssAudioResultWithResidual result = hpss_with_residual(audio, config);

    size_t n = result.harmonic.size();
    std::unique_ptr<float[]> harmonic(new float[n]);
    std::unique_ptr<float[]> percussive(new float[n]);
    std::unique_ptr<float[]> residual(new float[n]);
    std::memcpy(harmonic.get(), result.harmonic.data(), n * sizeof(float));
    std::memcpy(percussive.get(), result.percussive.data(), n * sizeof(float));
    std::memcpy(residual.get(), result.residual.data(), n * sizeof(float));

    *out_length = n;
    *out_sample_rate = result.harmonic.sample_rate();
    *out_harmonic = release_array(harmonic);
    *out_percussive = release_array(percussive);
    *out_residual = release_array(residual);
    return SONARE_OK;
  });
}

SonareError sonare_phase_vocoder(const float* samples, size_t length, int sample_rate, float rate,
                                 int n_fft, int hop_length, float** out, size_t* out_length) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  *out_length = 0;
  if (rate <= 0.0f) return SONARE_ERROR_INVALID_PARAMETER;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    StftConfig stft_config;
    stft_config.n_fft = n_fft;
    stft_config.hop_length = hop_length;
    Spectrogram spec = Spectrogram::compute(audio, stft_config);

    PhaseVocoderConfig pv_config;
    pv_config.hop_length = hop_length;
    Spectrogram stretched = phase_vocoder(spec, rate, pv_config);

    int expected_length = static_cast<int>(std::ceil(static_cast<float>(audio.size()) / rate));
    Audio result = stretched.to_audio(expected_length);

    std::unique_ptr<float[]> data(new float[result.size()]);
    std::memcpy(data.get(), result.data(), result.size() * sizeof(float));
    *out_length = result.size();
    *out = release_array(data);
    return SONARE_OK;
  });
}
