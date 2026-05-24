#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/audio.h"
#include "core/convert.h"
#include "core/resample.h"
#include "core/spectrum.h"
#include "feature/chroma.h"
#include "feature/mel_spectrogram.h"
#include "feature/nnls_chroma.h"
#include "feature/onset.h"
#include "feature/pitch.h"
#include "feature/spectral.h"
#include "sonare_c.h"
#include "sonare_c_internal.h"

using namespace sonare;
using namespace sonare_c_detail;

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

  *out_db = nullptr;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
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
  SONARE_C_CATCH
}

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
  std::unique_ptr<float[]> coeffs(new float[mfcc_data.size()]);
  std::memcpy(coeffs.get(), mfcc_data.data(), mfcc_data.size() * sizeof(float));
  out->coefficients = release_array(coeffs);
  return SONARE_OK;
  SONARE_C_CATCH
}

// Features - Onset
// ============================================================================

SonareError sonare_onset_strength(const float* samples, size_t length, int sr, int n_fft,
                                  int hop_length, int n_mels, float** out, size_t* out_length) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sr);
  if (err != SONARE_OK) return err;

  *out = nullptr;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sr);
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
  SONARE_C_CATCH
}

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
  SONARE_C_CATCH
}

SonareError sonare_nnls_chroma(const float* samples, size_t length, int sr, float** out,
                               size_t* out_length, int* out_n_frames) {
  if (!out || !out_length || !out_n_frames) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sr);
  if (err != SONARE_OK) return err;

  *out = nullptr;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sr);
  Chroma chroma = nnls_chroma(audio);

  *out_n_frames = chroma.n_frames();
  size_t total = static_cast<size_t>(chroma.n_chroma()) * chroma.n_frames();
  *out_length = total;
  if (total == 0) return SONARE_OK;
  std::unique_ptr<float[]> features(new float[total]);
  std::memcpy(features.get(), chroma.data(), total * sizeof(float));
  *out = release_array(features);
  return SONARE_OK;
  SONARE_C_CATCH
}

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

  *out = nullptr;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  std::vector<float> result = rms_energy(audio, frame_length, hop_length);
  *out_count = result.size();
  std::unique_ptr<float[]> tmp(new float[result.size()]);
  std::memcpy(tmp.get(), result.data(), result.size() * sizeof(float));
  *out = release_array(tmp);
  return SONARE_OK;
  SONARE_C_CATCH
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
                             int hop_length, float fmin, float fmax, float threshold,
                             SonarePitchResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  out->f0 = nullptr;
  out->voiced_prob = nullptr;
  out->voiced_flag = nullptr;

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

  out->f0 = nullptr;
  out->voiced_prob = nullptr;
  out->voiced_flag = nullptr;

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
