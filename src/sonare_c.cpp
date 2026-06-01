/// @file sonare_c.cpp
/// @brief Implementation of C API.

#include "sonare_c.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

#include "analysis/acoustic_analyzer.h"
#include "analysis/beat_analyzer.h"
#include "analysis/bpm_analyzer.h"
#include "analysis/chord_analyzer.h"
#include "analysis/dynamics_analyzer.h"
#include "analysis/key_analyzer.h"
#include "analysis/melody_analyzer.h"
#include "analysis/music_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "analysis/rhythm_analyzer.h"
#include "analysis/section_analyzer.h"
#include "analysis/timbre_analyzer.h"
#include "core/audio.h"
#include "core/convert.h"
#include "core/spectrum.h"
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/pitch_shift.h"
#include "effects/time_stretch.h"
#include "feature/chroma.h"
#include "feature/cqt.h"
#include "feature/mel_spectrogram.h"
#include "feature/pitch.h"
#include "feature/spectral.h"
#include "feature/vqt.h"
#include "quick.h"
#include "sonare.h"
#include "sonare_c_internal.h"

using namespace sonare;
using namespace sonare_c_detail;

namespace {

float* copy_float_vector_or_nan(const std::vector<float>& values, size_t count) {
  if (count == 0) {
    return nullptr;
  }
  float* out = new float[count];
  for (size_t i = 0; i < count; ++i) {
    out[i] = i < values.size() ? values[i] : std::numeric_limits<float>::quiet_NaN();
  }
  return out;
}

void fill_acoustic_result(const AcousticParameters& params, SonareAcousticResult* out) {
  out->rt60 = params.rt60;
  out->edt = params.edt;
  out->c50 = params.c50;
  out->c80 = params.c80;
  out->d50 = params.d50;
  out->band_count = params.rt60_bands.size();
  out->rt60_bands = copy_float_vector_or_nan(params.rt60_bands, out->band_count);
  out->edt_bands = copy_float_vector_or_nan(params.edt_bands, out->band_count);
  // Clarity bands are not computed in blind mode; expose null (rather than a
  // NaN-filled array) so callers can distinguish "not computed" from "invalid".
  out->c50_bands = params.c50_bands.empty()
                       ? nullptr
                       : copy_float_vector_or_nan(params.c50_bands, out->band_count);
  out->c80_bands = params.c80_bands.empty()
                       ? nullptr
                       : copy_float_vector_or_nan(params.c80_bands, out->band_count);
  out->confidence = params.confidence;
  out->is_blind = params.is_blind ? 1 : 0;
}

PitchClass from_c_pitch_class(SonarePitchClass pitch) {
  const int value = static_cast<int>(pitch);
  if (value < static_cast<int>(PitchClass::C) || value > static_cast<int>(PitchClass::B)) {
    return PitchClass::C;
  }
  return static_cast<PitchClass>(value);
}

Mode from_c_mode(SonareMode mode) {
  const int value = static_cast<int>(mode);
  if (value < static_cast<int>(Mode::Major) || value > static_cast<int>(Mode::Locrian)) {
    return Mode::Major;
  }
  return static_cast<Mode>(value);
}

bool fill_key_profile(SonareKeyProfileType profile_type, KeyConfig* config) {
  if (config == nullptr) {
    return false;
  }
  switch (profile_type) {
    case SONARE_KEY_PROFILE_KRUMHANSL_SCHMUCKLER:
      config->profile_type = KeyProfileType::KrumhanslSchmuckler;
      return true;
    case SONARE_KEY_PROFILE_TEMPERLEY:
      config->profile_type = KeyProfileType::Temperley;
      return true;
    case SONARE_KEY_PROFILE_SHAATH:
      config->profile_type = KeyProfileType::Shaath;
      return true;
    case SONARE_KEY_PROFILE_FARALDO_EDMT:
      config->profile_type = KeyProfileType::FaraldoEDMT;
      return true;
    case SONARE_KEY_PROFILE_FARALDO_EDMA:
      config->profile_type = KeyProfileType::FaraldoEDMA;
      return true;
    case SONARE_KEY_PROFILE_FARALDO_EDMM:
      config->profile_type = KeyProfileType::FaraldoEDMM;
      return true;
    case SONARE_KEY_PROFILE_BELLMAN_BUDGE:
      config->profile_type = KeyProfileType::BellmanBudge;
      return true;
    default:
      return false;
  }
}

bool fill_key_modes(const SonareMode* modes, size_t mode_count, KeyConfig* config) {
  if (mode_count == 0) {
    return true;
  }
  if (modes == nullptr || config == nullptr) {
    return false;
  }
  config->modes.clear();
  config->modes.reserve(mode_count);
  for (size_t i = 0; i < mode_count; ++i) {
    const int value = static_cast<int>(modes[i]);
    if (value < static_cast<int>(Mode::Major) || value > static_cast<int>(Mode::Locrian)) {
      return false;
    }
    config->modes.push_back(static_cast<Mode>(value));
  }
  return true;
}

void fill_chord_result(const std::vector<Chord>& chords, SonareChordAnalysisResult* out) {
  out->chord_count = chords.size();
  if (chords.empty()) {
    return;
  }

  std::unique_ptr<SonareChord[]> data(new SonareChord[chords.size()]);
  for (size_t i = 0; i < chords.size(); ++i) {
    data[i].root = static_cast<SonarePitchClass>(chords[i].root);
    data[i].quality = to_c_chord_quality(chords[i].quality);
    data[i].start = chords[i].start;
    data[i].end = chords[i].end;
    data[i].confidence = chords[i].confidence;
    data[i].bass = static_cast<SonarePitchClass>(chords[i].bass);
  }
  out->chords = release_array(data);
}

}  // namespace

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

