/// @file sonare_c.cpp
/// @brief Implementation of C API.

#include "sonare_c.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "analysis/beat_analyzer.h"
#include "analysis/bpm_analyzer.h"
#include "analysis/chord_analyzer.h"
#include "analysis/dynamics_analyzer.h"
#include "analysis/key_analyzer.h"
#include "analysis/music_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "analysis/rhythm_analyzer.h"
#include "analysis/timbre_analyzer.h"
#include "core/audio.h"
#include "core/convert.h"
#include "core/resample.h"
#include "core/spectrum.h"
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/pitch_shift.h"
#include "effects/time_stretch.h"
#include "effects/tts.h"
#include "feature/chroma.h"
#include "feature/mel_spectrogram.h"
#include "feature/pitch.h"
#include "feature/spectral.h"
#include "quick.h"
#include "sonare.h"
#include "sonare_c_internal.h"

using namespace sonare;
using namespace sonare_c_detail;

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

SonareError sonare_audio_detect_bpm(const SonareAudio* audio, float* out_bpm) {
  if (audio == nullptr || out_bpm == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  *out_bpm =
      quick::detect_bpm(audio->audio.data(), audio->audio.size(), audio->audio.sample_rate());
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_detect_key(const SonareAudio* audio, SonareKey* out_key) {
  if (audio == nullptr || out_key == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  Key key = quick::detect_key(audio->audio.data(), audio->audio.size(), audio->audio.sample_rate());
  out_key->root = static_cast<SonarePitchClass>(key.root);
  out_key->mode = static_cast<SonareMode>(key.mode);
  out_key->confidence = key.confidence;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_detect_beats(const SonareAudio* audio, float** out_times,
                                      size_t* out_count) {
  if (audio == nullptr || out_times == nullptr || out_count == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  SONARE_C_TRY
  std::vector<float> beats =
      quick::detect_beats(audio->audio.data(), audio->audio.size(), audio->audio.sample_rate());
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

SonareError sonare_audio_detect_onsets(const SonareAudio* audio, float** out_times,
                                       size_t* out_count) {
  if (audio == nullptr || out_times == nullptr || out_count == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  SONARE_C_TRY
  std::vector<float> onsets =
      quick::detect_onsets(audio->audio.data(), audio->audio.size(), audio->audio.sample_rate());
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

SonareError sonare_audio_analyze(const SonareAudio* audio, SonareAnalysisResult* out) {
  if (audio == nullptr || out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  out->beat_times = nullptr;

  SONARE_C_TRY
  AnalysisResult result =
      quick::analyze(audio->audio.data(), audio->audio.size(), audio->audio.sample_rate());

  out->bpm = result.bpm;
  out->bpm_confidence = result.bpm_confidence;
  out->key.root = static_cast<SonarePitchClass>(result.key.root);
  out->key.mode = static_cast<SonareMode>(result.key.mode);
  out->key.confidence = result.key.confidence;
  out->time_signature.numerator = result.time_signature.numerator;
  out->time_signature.denominator = result.time_signature.denominator;
  out->time_signature.confidence = result.time_signature.confidence;
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

SonareError sonare_analyze_bpm(const float* samples, size_t length, int sample_rate, float bpm_min,
                               float bpm_max, float start_bpm, int n_fft, int hop_length,
                               int max_candidates, SonareBpmAnalysisResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (bpm_min <= 0.0f || bpm_max <= bpm_min || n_fft <= 0 || hop_length <= 0 ||
      max_candidates < 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  out->candidates = nullptr;
  out->candidate_count = 0;
  out->autocorrelation = nullptr;
  out->autocorrelation_count = 0;
  out->tempogram = nullptr;
  out->tempogram_count = 0;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  BpmConfig config;
  config.bpm_min = bpm_min;
  config.bpm_max = bpm_max;
  config.start_bpm = start_bpm;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  BpmAnalyzer analyzer(audio, config);
  out->bpm = analyzer.bpm();
  out->confidence = analyzer.confidence();

  std::vector<BpmCandidate> candidates = analyzer.candidates(max_candidates);
  out->candidate_count = candidates.size();
  if (!candidates.empty()) {
    std::unique_ptr<SonareBpmCandidate[]> cands(new SonareBpmCandidate[candidates.size()]);
    for (size_t i = 0; i < candidates.size(); ++i) {
      cands[i].bpm = candidates[i].bpm;
      cands[i].confidence = candidates[i].confidence;
    }
    out->candidates = release_array(cands);
  }

  const std::vector<float>& autocorr = analyzer.autocorrelation();
  out->autocorrelation_count = autocorr.size();
  if (!autocorr.empty()) {
    std::unique_ptr<float[]> data(new float[autocorr.size()]);
    std::memcpy(data.get(), autocorr.data(), autocorr.size() * sizeof(float));
    out->autocorrelation = release_array(data);
  }

  const std::vector<float>& tempogram = analyzer.tempogram();
  out->tempogram_count = tempogram.size();
  if (!tempogram.empty()) {
    std::unique_ptr<float[]> data(new float[tempogram.size()]);
    std::memcpy(data.get(), tempogram.data(), tempogram.size() * sizeof(float));
    out->tempogram = release_array(data);
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_analyze_rhythm(const float* samples, size_t length, int sample_rate,
                                  float bpm_min, float bpm_max, float start_bpm, int n_fft,
                                  int hop_length, SonareRhythmResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (bpm_min <= 0.0f || bpm_max <= bpm_min || n_fft <= 0 || hop_length <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  out->beat_intervals = nullptr;
  out->beat_interval_count = 0;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  RhythmConfig config;
  config.bpm_min = bpm_min;
  config.bpm_max = bpm_max;
  config.start_bpm = start_bpm;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  RhythmAnalyzer analyzer(audio, config);
  RhythmFeatures features = analyzer.features();
  out->bpm = analyzer.bpm();
  out->time_signature.numerator = features.time_signature.numerator;
  out->time_signature.denominator = features.time_signature.denominator;
  out->time_signature.confidence = features.time_signature.confidence;
  out->groove_type = to_c_groove_type(features.groove_type);
  out->syncopation = features.syncopation;
  out->pattern_regularity = features.pattern_regularity;
  out->tempo_stability = features.tempo_stability;

  const std::vector<float>& intervals = analyzer.beat_intervals();
  out->beat_interval_count = intervals.size();
  if (!intervals.empty()) {
    std::unique_ptr<float[]> data(new float[intervals.size()]);
    std::memcpy(data.get(), intervals.data(), intervals.size() * sizeof(float));
    out->beat_intervals = release_array(data);
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_analyze_dynamics(const float* samples, size_t length, int sample_rate,
                                    float window_sec, int hop_length, float compression_threshold,
                                    SonareDynamicsResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (window_sec <= 0.0f || hop_length <= 0 || compression_threshold < 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  out->loudness_times = nullptr;
  out->loudness_rms_db = nullptr;
  out->loudness_count = 0;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  DynamicsConfig config;
  config.window_sec = window_sec;
  config.hop_length = hop_length;
  config.compression_threshold = compression_threshold;

  DynamicsAnalyzer analyzer(audio, config);
  const Dynamics& dynamics = analyzer.dynamics();
  out->dynamic_range_db = dynamics.dynamic_range_db;
  out->peak_db = dynamics.peak_db;
  out->rms_db = dynamics.rms_db;
  out->crest_factor = dynamics.crest_factor;
  out->loudness_range_db = dynamics.loudness_range_db;
  out->is_compressed = dynamics.is_compressed ? 1 : 0;

  const LoudnessCurve& curve = analyzer.loudness_curve();
  size_t count = std::min(curve.times.size(), curve.rms_db.size());
  out->loudness_count = count;
  if (count > 0) {
    std::unique_ptr<float[]> times(new float[count]);
    std::unique_ptr<float[]> rms_db(new float[count]);
    std::memcpy(times.get(), curve.times.data(), count * sizeof(float));
    std::memcpy(rms_db.get(), curve.rms_db.data(), count * sizeof(float));
    out->loudness_times = release_array(times);
    out->loudness_rms_db = release_array(rms_db);
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_analyze_timbre(const float* samples, size_t length, int sample_rate, int n_fft,
                                  int hop_length, int n_mels, int n_mfcc, float window_sec,
                                  SonareTimbreResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (n_fft <= 0 || hop_length <= 0 || n_mels <= 0 || n_mfcc <= 0 || window_sec <= 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  out->spectral_centroid = nullptr;
  out->spectral_centroid_count = 0;
  out->spectral_flatness = nullptr;
  out->spectral_flatness_count = 0;
  out->spectral_rolloff = nullptr;
  out->spectral_rolloff_count = 0;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  TimbreConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;
  config.n_mfcc = n_mfcc;
  config.window_sec = window_sec;

  TimbreAnalyzer analyzer(audio, config);
  const Timbre& timbre = analyzer.timbre();
  out->brightness = timbre.brightness;
  out->warmth = timbre.warmth;
  out->density = timbre.density;
  out->roughness = timbre.roughness;
  out->complexity = timbre.complexity;

  const std::vector<float>& centroid = analyzer.spectral_centroid();
  out->spectral_centroid_count = centroid.size();
  if (!centroid.empty()) {
    std::unique_ptr<float[]> data(new float[centroid.size()]);
    std::memcpy(data.get(), centroid.data(), centroid.size() * sizeof(float));
    out->spectral_centroid = release_array(data);
  }

  const std::vector<float>& flatness = analyzer.spectral_flatness();
  out->spectral_flatness_count = flatness.size();
  if (!flatness.empty()) {
    std::unique_ptr<float[]> data(new float[flatness.size()]);
    std::memcpy(data.get(), flatness.data(), flatness.size() * sizeof(float));
    out->spectral_flatness = release_array(data);
  }

  const std::vector<float>& rolloff = analyzer.spectral_rolloff();
  out->spectral_rolloff_count = rolloff.size();
  if (!rolloff.empty()) {
    std::unique_ptr<float[]> data(new float[rolloff.size()]);
    std::memcpy(data.get(), rolloff.data(), rolloff.size() * sizeof(float));
    out->spectral_rolloff = release_array(data);
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_detect_chords(const float* samples, size_t length, int sample_rate,
                                 float min_duration, float smoothing_window, float threshold,
                                 int use_triads_only, int n_fft, int hop_length, int use_beat_sync,
                                 SonareChordAnalysisResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (min_duration < 0.0f || smoothing_window <= 0.0f || threshold < 0.0f || n_fft <= 0 ||
      hop_length <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  out->chords = nullptr;
  out->chord_count = 0;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  ChordConfig config;
  config.min_duration = min_duration;
  config.smoothing_window = smoothing_window;
  config.threshold = threshold;
  config.use_triads_only = use_triads_only != 0;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.use_beat_sync = use_beat_sync != 0;

  std::vector<Chord> chords = detect_chords(audio, config);
  out->chord_count = chords.size();
  if (!chords.empty()) {
    std::unique_ptr<SonareChord[]> data(new SonareChord[chords.size()]);
    for (size_t i = 0; i < chords.size(); ++i) {
      data[i].root = static_cast<SonarePitchClass>(chords[i].root);
      data[i].quality = to_c_chord_quality(chords[i].quality);
      data[i].start = chords[i].start;
      data[i].end = chords[i].end;
      data[i].confidence = chords[i].confidence;
    }
    out->chords = release_array(data);
  }
  return SONARE_OK;
  SONARE_C_CATCH
}
