#include "analysis/analysis_json.h"
#include "c_api/core_internal.h"

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

// Full quick analysis. Although SonareAnalysisResult is a flat C struct, it
// still exposes meter/beat data, so this intentionally pays the full quick
// analysis cost. Use sonare_detect_bpm/key/beats for cheaper single-purpose
// queries.

SonareError sonare_analyze(const float* samples, size_t length, int sample_rate,
                           SonareAnalysisResult* out) {
  if (out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  // Zero the whole struct up front so a rejected input (e.g. validate_audio_params
  // failure inside run_offline) never leaves an inconsistent (null beat_times,
  // garbage beat_count) pair (matches sonare_analyze_melody).
  *out = {};

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

// Full analysis serialized to a camelCase JSON object (chords, sections,
// timbre, dynamics, rhythm, melody, form) — the rich counterpart to
// sonare_analyze, which only fills the flat bpm/key/beats struct. The schema is
// the single source of truth in analysis_result_to_json and is mirrored by the
// WASM native object. *out_json is heap-allocated; free with sonare_free_string.
SonareError sonare_analyze_json(const float* samples, size_t length, int sample_rate,
                                char** out_json) {
  if (out_json == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out_json = nullptr;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    MusicAnalyzer analyzer(audio);
    AnalysisResult result = analyzer.analyze();
    *out_json = copy_string(analysis_result_to_json(result));
    return SONARE_OK;
  });
}

// Same as sonare_analyze_json but reports per-stage progress through @p callback
// (progress in [0,1] plus a stage label). A null callback runs silently. The
// callback is invoked on the calling thread before the function returns.
SonareError sonare_analyze_json_with_progress(const float* samples, size_t length, int sample_rate,
                                              SonareAnalyzeProgressCallback callback,
                                              void* user_data, char** out_json) {
  if (out_json == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out_json = nullptr;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    MusicAnalyzer analyzer(audio);
    if (callback != nullptr) {
      analyzer.set_progress_callback([callback, user_data](float progress, const char* stage) {
        callback(progress, stage, user_data);
      });
    }
    AnalysisResult result = analyzer.analyze();
    *out_json = copy_string(analysis_result_to_json(result));
    return SONARE_OK;
  });
}