SonareError sonare_audio_detect_downbeats(const SonareAudio* audio, float** out_times,
                                          size_t* out_count) {
  if (audio == nullptr || out_times == nullptr || out_count == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  SONARE_C_TRY
  std::vector<float> downbeats =
      quick::detect_downbeats(audio->audio.data(), audio->audio.size(), audio->audio.sample_rate());
  *out_count = downbeats.size();
  if (downbeats.empty()) {
    *out_times = nullptr;
  } else {
    *out_times = new float[downbeats.size()];
    std::memcpy(*out_times, downbeats.data(), downbeats.size() * sizeof(float));
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

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    *out_bpm = quick::detect_bpm(audio.data(), audio.size(), audio.sample_rate());
    return SONARE_OK;
  });
}

SonareError sonare_detect_key(const float* samples, size_t length, int sample_rate,
                              SonareKey* out_key) {
  if (out_key == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    Key key = quick::detect_key(audio.data(), audio.size(), audio.sample_rate());
    out_key->root = static_cast<SonarePitchClass>(key.root);
    out_key->mode = static_cast<SonareMode>(key.mode);
    out_key->confidence = key.confidence;
    return SONARE_OK;
  });
}

SonareError sonare_detect_key_with_options(const float* samples, size_t length, int sample_rate,
                                           int n_fft, int hop_length, int use_hpss,
                                           int loudness_weighted, float high_pass_hz,
                                           SonareKey* out_key) {
  return sonare_detect_key_with_options_and_modes(samples, length, sample_rate, n_fft, hop_length,
                                                  use_hpss, loudness_weighted, high_pass_hz,
                                                  nullptr, 0, out_key);
}

SonareError sonare_detect_key_with_options_and_modes(const float* samples, size_t length,
                                                     int sample_rate, int n_fft, int hop_length,
                                                     int use_hpss, int loudness_weighted,
                                                     float high_pass_hz, const SonareMode* modes,
                                                     size_t mode_count, SonareKey* out_key) {
  return sonare_detect_key_with_extended_options(
      samples, length, sample_rate, n_fft, hop_length, use_hpss, loudness_weighted, high_pass_hz,
      modes, mode_count, SONARE_KEY_PROFILE_KRUMHANSL_SCHMUCKLER, nullptr, out_key);
}

