#include "c_api/features_internal.h"

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

SonareError sonare_mel_spectrogram_ex(const float* samples, size_t length, int sample_rate,
                                      int n_fft, int hop_length, int n_mels, float fmin, float fmax,
                                      int htk, SonareMelResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;

  out->power = nullptr;
  out->db = nullptr;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    MelConfig config;
    config.n_fft = n_fft;
    config.hop_length = hop_length;
    config.n_mels = n_mels;
    // 0.0 keeps the librosa default (fmin 0, fmax sr/2); a positive value overrides.
    if (fmin > 0.0f) config.fmin = fmin;
    if (fmax > 0.0f) config.fmax = fmax;
    config.htk = htk != 0;
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

SonareError sonare_mel_spectrogram(const float* samples, size_t length, int sample_rate, int n_fft,
                                   int hop_length, int n_mels, SonareMelResult* out) {
  // Default Mel range (fmin 0, fmax sr/2, Slaney) — see sonare_mel_spectrogram_ex
  // for the custom-range forward transform that round-trips with the inverse API.
  return sonare_mel_spectrogram_ex(samples, length, sample_rate, n_fft, hop_length, n_mels, 0.0f,
                                   0.0f, 0, out);
}

SonareError sonare_mfcc_ex(const float* samples, size_t length, int sample_rate, int n_fft,
                           int hop_length, int n_mels, int n_mfcc, float fmin, float fmax, int htk,
                           SonareMfccResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;

  out->coefficients = nullptr;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    MelConfig config;
    config.n_fft = n_fft;
    config.hop_length = hop_length;
    config.n_mels = n_mels;
    if (fmin > 0.0f) config.fmin = fmin;
    if (fmax > 0.0f) config.fmax = fmax;
    config.htk = htk != 0;
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

SonareError sonare_mfcc(const float* samples, size_t length, int sample_rate, int n_fft,
                        int hop_length, int n_mels, int n_mfcc, SonareMfccResult* out) {
  return sonare_mfcc_ex(samples, length, sample_rate, n_fft, hop_length, n_mels, n_mfcc, 0.0f, 0.0f,
                        0, out);
}
