#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/audio.h"
#include "core/convert.h"
#include "core/resample.h"
#include "core/spectrum.h"
#include "feature/chroma.h"
#include "feature/inverse.h"
#include "feature/mel_spectrogram.h"
#include "feature/nnls_chroma.h"
#include "feature/onset.h"
#include "feature/pitch.h"
#include "feature/spectral.h"
#include "sonare_c.h"
#include "sonare_c_internal.h"
#include "streaming/stream_analyzer.h"
#include "streaming/stream_config.h"
#include "streaming/stream_frame.h"

using namespace sonare;
using namespace sonare_c_detail;

namespace {

NnlsChromaConfig make_fast_nnls_chroma_config(int hop_length = 512) {
  NnlsChromaConfig config;
  config.cqt.bins_per_octave = 12;
  config.cqt.n_bins = 84;
  config.cqt.hop_length = hop_length;
  config.midi_min = 24;
  config.n_pitches = 60;
  config.n_harmonics = 4;
  config.max_iter = 25;
  config.tolerance = 1.0e-3f;
  return config;
}

}  // namespace

// Features - Spectrogram
// ============================================================================

SonareError sonare_stft(const float* samples, size_t length, int sample_rate, int n_fft,
                        int hop_length, SonareStftResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;

  out->magnitude = nullptr;
  out->power = nullptr;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
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
  });
}

SonareError sonare_stft_db(const float* samples, size_t length, int sample_rate, int n_fft,
                           int hop_length, int* out_n_bins, int* out_n_frames, float** out_db) {
  if (!out_n_bins || !out_n_frames || !out_db) return SONARE_ERROR_INVALID_PARAMETER;

  *out_db = nullptr;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    StftConfig config;
    config.n_fft = n_fft;
    config.hop_length = hop_length;
    Spectrogram spec = Spectrogram::compute(audio, config);

    *out_n_bins = spec.n_bins();
    *out_n_frames = spec.n_frames();
    std::vector<float> db = spec.to_db();
    std::unique_ptr<float[]> db_out(new float[db.size()]);
    std::memcpy(db_out.get(), db.data(), db.size() * sizeof(float));
    *out_db = release_array(db_out);
    return SONARE_OK;
  });
}

// Features - Mel
// ============================================================================

SonareError sonare_mel_spectrogram(const float* samples, size_t length, int sample_rate, int n_fft,
                                   int hop_length, int n_mels, SonareMelResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;

  out->power = nullptr;
  out->db = nullptr;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
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
  });
}

SonareError sonare_mfcc(const float* samples, size_t length, int sample_rate, int n_fft,
                        int hop_length, int n_mels, int n_mfcc, SonareMfccResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;

  out->coefficients = nullptr;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    MelConfig config;
    config.n_fft = n_fft;
    config.hop_length = hop_length;
    config.n_mels = n_mels;
    MelSpectrogram mel = MelSpectrogram::compute(audio, config);
    std::vector<float> mfcc_data = mel.mfcc(n_mfcc);

    out->n_mfcc = n_mfcc;
    out->n_frames = mel.n_frames();
    std::unique_ptr<float[]> coeffs(new float[mfcc_data.size()]);
    std::memcpy(coeffs.get(), mfcc_data.data(), mfcc_data.size() * sizeof(float));
    out->coefficients = release_array(coeffs);
    return SONARE_OK;
  });
}

// Features - Inverse reconstruction (Mel/MFCC -> spectrogram -> audio)
// ============================================================================

namespace {

// Copies a row-major matrix into a freshly allocated SonareInverseResult.
SonareError fill_inverse_result(const std::vector<float>& data, int rows, int n_frames,
                                SonareInverseResult* out) {
  *out = {};
  out->rows = rows;
  out->n_frames = n_frames;
  if (!data.empty()) {
    std::unique_ptr<float[]> buf(new float[data.size()]);
    std::memcpy(buf.get(), data.data(), data.size() * sizeof(float));
    out->data = release_array(buf);
  }
  return SONARE_OK;
}

// Copies audio samples into a freshly allocated float array output.
SonareError fill_audio_samples(const Audio& audio, float** out, size_t* out_length) {
  *out = nullptr;
  *out_length = audio.size();
  if (audio.size() == 0) return SONARE_OK;
  std::unique_ptr<float[]> buf(new float[audio.size()]);
  std::memcpy(buf.get(), audio.data(), audio.size() * sizeof(float));
  *out = release_array(buf);
  return SONARE_OK;
}

}  // namespace