SonareError sonare_detect_key_with_extended_options(
    const float* samples, size_t length, int sample_rate, int n_fft, int hop_length, int use_hpss,
    int loudness_weighted, float high_pass_hz, const SonareMode* modes, size_t mode_count,
    SonareKeyProfileType profile_type, const char* genre_hint, SonareKey* out_key) {
  if (out_key == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_fft <= 0 || hop_length <= 0 || high_pass_hz < 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    KeyConfig config;
    config.n_fft = n_fft;
    config.hop_length = hop_length;
    config.use_hpss = use_hpss != 0;
    config.loudness_weighted = loudness_weighted != 0;
    config.high_pass_hz = high_pass_hz;
    if (!fill_key_modes(modes, mode_count, &config)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (!fill_key_profile(profile_type, &config)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (genre_hint != nullptr && genre_hint[0] != '\0') {
      config.genre_hint = genre_hint;
    }
    Key key = quick::detect_key(audio.data(), audio.size(), audio.sample_rate(), config);
    out_key->root = static_cast<SonarePitchClass>(key.root);
    out_key->mode = static_cast<SonareMode>(key.mode);
    out_key->confidence = key.confidence;
    return SONARE_OK;
  });
}

SonareError sonare_detect_key_candidates(const float* samples, size_t length, int sample_rate,
                                         int n_fft, int hop_length, int use_hpss,
                                         int loudness_weighted, float high_pass_hz,
                                         SonareKeyCandidate** out_candidates, size_t* out_count) {
  return sonare_detect_key_candidates_with_modes(samples, length, sample_rate, n_fft, hop_length,
                                                 use_hpss, loudness_weighted, high_pass_hz, nullptr,
                                                 0, out_candidates, out_count);
}

SonareError sonare_detect_key_candidates_with_modes(
    const float* samples, size_t length, int sample_rate, int n_fft, int hop_length, int use_hpss,
    int loudness_weighted, float high_pass_hz, const SonareMode* modes, size_t mode_count,
    SonareKeyCandidate** out_candidates, size_t* out_count) {
  return sonare_detect_key_candidates_with_extended_options(
      samples, length, sample_rate, n_fft, hop_length, use_hpss, loudness_weighted, high_pass_hz,
      modes, mode_count, SONARE_KEY_PROFILE_KRUMHANSL_SCHMUCKLER, nullptr, out_candidates,
      out_count);
}

SonareError sonare_detect_key_candidates_with_extended_options(
    const float* samples, size_t length, int sample_rate, int n_fft, int hop_length, int use_hpss,
    int loudness_weighted, float high_pass_hz, const SonareMode* modes, size_t mode_count,
    SonareKeyProfileType profile_type, const char* genre_hint, SonareKeyCandidate** out_candidates,
    size_t* out_count) {
  if (out_candidates == nullptr || out_count == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out_candidates = nullptr;
  *out_count = 0;
  if (n_fft <= 0 || hop_length <= 0 || high_pass_hz < 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    KeyConfig config;
    config.n_fft = n_fft;
    config.hop_length = hop_length;
    config.use_hpss = use_hpss != 0;
    config.loudness_weighted = loudness_weighted != 0;
    config.high_pass_hz = high_pass_hz;
    if (!fill_key_modes(modes, mode_count, &config)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (!fill_key_profile(profile_type, &config)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (genre_hint != nullptr && genre_hint[0] != '\0') {
      config.genre_hint = genre_hint;
    }

    const auto candidates =
        quick::detect_key_candidates(audio.data(), audio.size(), audio.sample_rate(), config);
    *out_count = candidates.size();
    if (candidates.empty()) {
      return SONARE_OK;
    }

    auto* out = new SonareKeyCandidate[candidates.size()];
    for (size_t i = 0; i < candidates.size(); ++i) {
      out[i].key.root = static_cast<SonarePitchClass>(candidates[i].key.root);
      out[i].key.mode = static_cast<SonareMode>(candidates[i].key.mode);
      out[i].key.confidence = candidates[i].key.confidence;
      out[i].correlation = candidates[i].correlation;
    }
    *out_candidates = out;
    return SONARE_OK;
  });
}

SonareError sonare_detect_beats(const float* samples, size_t length, int sample_rate,
                                float** out_times, size_t* out_count) {
  if (out_times == nullptr || out_count == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    std::vector<float> beats = quick::detect_beats(audio.data(), audio.size(), audio.sample_rate());
    *out_count = beats.size();
    if (beats.empty()) {
      *out_times = nullptr;
    } else {
      *out_times = new float[beats.size()];
      std::memcpy(*out_times, beats.data(), beats.size() * sizeof(float));
    }
    return SONARE_OK;
  });
}

SonareError sonare_detect_downbeats(const float* samples, size_t length, int sample_rate,
                                    float** out_times, size_t* out_count) {
  if (out_times == nullptr || out_count == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    std::vector<float> downbeats =
        quick::detect_downbeats(audio.data(), audio.size(), audio.sample_rate());
    *out_count = downbeats.size();
    if (downbeats.empty()) {
      *out_times = nullptr;
    } else {
      *out_times = new float[downbeats.size()];
      std::memcpy(*out_times, downbeats.data(), downbeats.size() * sizeof(float));
    }
    return SONARE_OK;
  });
}

SonareError sonare_detect_onsets(const float* samples, size_t length, int sample_rate,
                                 float** out_times, size_t* out_count) {
  if (out_times == nullptr || out_count == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    std::vector<float> onsets =
        quick::detect_onsets(audio.data(), audio.size(), audio.sample_rate());
    *out_count = onsets.size();
    if (onsets.empty()) {
      *out_times = nullptr;
    } else {
      *out_times = new float[onsets.size()];
      std::memcpy(*out_times, onsets.data(), onsets.size() * sizeof(float));
    }
    return SONARE_OK;
  });
}

