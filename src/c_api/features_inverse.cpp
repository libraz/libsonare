#include "c_api/features_internal.h"

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