SonareError sonare_mel_to_stft(const float* mel, int n_mels, int n_frames, int sample_rate,
                               int n_fft, float fmin, float fmax, SonareInverseResult* out) {
  if (!out || !mel) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_mels <= 0 || n_frames <= 0 || n_fft <= 0 || sample_rate <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  *out = {};

  SONARE_C_TRY
  MelConfig config;
  config.n_mels = n_mels;
  config.n_fft = n_fft;
  config.fmin = fmin;
  config.fmax = fmax;
  std::vector<float> stft = mel_to_stft(mel, n_mels, n_frames, config, sample_rate);
  return fill_inverse_result(stft, n_fft / 2 + 1, n_frames, out);
  SONARE_C_CATCH
}

SonareError sonare_mel_to_audio(const float* mel, int n_mels, int n_frames, int sample_rate,
                                int n_fft, int hop_length, float fmin, float fmax, int n_iter,
                                float** out, size_t* out_length) {
  if (!out || !out_length || !mel) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_mels <= 0 || n_frames <= 0 || n_fft <= 0 || hop_length <= 0 || sample_rate <= 0 ||
      n_iter <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  *out = nullptr;
  *out_length = 0;

  SONARE_C_TRY
  MelConfig config;
  config.n_mels = n_mels;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.fmax = fmax;
  Audio audio = mel_to_audio(mel, n_mels, n_frames, config, n_iter, sample_rate);
  return fill_audio_samples(audio, out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_mfcc_to_mel(const float* mfcc, int n_mfcc, int n_frames, int n_mels,
                               SonareInverseResult* out) {
  if (!out || !mfcc) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_mfcc <= 0 || n_frames <= 0 || n_mels <= 0) return SONARE_ERROR_INVALID_PARAMETER;

  *out = {};

  SONARE_C_TRY
  std::vector<float> mel = mfcc_to_mel(mfcc, n_mfcc, n_frames, n_mels);
  return fill_inverse_result(mel, n_mels, n_frames, out);
  SONARE_C_CATCH
}

SonareError sonare_mfcc_to_audio(const float* mfcc, int n_mfcc, int n_frames, int n_mels,
                                 int sample_rate, int n_fft, int hop_length, float fmin, float fmax,
                                 int n_iter, float** out, size_t* out_length) {
  if (!out || !out_length || !mfcc) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_mfcc <= 0 || n_frames <= 0 || n_mels <= 0 || n_fft <= 0 || hop_length <= 0 ||
      sample_rate <= 0 || n_iter <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  *out = nullptr;
  *out_length = 0;

  SONARE_C_TRY
  MelConfig config;
  config.n_mels = n_mels;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.fmax = fmax;
  Audio audio = mfcc_to_audio(mfcc, n_mfcc, n_frames, config, n_iter, sample_rate);
  return fill_audio_samples(audio, out, out_length);
  SONARE_C_CATCH
}

void sonare_free_inverse_result(SonareInverseResult* result) {
  if (!result) return;
  delete[] result->data;
  result->data = nullptr;
  result->rows = 0;
  result->n_frames = 0;
}

// Features - Onset
// ============================================================================

SonareError sonare_onset_strength(const float* samples, size_t length, int sr, int n_fft,
                                  int hop_length, int n_mels, float** out, size_t* out_length) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;

  *out = nullptr;
  *out_length = 0;

  return run_offline(samples, length, sr, [&](const Audio& audio) -> SonareError {
    MelConfig mel_config;
    mel_config.n_fft = n_fft;
    mel_config.hop_length = hop_length;
    mel_config.n_mels = n_mels;
    std::vector<float> env = compute_onset_strength(audio, mel_config, OnsetConfig());

    *out_length = env.size();
    if (env.empty()) return SONARE_OK;
    std::unique_ptr<float[]> data(new float[env.size()]);
    std::memcpy(data.get(), env.data(), env.size() * sizeof(float));
    *out = release_array(data);
    return SONARE_OK;
  });
}

// Features - Chroma
// ============================================================================

SonareError sonare_chroma(const float* samples, size_t length, int sample_rate, int n_fft,
                          int hop_length, SonareChromaResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;

  out->features = nullptr;
  out->mean_energy = nullptr;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    ChromaConfig config;
    config.n_fft = n_fft;
    config.hop_length = hop_length;
    Chroma chroma = Chroma::compute(audio, config);

    out->n_chroma = chroma.n_chroma();
    out->n_frames = chroma.n_frames();
    out->sample_rate = chroma.sample_rate();
    out->hop_length = chroma.hop_length();

    size_t total = static_cast<size_t>(chroma.n_chroma()) * chroma.n_frames();
    std::unique_ptr<float[]> features(new float[total]);
    std::memcpy(features.get(), chroma.data(), total * sizeof(float));
    out->features = release_array(features);

    auto mean = chroma.mean_energy();
    std::unique_ptr<float[]> mean_out(new float[chroma.n_chroma()]);
    std::memcpy(mean_out.get(), mean.data(), chroma.n_chroma() * sizeof(float));
    out->mean_energy = release_array(mean_out);
    return SONARE_OK;
  });
}