// Full analysis

SonareError sonare_analyze(const float* samples, size_t length, int sample_rate,
                           SonareAnalysisResult* out) {
  if (out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  out->beat_times = nullptr;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    AnalysisResult result = quick::analyze(audio.data(), audio.size(), audio.sample_rate());

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
  });
}

SonareError sonare_analyze_bpm(const float* samples, size_t length, int sample_rate, float bpm_min,
                               float bpm_max, float start_bpm, int n_fft, int hop_length,
                               int max_candidates, SonareBpmAnalysisResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
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

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
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
  });
}

SonareError sonare_analyze_impulse_response(const float* samples, size_t length, int sample_rate,
                                            int n_octave_bands, SonareAcousticResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_octave_bands < 0) return SONARE_ERROR_INVALID_PARAMETER;

  out->rt60_bands = nullptr;
  out->edt_bands = nullptr;
  out->c50_bands = nullptr;
  out->c80_bands = nullptr;
  out->band_count = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    AcousticConfig config;
    config.n_octave_bands = n_octave_bands;
    fill_acoustic_result(sonare::analyze_impulse_response(audio, config), out);
    return SONARE_OK;
  });
}

SonareError sonare_detect_acoustic(const float* samples, size_t length, int sample_rate,
                                   int n_octave_bands, int n_third_octave_subbands,
                                   float min_decay_db, float noise_floor_margin_db,
                                   SonareAcousticResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_octave_bands < 0 || n_third_octave_subbands < 0 || min_decay_db <= 0.0f ||
      noise_floor_margin_db < 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  out->rt60_bands = nullptr;
  out->edt_bands = nullptr;
  out->c50_bands = nullptr;
  out->c80_bands = nullptr;
  out->band_count = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    AcousticConfig config;
    config.mode = AcousticConfig::Mode::Blind;
    config.n_octave_bands = n_octave_bands;
    config.n_third_octave_subbands = n_third_octave_subbands;
    config.min_decay_db = min_decay_db;
    config.noise_floor_margin_db = noise_floor_margin_db;
    fill_acoustic_result(sonare::detect_acoustic(audio, config), out);
    return SONARE_OK;
  });
}

SonareError sonare_analyze_rhythm(const float* samples, size_t length, int sample_rate,
                                  float bpm_min, float bpm_max, float start_bpm, int n_fft,
                                  int hop_length, SonareRhythmResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (bpm_min <= 0.0f || bpm_max <= bpm_min || n_fft <= 0 || hop_length <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  out->beat_intervals = nullptr;
  out->beat_interval_count = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
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
  });
}

SonareError sonare_analyze_dynamics(const float* samples, size_t length, int sample_rate,
                                    float window_sec, int hop_length, float compression_threshold,
                                    SonareDynamicsResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (window_sec <= 0.0f || hop_length <= 0 || compression_threshold < 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  out->loudness_times = nullptr;
  out->loudness_rms_db = nullptr;
  out->loudness_count = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
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
  });
}

