#include "c_api/features_internal.h"

namespace {

// Validates that an inverse-transform input buffer holds exactly
// rows * n_frames floats. Returns SONARE_OK on a match, otherwise
// SONARE_ERROR_INVALID_PARAMETER. Guards the product against size_t overflow
// (32-bit WASM) before comparing. @p rows and @p n_frames are assumed already
// validated as > 0 by the caller (each *_checked validates them first).
SonareError check_inverse_input_length(size_t input_length, int rows, int n_frames) {
  const size_t r = static_cast<size_t>(rows);
  const size_t f = static_cast<size_t>(n_frames);
  if (f != 0 && r > sonare_c_detail::kMaxBufferSize / f) return SONARE_ERROR_INVALID_PARAMETER;
  if (input_length != r * f) return SONARE_ERROR_INVALID_PARAMETER;
  return SONARE_OK;
}

bool all_finite(const float* input, int rows, int n_frames) noexcept {
  const auto total = static_cast<size_t>(rows) * static_cast<size_t>(n_frames);
  for (size_t index = 0; index < total; ++index) {
    if (!std::isfinite(input[index])) return false;
  }
  return true;
}

}  // namespace

SonareError sonare_mel_to_stft_ex(const float* mel, int n_mels, int n_frames, int sample_rate,
                                  int n_fft, float fmin, float fmax, int htk,
                                  SonareInverseResult* out) {
  if (!out || !mel) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_mels <= 0 || n_frames <= 0 || n_fft <= 0 || sample_rate <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  *out = {};
  if (!all_finite(mel, n_mels, n_frames)) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  MelConfig config;
  config.n_mels = n_mels;
  config.n_fft = n_fft;
  config.fmin = fmin;
  config.fmax = fmax;
  config.htk = htk != 0;
  std::vector<float> stft = mel_to_stft(mel, n_mels, n_frames, config, sample_rate);
  return fill_inverse_result(stft, n_fft / 2 + 1, n_frames, out);
  SONARE_C_CATCH
}

SonareError sonare_mel_to_stft(const float* mel, int n_mels, int n_frames, int sample_rate,
                               int n_fft, float fmin, float fmax, SonareInverseResult* out) {
  return sonare_mel_to_stft_ex(mel, n_mels, n_frames, sample_rate, n_fft, fmin, fmax, 0, out);
}