SonareError sonare_nnls_chroma(const float* samples, size_t length, int sr, float** out,
                               size_t* out_length, int* out_n_frames) {
  if (!out || !out_length || !out_n_frames) return SONARE_ERROR_INVALID_PARAMETER;

  *out = nullptr;

  return run_offline(samples, length, sr, [&](const Audio& audio) -> SonareError {
    Chroma chroma = nnls_chroma(audio, make_fast_nnls_chroma_config());

    *out_n_frames = chroma.n_frames();
    size_t total = static_cast<size_t>(chroma.n_chroma()) * chroma.n_frames();
    *out_length = total;
    if (total == 0) return SONARE_OK;
    std::unique_ptr<float[]> features(new float[total]);
    std::memcpy(features.get(), chroma.data(), total * sizeof(float));
    *out = release_array(features);
    return SONARE_OK;
  });
}

// Features - Spectral
// ============================================================================

SonareError sonare_spectral_centroid(const float* samples, size_t length, int sample_rate,
                                     int n_fft, int hop_length, float** out, size_t* out_count) {
  if (!out || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  if (out) *out = nullptr;
  if (out_count) *out_count = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    StftConfig config;
    config.n_fft = n_fft;
    config.hop_length = hop_length;
    Spectrogram spec = Spectrogram::compute(audio, config);
    std::vector<float> result = spectral_centroid(spec, audio.sample_rate());
    *out_count = result.size();
    *out = new float[result.size()];
    std::memcpy(*out, result.data(), result.size() * sizeof(float));
    return SONARE_OK;
  });
}

SonareError sonare_spectral_bandwidth(const float* samples, size_t length, int sample_rate,
                                      int n_fft, int hop_length, float** out, size_t* out_count) {
  if (!out || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  if (out) *out = nullptr;
  if (out_count) *out_count = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    StftConfig config;
    config.n_fft = n_fft;
    config.hop_length = hop_length;
    Spectrogram spec = Spectrogram::compute(audio, config);
    std::vector<float> result = spectral_bandwidth(spec, audio.sample_rate());
    *out_count = result.size();
    *out = new float[result.size()];
    std::memcpy(*out, result.data(), result.size() * sizeof(float));
    return SONARE_OK;
  });
}

SonareError sonare_spectral_rolloff(const float* samples, size_t length, int sample_rate, int n_fft,
                                    int hop_length, float roll_percent, float** out,
                                    size_t* out_count) {
  if (!out || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  if (out) *out = nullptr;
  if (out_count) *out_count = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    StftConfig config;
    config.n_fft = n_fft;
    config.hop_length = hop_length;
    Spectrogram spec = Spectrogram::compute(audio, config);
    std::vector<float> result = spectral_rolloff(spec, audio.sample_rate(), roll_percent);
    *out_count = result.size();
    *out = new float[result.size()];
    std::memcpy(*out, result.data(), result.size() * sizeof(float));
    return SONARE_OK;
  });
}

SonareError sonare_spectral_flatness(const float* samples, size_t length, int sample_rate,
                                     int n_fft, int hop_length, float** out, size_t* out_count) {
  if (!out || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  *out_count = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    StftConfig config;
    config.n_fft = n_fft;
    config.hop_length = hop_length;
    Spectrogram spec = Spectrogram::compute(audio, config);
    std::vector<float> result = spectral_flatness(spec);
    *out_count = result.size();
    *out = new float[result.size()];
    std::memcpy(*out, result.data(), result.size() * sizeof(float));
    return SONARE_OK;
  });
}

SonareError sonare_zero_crossing_rate(const float* samples, size_t length, int sample_rate,
                                      int frame_length, int hop_length, float** out,
                                      size_t* out_count) {
  if (!out || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  *out_count = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    std::vector<float> result = zero_crossing_rate(audio, frame_length, hop_length);
    *out_count = result.size();
    *out = new float[result.size()];
    std::memcpy(*out, result.data(), result.size() * sizeof(float));
    return SONARE_OK;
  });
}

SonareError sonare_rms_energy(const float* samples, size_t length, int sample_rate,
                              int frame_length, int hop_length, float** out, size_t* out_count) {
  if (!out || !out_count) return SONARE_ERROR_INVALID_PARAMETER;

  *out = nullptr;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    std::vector<float> result = rms_energy(audio, frame_length, hop_length);
    *out_count = result.size();
    std::unique_ptr<float[]> tmp(new float[result.size()]);
    std::memcpy(tmp.get(), result.data(), result.size() * sizeof(float));
    *out = release_array(tmp);
    return SONARE_OK;
  });
}