SonareError sonare_analyze_timbre(const float* samples, size_t length, int sample_rate, int n_fft,
                                  int hop_length, int n_mels, int n_mfcc, float window_sec,
                                  SonareTimbreResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_fft <= 0 || hop_length <= 0 || n_mels <= 0 || n_mfcc <= 0 || window_sec <= 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  out->spectral_centroid = nullptr;
  out->spectral_centroid_count = 0;
  out->spectral_flatness = nullptr;
  out->spectral_flatness_count = 0;
  out->spectral_rolloff = nullptr;
  out->spectral_rolloff_count = 0;
  out->timbre_over_time = nullptr;
  out->timbre_over_time_count = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
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

    const std::vector<Timbre>& over_time = analyzer.timbre_over_time();
    out->timbre_over_time_count = over_time.size();
    if (!over_time.empty()) {
      std::unique_ptr<SonareTimbreFrame[]> frames(new SonareTimbreFrame[over_time.size()]);
      for (size_t i = 0; i < over_time.size(); ++i) {
        frames[i].brightness = over_time[i].brightness;
        frames[i].warmth = over_time[i].warmth;
        frames[i].density = over_time[i].density;
        frames[i].roughness = over_time[i].roughness;
        frames[i].complexity = over_time[i].complexity;
      }
      out->timbre_over_time = release_array(frames);
    }
    return SONARE_OK;
  });
}

SonareError sonare_detect_chords(const float* samples, size_t length, int sample_rate,
                                 float min_duration, float smoothing_window, float threshold,
                                 int use_triads_only, int n_fft, int hop_length, int use_beat_sync,
                                 SonareChordAnalysisResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (min_duration < 0.0f || smoothing_window <= 0.0f || threshold < 0.0f || n_fft <= 0 ||
      hop_length <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  out->chords = nullptr;
  out->chord_count = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    ChordConfig config;
    config.min_duration = min_duration;
    config.smoothing_window = smoothing_window;
    config.threshold = threshold;
    config.use_triads_only = use_triads_only != 0;
    config.n_fft = n_fft;
    config.hop_length = hop_length;
    config.use_beat_sync = use_beat_sync != 0;

    std::vector<Chord> chords = detect_chords(audio, config);
    fill_chord_result(chords, out);
    return SONARE_OK;
  });
}

SonareError sonare_detect_chords_ex(const float* samples, size_t length, int sample_rate,
                                    const SonareChordDetectionOptions* options,
                                    SonareChordAnalysisResult* out) {
  if (!out || !options) return SONARE_ERROR_INVALID_PARAMETER;
  if (options->min_duration < 0.0f || options->smoothing_window <= 0.0f ||
      options->threshold < 0.0f || options->n_fft <= 0 || options->hop_length <= 0 ||
      options->hmm_beam_width < 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  out->chords = nullptr;
  out->chord_count = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    ChordConfig config;
    config.min_duration = options->min_duration;
    config.smoothing_window = options->smoothing_window;
    config.threshold = options->threshold;
    config.use_triads_only = options->use_triads_only != 0;
    config.n_fft = options->n_fft;
    config.hop_length = options->hop_length;
    config.use_beat_sync = options->use_beat_sync != 0;
    config.use_hmm = options->use_hmm != 0;
    config.hmm_beam_width = options->hmm_beam_width;
    config.use_key_context = options->use_key_context != 0;
    config.key_root = from_c_pitch_class(options->key_root);
    config.key_mode = from_c_mode(options->key_mode);
    config.detect_inversions = options->detect_inversions != 0;
    config.chroma_method = options->chroma_method == 1 ? ChromaMethod::NNLS : ChromaMethod::STFT;

    std::vector<Chord> chords = detect_chords(audio, config);
    fill_chord_result(chords, out);
    return SONARE_OK;
  });
}

