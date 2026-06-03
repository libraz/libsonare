#include "c_api/features_internal.h"

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
    // Allocate before publishing out/out_count so a throwing allocation leaves
    // both at their cleared values, and an empty result yields (NULL, 0) like
    // the other feature wrappers rather than a non-NULL zero-length buffer.
    if (!result.empty()) {
      auto* buffer = new float[result.size()];
      std::memcpy(buffer, result.data(), result.size() * sizeof(float));
      *out = buffer;
    }
    *out_count = result.size();
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
    // Allocate before publishing out/out_count so a throwing allocation leaves
    // both at their cleared values, and an empty result yields (NULL, 0) like
    // the other feature wrappers rather than a non-NULL zero-length buffer.
    if (!result.empty()) {
      auto* buffer = new float[result.size()];
      std::memcpy(buffer, result.data(), result.size() * sizeof(float));
      *out = buffer;
    }
    *out_count = result.size();
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
    // Allocate before publishing out/out_count so a throwing allocation leaves
    // both at their cleared values, and an empty result yields (NULL, 0) like
    // the other feature wrappers rather than a non-NULL zero-length buffer.
    if (!result.empty()) {
      auto* buffer = new float[result.size()];
      std::memcpy(buffer, result.data(), result.size() * sizeof(float));
      *out = buffer;
    }
    *out_count = result.size();
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
    // Allocate before publishing out/out_count so a throwing allocation leaves
    // both at their cleared values, and an empty result yields (NULL, 0) like
    // the other feature wrappers rather than a non-NULL zero-length buffer.
    if (!result.empty()) {
      auto* buffer = new float[result.size()];
      std::memcpy(buffer, result.data(), result.size() * sizeof(float));
      *out = buffer;
    }
    *out_count = result.size();
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
    // Allocate before publishing out/out_count so a throwing allocation leaves
    // both at their cleared values, and an empty result yields (NULL, 0) like
    // the other feature wrappers rather than a non-NULL zero-length buffer.
    if (!result.empty()) {
      auto* buffer = new float[result.size()];
      std::memcpy(buffer, result.data(), result.size() * sizeof(float));
      *out = buffer;
    }
    *out_count = result.size();
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
  // Bound the projected output size (length * target_sr / src_sr) against
  // kMaxBufferSize, mirroring the input-side ceiling in validate_audio_params.
  // Compute in double so the multiplication cannot overflow before comparison
  // (src_sr is already validated >= kMinSampleRate, so the division is safe).
  const double projected =
      static_cast<double>(length) * static_cast<double>(target_sr) / static_cast<double>(src_sr);
  if (projected > static_cast<double>(kMaxBufferSize)) {
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