SonareError sonare_spectral_contrast(const float* samples, size_t length, int sample_rate,
                                     int n_fft, int hop_length, int n_bands, float fmin,
                                     float quantile, float** out, int* out_rows, int* out_cols) {
  if (!out || !out_rows || !out_cols) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  *out_rows = 0;
  *out_cols = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    StftConfig config;
    config.n_fft = n_fft;
    config.hop_length = hop_length;
    Spectrogram spec = Spectrogram::compute(audio, config);
    std::vector<float> result =
        spectral_contrast(spec, audio.sample_rate(), n_bands, fmin, quantile);
    int rows = n_bands + 1;
    int cols = spec.n_frames();
    *out_rows = rows;
    *out_cols = cols;
    std::unique_ptr<float[]> tmp(new float[result.size()]);
    std::memcpy(tmp.get(), result.data(), result.size() * sizeof(float));
    *out = release_array(tmp);
    return SONARE_OK;
  });
}

SonareError sonare_poly_features(const float* samples, size_t length, int sample_rate, int n_fft,
                                 int hop_length, int order, float** out, int* out_rows,
                                 int* out_cols) {
  if (!out || !out_rows || !out_cols) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  *out_rows = 0;
  *out_cols = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    StftConfig config;
    config.n_fft = n_fft;
    config.hop_length = hop_length;
    Spectrogram spec = Spectrogram::compute(audio, config);
    std::vector<float> result = poly_features(spec, audio.sample_rate(), order);
    int rows = order + 1;
    int cols = spec.n_frames();
    *out_rows = rows;
    *out_cols = cols;
    std::unique_ptr<float[]> tmp(new float[result.size()]);
    std::memcpy(tmp.get(), result.data(), result.size() * sizeof(float));
    *out = release_array(tmp);
    return SONARE_OK;
  });
}

SonareError sonare_zero_crossings(const float* samples, size_t length, float threshold,
                                  int ref_magnitude, int pad, int zero_pos, int** out,
                                  size_t* out_count) {
  if (!out || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  *out_count = 0;
  if (!samples && length > 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (!(threshold >= 0.0f)) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  std::vector<int> result =
      zero_crossings(samples, length, threshold, ref_magnitude != 0, pad != 0, zero_pos != 0);
  *out_count = result.size();
  if (result.empty()) return SONARE_OK;
  std::unique_ptr<int[]> tmp(new int[result.size()]);
  std::memcpy(tmp.get(), result.data(), result.size() * sizeof(int));
  *out = release_array(tmp);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_pitch_tuning(const float* frequencies, size_t length, float resolution,
                                int bins_per_octave, float* out_tuning) {
  if (!out_tuning) return SONARE_ERROR_INVALID_PARAMETER;
  *out_tuning = 0.0f;
  if (!frequencies && length > 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (!(resolution > 0.0f) || bins_per_octave <= 0) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  std::vector<float> freqs;
  if (frequencies && length > 0) {
    freqs.assign(frequencies, frequencies + length);
  }
  *out_tuning = pitch_tuning(freqs, resolution, bins_per_octave);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_estimate_tuning(const float* samples, size_t length, int sample_rate, int n_fft,
                                   int hop_length, float resolution, int bins_per_octave,
                                   float* out_tuning) {
  if (!out_tuning) return SONARE_ERROR_INVALID_PARAMETER;
  *out_tuning = 0.0f;
  if (!(resolution > 0.0f) || bins_per_octave <= 0) return SONARE_ERROR_INVALID_PARAMETER;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    *out_tuning = estimate_tuning(audio, n_fft, hop_length, resolution, bins_per_octave);
    return SONARE_OK;
  });
}

namespace {

SonareError fill_pitch_result(const PitchResult& result, SonarePitchResult* out) {
  out->n_frames = result.n_frames();
  out->median_f0 = result.median_f0();
  out->mean_f0 = result.mean_f0();

  size_t n = static_cast<size_t>(result.n_frames());
  std::unique_ptr<float[]> f0(new float[n]);
  std::unique_ptr<float[]> voiced_prob(new float[n]);
  std::unique_ptr<int[]> voiced_flag(new int[n]);

  std::memcpy(f0.get(), result.f0.data(), n * sizeof(float));
  std::memcpy(voiced_prob.get(), result.voiced_prob.data(), n * sizeof(float));
  for (size_t i = 0; i < n; ++i) {
    voiced_flag[i] = result.voiced_flag[i] ? 1 : 0;
  }

  out->f0 = release_array(f0);
  out->voiced_prob = release_array(voiced_prob);
  out->voiced_flag = release_array(voiced_flag);
  return SONARE_OK;
}

}  // namespace

SonareError sonare_pitch_yin(const float* samples, size_t length, int sample_rate, int frame_length,
                             int hop_length, float fmin, float fmax, float threshold, int fill_na,
                             SonarePitchResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;

  out->f0 = nullptr;
  out->voiced_prob = nullptr;
  out->voiced_flag = nullptr;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    PitchConfig config;
    config.frame_length = frame_length;
    config.hop_length = hop_length;
    config.fmin = fmin;
    config.fmax = fmax;
    config.threshold = threshold;
    config.fill_na = fill_na != 0;
    PitchResult result = yin_track(audio, config);
    return fill_pitch_result(result, out);
  });
}

SonareError sonare_pitch_pyin(const float* samples, size_t length, int sample_rate,
                              int frame_length, int hop_length, float fmin, float fmax,
                              float threshold, int fill_na, SonarePitchResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;

  out->f0 = nullptr;
  out->voiced_prob = nullptr;
  out->voiced_flag = nullptr;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    PitchConfig config;
    config.frame_length = frame_length;
    config.hop_length = hop_length;
    config.fmin = fmin;
    config.fmax = fmax;
    config.threshold = threshold;
    config.fill_na = fill_na != 0;
    PitchResult result = pyin(audio, config);
    return fill_pitch_result(result, out);
  });
}

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
// Streaming - StreamAnalyzer (stateful real-time frame analyzer)
// ============================================================================