SonareError sonare_analyze_sections(const float* samples, size_t length, int sample_rate, int n_fft,
                                    int hop_length, float min_section_sec,
                                    SonareSectionResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_fft <= 0 || hop_length <= 0 || min_section_sec < 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  out->sections = nullptr;
  out->section_count = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    SectionConfig config;
    config.n_fft = n_fft;
    config.hop_length = hop_length;
    config.min_section_sec = min_section_sec;
    SectionAnalyzer analyzer(audio, config);
    const std::vector<Section>& sections = analyzer.sections();
    if (!sections.empty()) {
      auto data = std::make_unique<SonareSection[]>(sections.size());
      for (size_t i = 0; i < sections.size(); ++i) {
        data[i].type = static_cast<SonareSectionType>(sections[i].type);
        data[i].start = sections[i].start;
        data[i].end = sections[i].end;
        data[i].energy_level = sections[i].energy_level;
        data[i].confidence = sections[i].confidence;
      }
      out->sections = release_array(data);
      out->section_count = sections.size();
    }
    return SONARE_OK;
  });
}

SonareError sonare_analyze_melody(const float* samples, size_t length, int sample_rate, float fmin,
                                  float fmax, int frame_length, int hop_length, float threshold,
                                  SonareMelodyResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (fmin <= 0.0f || fmax <= fmin || frame_length <= 0 || hop_length <= 0 || threshold <= 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  *out = {};

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    MelodyConfig config;
    config.fmin = fmin;
    config.fmax = fmax;
    config.frame_length = frame_length;
    config.hop_length = hop_length;
    config.threshold = threshold;
    MelodyAnalyzer analyzer(audio, config);
    const MelodyContour& contour = analyzer.contour();
    out->pitch_range_octaves = contour.pitch_range_octaves;
    out->pitch_stability = contour.pitch_stability;
    out->mean_frequency = contour.mean_frequency;
    out->vibrato_rate = contour.vibrato_rate;
    if (!contour.pitches.empty()) {
      auto data = std::make_unique<SonareMelodyPoint[]>(contour.pitches.size());
      for (size_t i = 0; i < contour.pitches.size(); ++i) {
        data[i].time = contour.pitches[i].time;
        data[i].frequency = contour.pitches[i].frequency;
        data[i].confidence = contour.pitches[i].confidence;
      }
      out->points = release_array(data);
      out->point_count = contour.pitches.size();
    }
    return SONARE_OK;
  });
}

namespace {

SonareError fill_cqt_result(const CqtResult& result, SonareCqtResult* out) {
  *out = {};
  out->n_bins = result.n_bins();
  out->n_frames = result.n_frames();
  out->hop_length = result.hop_length();
  out->sample_rate = result.sample_rate();
  const std::vector<float>& magnitude = result.magnitude();
  if (!magnitude.empty()) {
    auto mag = std::make_unique<float[]>(magnitude.size());
    std::copy(magnitude.begin(), magnitude.end(), mag.get());
    out->magnitude = mag.release();
  }
  const std::vector<float>& freqs = result.frequencies();
  if (!freqs.empty()) {
    auto fr = std::make_unique<float[]>(freqs.size());
    std::copy(freqs.begin(), freqs.end(), fr.get());
    out->frequencies = fr.release();
  }
  return SONARE_OK;
}

}  // namespace

SonareError sonare_cqt(const float* samples, size_t length, int sample_rate, int hop_length,
                       float fmin, int n_bins, int bins_per_octave, SonareCqtResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (hop_length <= 0 || fmin <= 0.0f || n_bins <= 0 || bins_per_octave <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  *out = {};

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    CqtConfig config;
    config.hop_length = hop_length;
    config.fmin = fmin;
    config.n_bins = n_bins;
    config.bins_per_octave = bins_per_octave;
    CqtResult result = cqt(audio, config);
    return fill_cqt_result(result, out);
  });
}

SonareError sonare_vqt(const float* samples, size_t length, int sample_rate, int hop_length,
                       float fmin, int n_bins, int bins_per_octave, float gamma,
                       SonareCqtResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (hop_length <= 0 || fmin <= 0.0f || n_bins <= 0 || bins_per_octave <= 0 || gamma < 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  *out = {};

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    VqtConfig config;
    config.hop_length = hop_length;
    config.fmin = fmin;
    config.n_bins = n_bins;
    config.bins_per_octave = bins_per_octave;
    config.gamma = gamma;
    VqtResult result = vqt(audio, config);
    return fill_cqt_result(result, out);
  });
}