SonareError sonare_mel_to_audio_ex(const float* mel, int n_mels, int n_frames, int sample_rate,
                                   int n_fft, int hop_length, float fmin, float fmax, int htk,
                                   int n_iter, float** out, size_t* out_length) {
  if (!out || !out_length || !mel) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_mels <= 0 || n_frames <= 0 || n_fft <= 0 || hop_length <= 0 || sample_rate <= 0 ||
      n_iter <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  *out = nullptr;
  *out_length = 0;
  if (!all_finite(mel, n_mels, n_frames)) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  MelConfig config;
  config.n_mels = n_mels;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.fmax = fmax;
  config.htk = htk != 0;
  Audio audio = mel_to_audio(mel, n_mels, n_frames, config, n_iter, sample_rate);
  return fill_audio_samples(audio, out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_mel_to_audio(const float* mel, int n_mels, int n_frames, int sample_rate,
                                int n_fft, int hop_length, float fmin, float fmax, int n_iter,
                                float** out, size_t* out_length) {
  return sonare_mel_to_audio_ex(mel, n_mels, n_frames, sample_rate, n_fft, hop_length, fmin, fmax,
                                0, n_iter, out, out_length);
}

SonareError sonare_mfcc_to_mel(const float* mfcc, int n_mfcc, int n_frames, int n_mels,
                               SonareInverseResult* out) {
  if (!out || !mfcc) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_mfcc <= 0 || n_frames <= 0 || n_mels <= 0) return SONARE_ERROR_INVALID_PARAMETER;

  *out = {};
  if (!all_finite(mfcc, n_mfcc, n_frames)) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  std::vector<float> mel = mfcc_to_mel(mfcc, n_mfcc, n_frames, n_mels);
  return fill_inverse_result(mel, n_mels, n_frames, out);
  SONARE_C_CATCH
}

SonareError sonare_mfcc_to_audio_ex(const float* mfcc, int n_mfcc, int n_frames, int n_mels,
                                    int sample_rate, int n_fft, int hop_length, float fmin,
                                    float fmax, int htk, int n_iter, float** out,
                                    size_t* out_length) {
  if (!out || !out_length || !mfcc) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_mfcc <= 0 || n_frames <= 0 || n_mels <= 0 || n_fft <= 0 || hop_length <= 0 ||
      sample_rate <= 0 || n_iter <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  *out = nullptr;
  *out_length = 0;
  if (!all_finite(mfcc, n_mfcc, n_frames)) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  MelConfig config;
  config.n_mels = n_mels;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.fmax = fmax;
  config.htk = htk != 0;
  Audio audio = mfcc_to_audio(mfcc, n_mfcc, n_frames, config, n_iter, sample_rate);
  return fill_audio_samples(audio, out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_mfcc_to_audio(const float* mfcc, int n_mfcc, int n_frames, int n_mels,
                                 int sample_rate, int n_fft, int hop_length, float fmin, float fmax,
                                 int n_iter, float** out, size_t* out_length) {
  return sonare_mfcc_to_audio_ex(mfcc, n_mfcc, n_frames, n_mels, sample_rate, n_fft, hop_length,
                                 fmin, fmax, 0, n_iter, out, out_length);
}

SonareError sonare_mel_to_stft_checked(const float* mel, size_t input_length, int n_mels,
                                       int n_frames, int sample_rate, int n_fft, float fmin,
                                       float fmax, SonareInverseResult* out) {
  if (!out || !mel) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_mels <= 0 || n_frames <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (check_inverse_input_length(input_length, n_mels, n_frames) != SONARE_OK) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return sonare_mel_to_stft(mel, n_mels, n_frames, sample_rate, n_fft, fmin, fmax, out);
}

SonareError sonare_mel_to_stft_checked_ex(const float* mel, size_t input_length, int n_mels,
                                          int n_frames, int sample_rate, int n_fft, float fmin,
                                          float fmax, int htk, SonareInverseResult* out) {
  if (!out || !mel) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_mels <= 0 || n_frames <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (check_inverse_input_length(input_length, n_mels, n_frames) != SONARE_OK) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return sonare_mel_to_stft_ex(mel, n_mels, n_frames, sample_rate, n_fft, fmin, fmax, htk, out);
}

SonareError sonare_mel_to_audio_checked(const float* mel, size_t input_length, int n_mels,
                                        int n_frames, int sample_rate, int n_fft, int hop_length,
                                        float fmin, float fmax, int n_iter, float** out,
                                        size_t* out_length) {
  if (!out || !out_length || !mel) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_mels <= 0 || n_frames <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (check_inverse_input_length(input_length, n_mels, n_frames) != SONARE_OK) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return sonare_mel_to_audio(mel, n_mels, n_frames, sample_rate, n_fft, hop_length, fmin, fmax,
                             n_iter, out, out_length);
}

SonareError sonare_mel_to_audio_checked_ex(const float* mel, size_t input_length, int n_mels,
                                           int n_frames, int sample_rate, int n_fft, int hop_length,
                                           float fmin, float fmax, int htk, int n_iter, float** out,
                                           size_t* out_length) {
  if (!out || !out_length || !mel) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_mels <= 0 || n_frames <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (check_inverse_input_length(input_length, n_mels, n_frames) != SONARE_OK) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return sonare_mel_to_audio_ex(mel, n_mels, n_frames, sample_rate, n_fft, hop_length, fmin, fmax,
                                htk, n_iter, out, out_length);
}

SonareError sonare_mfcc_to_mel_checked(const float* mfcc, size_t input_length, int n_mfcc,
                                       int n_frames, int n_mels, SonareInverseResult* out) {
  if (!out || !mfcc) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_mfcc <= 0 || n_frames <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (check_inverse_input_length(input_length, n_mfcc, n_frames) != SONARE_OK) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return sonare_mfcc_to_mel(mfcc, n_mfcc, n_frames, n_mels, out);
}

SonareError sonare_mfcc_to_audio_checked(const float* mfcc, size_t input_length, int n_mfcc,
                                         int n_frames, int n_mels, int sample_rate, int n_fft,
                                         int hop_length, float fmin, float fmax, int n_iter,
                                         float** out, size_t* out_length) {
  if (!out || !out_length || !mfcc) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_mfcc <= 0 || n_frames <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (check_inverse_input_length(input_length, n_mfcc, n_frames) != SONARE_OK) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return sonare_mfcc_to_audio(mfcc, n_mfcc, n_frames, n_mels, sample_rate, n_fft, hop_length, fmin,
                              fmax, n_iter, out, out_length);
}

SonareError sonare_mfcc_to_audio_checked_ex(const float* mfcc, size_t input_length, int n_mfcc,
                                            int n_frames, int n_mels, int sample_rate, int n_fft,
                                            int hop_length, float fmin, float fmax, int htk,
                                            int n_iter, float** out, size_t* out_length) {
  if (!out || !out_length || !mfcc) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_mfcc <= 0 || n_frames <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (check_inverse_input_length(input_length, n_mfcc, n_frames) != SONARE_OK) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return sonare_mfcc_to_audio_ex(mfcc, n_mfcc, n_frames, n_mels, sample_rate, n_fft, hop_length,
                                 fmin, fmax, htk, n_iter, out, out_length);
}

void sonare_free_inverse_result(SonareInverseResult* result) {
  if (!result) return;
  delete[] result->data;
  result->data = nullptr;
  result->rows = 0;
  result->n_frames = 0;
}