// Opaque handle backing SonareStreamAnalyzer. Owns the C++ analyzer instance.
struct SonareStreamAnalyzer {
  std::unique_ptr<sonare::StreamAnalyzer> analyzer;
};

namespace {

// Copies a float vector into a freshly allocated array (nulls on empty).
float* copy_float_vector(const std::vector<float>& v) {
  if (v.empty()) return nullptr;
  std::unique_ptr<float[]> buf(new float[v.size()]);
  std::memcpy(buf.get(), v.data(), v.size() * sizeof(float));
  return release_array(buf);
}

// Copies an int vector into a freshly allocated array (nulls on empty).
int* copy_int_vector(const std::vector<int>& v) {
  if (v.empty()) return nullptr;
  std::unique_ptr<int[]> buf(new int[v.size()]);
  std::memcpy(buf.get(), v.data(), v.size() * sizeof(int));
  return release_array(buf);
}

uint8_t* copy_u8_vector(const std::vector<uint8_t>& v) {
  if (v.empty()) return nullptr;
  std::unique_ptr<uint8_t[]> buf(new uint8_t[v.size()]);
  std::memcpy(buf.get(), v.data(), v.size() * sizeof(uint8_t));
  return release_array(buf);
}

int16_t* copy_i16_vector(const std::vector<int16_t>& v) {
  if (v.empty()) return nullptr;
  std::unique_ptr<int16_t[]> buf(new int16_t[v.size()]);
  std::memcpy(buf.get(), v.data(), v.size() * sizeof(int16_t));
  return release_array(buf);
}

bool finite_positive(float value) { return std::isfinite(value) && value > 0.0f; }

bool finite_non_negative(float value) { return std::isfinite(value) && value >= 0.0f; }

bool valid_window(int value) {
  return value >= SONARE_WINDOW_HANN && value <= SONARE_WINDOW_RECTANGULAR;
}

bool valid_output_format(int value) {
  return value >= SONARE_STREAM_OUTPUT_FLOAT32 && value <= SONARE_STREAM_OUTPUT_UINT8;
}

WindowType to_window_type(int value) {
  switch (static_cast<SonareWindowType>(value)) {
    case SONARE_WINDOW_HAMMING:
      return WindowType::Hamming;
    case SONARE_WINDOW_BLACKMAN:
      return WindowType::Blackman;
    case SONARE_WINDOW_RECTANGULAR:
      return WindowType::Rectangular;
    case SONARE_WINDOW_HANN:
    default:
      return WindowType::Hann;
  }
}

OutputFormat to_output_format(int value) {
  switch (static_cast<SonareStreamOutputFormat>(value)) {
    case SONARE_STREAM_OUTPUT_INT16:
      return OutputFormat::Int16;
    case SONARE_STREAM_OUTPUT_UINT8:
      return OutputFormat::Uint8;
    case SONARE_STREAM_OUTPUT_FLOAT32:
    default:
      return OutputFormat::Float32;
  }
}

SonareStreamChordChange* copy_chord_changes(const std::vector<ChordChange>& v) {
  if (v.empty()) return nullptr;
  std::unique_ptr<SonareStreamChordChange[]> buf(new SonareStreamChordChange[v.size()]);
  for (size_t i = 0; i < v.size(); ++i) {
    buf[i] = {v[i].root, v[i].quality, v[i].start_time, v[i].confidence};
  }
  return release_array(buf);
}

SonareStreamBarChord* copy_bar_chords(const std::vector<BarChord>& v) {
  if (v.empty()) return nullptr;
  std::unique_ptr<SonareStreamBarChord[]> buf(new SonareStreamBarChord[v.size()]);
  for (size_t i = 0; i < v.size(); ++i) {
    buf[i] = {v[i].bar_index, v[i].root, v[i].quality, v[i].start_time, v[i].confidence};
  }
  return release_array(buf);
}

SonareStreamPatternScore* copy_pattern_scores(const std::vector<std::pair<std::string, float>>& v) {
  if (v.empty()) return nullptr;
  std::unique_ptr<SonareStreamPatternScore[]> buf(new SonareStreamPatternScore[v.size()]);
  for (size_t i = 0; i < v.size(); ++i) {
    std::strncpy(buf[i].name, v[i].first.c_str(), sizeof(buf[i].name) - 1);
    buf[i].name[sizeof(buf[i].name) - 1] = '\0';
    buf[i].score = v[i].second;
  }
  return release_array(buf);
}

}  // namespace

SonareError sonare_stream_analyzer_config_default(SonareStreamConfig* config) {
  if (!config) return SONARE_ERROR_INVALID_PARAMETER;
  StreamConfig defaults;
  config->sample_rate = defaults.sample_rate;
  config->n_fft = defaults.n_fft;
  config->hop_length = defaults.hop_length;
  config->n_mels = defaults.n_mels;
  config->fmin = defaults.fmin;
  config->fmax = defaults.fmax;
  config->tuning_ref_hz = defaults.tuning_ref_hz;
  config->compute_magnitude = defaults.compute_magnitude ? 1 : 0;
  config->compute_mel = defaults.compute_mel ? 1 : 0;
  config->compute_chroma = defaults.compute_chroma ? 1 : 0;
  config->compute_onset = defaults.compute_onset ? 1 : 0;
  config->compute_spectral = defaults.compute_spectral ? 1 : 0;
  config->emit_every_n_frames = defaults.emit_every_n_frames;
  config->magnitude_downsample = defaults.magnitude_downsample;
  config->key_update_interval_sec = defaults.key_update_interval_sec;
  config->bpm_update_interval_sec = defaults.bpm_update_interval_sec;
  config->window = SONARE_WINDOW_HANN;
  config->output_format = SONARE_STREAM_OUTPUT_FLOAT32;
  return SONARE_OK;
}

SonareError sonare_stream_analyzer_create(const SonareStreamConfig* config,
                                          SonareStreamAnalyzer** out) {
  if (!config || !out) return SONARE_ERROR_INVALID_PARAMETER;
  if (config->sample_rate <= 0 || config->n_fft <= 0 || config->hop_length <= 0 ||
      config->hop_length > config->n_fft || config->n_mels <= 0 ||
      config->emit_every_n_frames <= 0 || config->magnitude_downsample <= 0 ||
      !finite_non_negative(config->fmin) || !finite_non_negative(config->fmax) ||
      (config->fmax > 0.0f && config->fmax <= config->fmin) ||
      !finite_positive(config->tuning_ref_hz) ||
      !finite_positive(config->key_update_interval_sec) ||
      !finite_positive(config->bpm_update_interval_sec) || !valid_window(config->window) ||
      !valid_output_format(config->output_format)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  *out = nullptr;

  SONARE_C_TRY
  StreamConfig cfg;
  cfg.sample_rate = config->sample_rate;
  cfg.n_fft = config->n_fft;
  cfg.hop_length = config->hop_length;
  cfg.n_mels = config->n_mels;
  cfg.fmin = config->fmin;
  cfg.fmax = config->fmax;
  cfg.tuning_ref_hz = config->tuning_ref_hz;
  cfg.window = to_window_type(config->window);
  cfg.compute_magnitude = config->compute_magnitude != 0;
  cfg.compute_mel = config->compute_mel != 0;
  cfg.compute_chroma = config->compute_chroma != 0;
  cfg.compute_onset = config->compute_onset != 0;
  cfg.compute_spectral = config->compute_spectral != 0;
  cfg.emit_every_n_frames = config->emit_every_n_frames;
  cfg.magnitude_downsample = config->magnitude_downsample;
  cfg.output_format = to_output_format(config->output_format);
  cfg.key_update_interval_sec = config->key_update_interval_sec;
  cfg.bpm_update_interval_sec = config->bpm_update_interval_sec;

  auto handle = std::make_unique<SonareStreamAnalyzer>();
  handle->analyzer = std::make_unique<StreamAnalyzer>(cfg);
  *out = handle.release();
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_stream_analyzer_destroy(SonareStreamAnalyzer* analyzer) { delete analyzer; }

SonareError sonare_stream_analyzer_process(SonareStreamAnalyzer* analyzer, const float* samples,
                                           size_t n_samples) {
  if (!analyzer || !analyzer->analyzer || (!samples && n_samples > 0)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  analyzer->analyzer->process(samples, n_samples);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_process_with_offset(SonareStreamAnalyzer* analyzer,
                                                       const float* samples, size_t n_samples,
                                                       size_t sample_offset) {
  if (!analyzer || !analyzer->analyzer || (!samples && n_samples > 0)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  analyzer->analyzer->process(samples, n_samples, sample_offset);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_available_frames(SonareStreamAnalyzer* analyzer,
                                                    size_t* out_count) {
  if (!analyzer || !analyzer->analyzer || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  *out_count = analyzer->analyzer->available_frames();
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_read_frames(SonareStreamAnalyzer* analyzer, size_t max_frames,
                                               SonareStreamFrames* out) {
  if (!analyzer || !analyzer->analyzer || !out) return SONARE_ERROR_INVALID_PARAMETER;

  *out = {};

  SONARE_C_TRY
  FrameBuffer buffer;
  analyzer->analyzer->read_frames_soa(max_frames, buffer);

  out->n_frames = static_cast<int>(buffer.n_frames);
  out->n_mels = analyzer->analyzer->config().n_mels;
  out->timestamps = copy_float_vector(buffer.timestamps);
  out->mel = copy_float_vector(buffer.mel);
  out->chroma = copy_float_vector(buffer.chroma);
  out->onset_strength = copy_float_vector(buffer.onset_strength);
  out->rms_energy = copy_float_vector(buffer.rms_energy);
  out->spectral_centroid = copy_float_vector(buffer.spectral_centroid);
  out->spectral_flatness = copy_float_vector(buffer.spectral_flatness);
  out->chord_root = copy_int_vector(buffer.chord_root);
  out->chord_quality = copy_int_vector(buffer.chord_quality);
  out->chord_confidence = copy_float_vector(buffer.chord_confidence);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_read_frames_u8(SonareStreamAnalyzer* analyzer, size_t max_frames,
                                                  SonareStreamFramesU8* out) {
  if (!analyzer || !analyzer->analyzer || !out) return SONARE_ERROR_INVALID_PARAMETER;

  *out = {};

  SONARE_C_TRY
  QuantizedFrameBufferU8 buffer;
  analyzer->analyzer->read_frames_quantized_u8(max_frames, buffer);

  out->n_frames = static_cast<int>(buffer.n_frames);
  out->n_mels = buffer.n_mels;
  out->timestamps = copy_float_vector(buffer.timestamps);
  out->mel = copy_u8_vector(buffer.mel);
  out->chroma = copy_u8_vector(buffer.chroma);
  out->onset_strength = copy_u8_vector(buffer.onset_strength);
  out->rms_energy = copy_u8_vector(buffer.rms_energy);
  out->spectral_centroid = copy_u8_vector(buffer.spectral_centroid);
  out->spectral_flatness = copy_u8_vector(buffer.spectral_flatness);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_read_frames_i16(SonareStreamAnalyzer* analyzer,
                                                   size_t max_frames, SonareStreamFramesI16* out) {
  if (!analyzer || !analyzer->analyzer || !out) return SONARE_ERROR_INVALID_PARAMETER;

  *out = {};

  SONARE_C_TRY
  QuantizedFrameBufferI16 buffer;
  analyzer->analyzer->read_frames_quantized_i16(max_frames, buffer);

  out->n_frames = static_cast<int>(buffer.n_frames);
  out->n_mels = buffer.n_mels;
  out->timestamps = copy_float_vector(buffer.timestamps);
  out->mel = copy_i16_vector(buffer.mel);
  out->chroma = copy_i16_vector(buffer.chroma);
  out->onset_strength = copy_i16_vector(buffer.onset_strength);
  out->rms_energy = copy_i16_vector(buffer.rms_energy);
  out->spectral_centroid = copy_i16_vector(buffer.spectral_centroid);
  out->spectral_flatness = copy_i16_vector(buffer.spectral_flatness);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_reset(SonareStreamAnalyzer* analyzer,
                                         size_t base_sample_offset) {
  if (!analyzer || !analyzer->analyzer) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  analyzer->analyzer->reset(base_sample_offset);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_stats(SonareStreamAnalyzer* analyzer, SonareStreamStats* out) {
  if (!analyzer || !analyzer->analyzer || !out) return SONARE_ERROR_INVALID_PARAMETER;

  *out = {};

  SONARE_C_TRY
  AnalyzerStats s = analyzer->analyzer->stats();
  out->total_frames = s.total_frames;
  out->total_samples = s.total_samples;
  out->duration_seconds = s.duration_seconds;
  out->bpm = s.estimate.bpm;
  out->bpm_confidence = s.estimate.bpm_confidence;
  out->bpm_candidate_count = s.estimate.bpm_candidate_count;
  out->key = s.estimate.key;
  out->key_minor = s.estimate.key_minor ? 1 : 0;
  out->key_confidence = s.estimate.key_confidence;
  out->chord_root = s.estimate.chord_root;
  out->chord_quality = s.estimate.chord_quality;
  out->chord_confidence = s.estimate.chord_confidence;
  out->chord_start_time = s.estimate.chord_start_time;
  out->current_bar = s.estimate.current_bar;
  out->bar_duration = s.estimate.bar_duration;
  out->chord_progression_count = s.estimate.chord_progression.size();
  out->chord_progression = copy_chord_changes(s.estimate.chord_progression);
  out->bar_chord_progression_count = s.estimate.bar_chord_progression.size();
  out->bar_chord_progression = copy_bar_chords(s.estimate.bar_chord_progression);
  out->pattern_length = s.estimate.pattern_length;
  out->voted_pattern_count = s.estimate.voted_pattern.size();
  out->voted_pattern = copy_bar_chords(s.estimate.voted_pattern);
  std::strncpy(out->detected_pattern_name, s.estimate.detected_pattern_name.c_str(),
               sizeof(out->detected_pattern_name) - 1);
  out->detected_pattern_name[sizeof(out->detected_pattern_name) - 1] = '\0';
  out->detected_pattern_score = s.estimate.detected_pattern_score;
  out->all_pattern_scores_count = s.estimate.all_pattern_scores.size();
  out->all_pattern_scores = copy_pattern_scores(s.estimate.all_pattern_scores);
  out->accumulated_seconds = s.estimate.accumulated_seconds;
  out->used_frames = s.estimate.used_frames;
  out->updated = s.estimate.updated ? 1 : 0;
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_free_stream_stats(SonareStreamStats* stats) {
  if (!stats) return;
  delete[] stats->chord_progression;
  delete[] stats->bar_chord_progression;
  delete[] stats->voted_pattern;
  delete[] stats->all_pattern_scores;
  *stats = {};
}

SonareError sonare_stream_analyzer_frame_count(SonareStreamAnalyzer* analyzer, int* out_count) {
  if (!analyzer || !analyzer->analyzer || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  *out_count = analyzer->analyzer->frame_count();
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_current_time(SonareStreamAnalyzer* analyzer,
                                                float* out_seconds) {
  if (!analyzer || !analyzer->analyzer || !out_seconds) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  *out_seconds = analyzer->analyzer->current_time();
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_sample_rate(SonareStreamAnalyzer* analyzer,
                                               int* out_sample_rate) {
  if (!analyzer || !analyzer->analyzer || !out_sample_rate) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  *out_sample_rate = analyzer->analyzer->config().sample_rate;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_set_expected_duration(SonareStreamAnalyzer* analyzer,
                                                         float duration_seconds) {
  if (!analyzer || !analyzer->analyzer) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  analyzer->analyzer->set_expected_duration(duration_seconds);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_set_normalization_gain(SonareStreamAnalyzer* analyzer,
                                                          float gain) {
  if (!analyzer || !analyzer->analyzer) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  analyzer->analyzer->set_normalization_gain(gain);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_set_tuning_ref_hz(SonareStreamAnalyzer* analyzer, float ref_hz) {
  if (!analyzer || !analyzer->analyzer) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  analyzer->analyzer->set_tuning_ref_hz(ref_hz);
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_free_stream_frames(SonareStreamFrames* frames) {
  if (!frames) return;
  delete[] frames->timestamps;
  delete[] frames->mel;
  delete[] frames->chroma;
  delete[] frames->onset_strength;
  delete[] frames->rms_energy;
  delete[] frames->spectral_centroid;
  delete[] frames->spectral_flatness;
  delete[] frames->chord_root;
  delete[] frames->chord_quality;
  delete[] frames->chord_confidence;
  *frames = {};
}

void sonare_free_stream_frames_u8(SonareStreamFramesU8* frames) {
  if (!frames) return;
  delete[] frames->timestamps;
  delete[] frames->mel;
  delete[] frames->chroma;
  delete[] frames->onset_strength;
  delete[] frames->rms_energy;
  delete[] frames->spectral_centroid;
  delete[] frames->spectral_flatness;
  *frames = {};
}

void sonare_free_stream_frames_i16(SonareStreamFramesI16* frames) {
  if (!frames) return;
  delete[] frames->timestamps;
  delete[] frames->mel;
  delete[] frames->chroma;
  delete[] frames->onset_strength;
  delete[] frames->rms_energy;
  delete[] frames->spectral_centroid;
  delete[] frames->spectral_flatness;
  *frames = {};
}
