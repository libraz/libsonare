#include <cmath>

#include "analysis/analysis_json.h"
#include "analysis/onset_analyzer.h"
#include "c_api/core_internal.h"

// Quick detection functions

SonareError sonare_detect_bpm(const float* samples, size_t length, int sample_rate,
                              float* out_bpm) {
  SONARE_C_API_ENTRY;
  if (out_bpm == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    *out_bpm = quick::detect_bpm(audio.data(), audio.size(), audio.sample_rate());
    return SONARE_OK;
  });
}

SonareError sonare_detect_key(const float* samples, size_t length, int sample_rate,
                              SonareKey* out_key) {
  SONARE_C_API_ENTRY;
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
  SONARE_C_API_ENTRY;
  return sonare_detect_key_with_options_and_modes(samples, length, sample_rate, n_fft, hop_length,
                                                  use_hpss, loudness_weighted, high_pass_hz,
                                                  nullptr, 0, out_key);
}

SonareError sonare_detect_key_with_options_and_modes(const float* samples, size_t length,
                                                     int sample_rate, int n_fft, int hop_length,
                                                     int use_hpss, int loudness_weighted,
                                                     float high_pass_hz, const SonareMode* modes,
                                                     size_t mode_count, SonareKey* out_key) {
  SONARE_C_API_ENTRY;
  return sonare_detect_key_with_extended_options(
      samples, length, sample_rate, n_fft, hop_length, use_hpss, loudness_weighted, high_pass_hz,
      modes, mode_count, SONARE_KEY_PROFILE_KRUMHANSL_SCHMUCKLER, nullptr, out_key);
}

SonareError sonare_detect_key_with_extended_options(
    const float* samples, size_t length, int sample_rate, int n_fft, int hop_length, int use_hpss,
    int loudness_weighted, float high_pass_hz, const SonareMode* modes, size_t mode_count,
    SonareKeyProfileType profile_type, const char* genre_hint, SonareKey* out_key) {
  SONARE_C_API_ENTRY;
  if (out_key == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_fft <= 0 || hop_length <= 0 || (use_hpss != 0 && hop_length < 16) || high_pass_hz < 0.0f) {
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
  SONARE_C_API_ENTRY;
  return sonare_detect_key_candidates_with_modes(samples, length, sample_rate, n_fft, hop_length,
                                                 use_hpss, loudness_weighted, high_pass_hz, nullptr,
                                                 0, out_candidates, out_count);
}

SonareError sonare_detect_key_candidates_with_modes(
    const float* samples, size_t length, int sample_rate, int n_fft, int hop_length, int use_hpss,
    int loudness_weighted, float high_pass_hz, const SonareMode* modes, size_t mode_count,
    SonareKeyCandidate** out_candidates, size_t* out_count) {
  SONARE_C_API_ENTRY;
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
  SONARE_C_API_ENTRY;
  if (out_candidates == nullptr || out_count == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out_candidates = nullptr;
  *out_count = 0;
  if (n_fft <= 0 || hop_length <= 0 || (use_hpss != 0 && hop_length < 16) || high_pass_hz < 0.0f) {
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
  SONARE_C_API_ENTRY;
  if (out_times == nullptr || out_count == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    std::vector<float> beats = quick::detect_beats(audio.data(), audio.size(), audio.sample_rate());
    return copy_vector(beats, out_times, out_count);
  });
}

SonareError sonare_detect_downbeats(const float* samples, size_t length, int sample_rate,
                                    float** out_times, size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (out_times == nullptr || out_count == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    std::vector<float> downbeats =
        quick::detect_downbeats(audio.data(), audio.size(), audio.sample_rate());
    return copy_vector(downbeats, out_times, out_count);
  });
}

SonareError sonare_detect_onsets(const float* samples, size_t length, int sample_rate,
                                 float** out_times, size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (out_times == nullptr || out_count == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    std::vector<float> onsets =
        quick::detect_onsets(audio.data(), audio.size(), audio.sample_rate());
    return copy_vector(onsets, out_times, out_count);
  });
}

SonareError sonare_detect_onsets_ex(const float* samples, size_t length, int sample_rate,
                                    const SonareOnsetDetectConfig* config, float** out_times,
                                    size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (config == nullptr || out_times == nullptr || out_count == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (config->n_fft <= 0 || config->hop_length <= 0 || config->pre_max < 0 ||
      config->post_max < 0 || config->pre_avg < 0 || config->post_avg < 0 || config->wait < 0 ||
      config->backtrack_range < 0 || config->backtrack > 1 || !std::isfinite(config->threshold) ||
      !std::isfinite(config->delta) || config->delta < 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  sonare::OnsetDetectConfig native;
  native.n_fft = config->n_fft;
  native.hop_length = config->hop_length;
  native.threshold = config->threshold;
  native.pre_max = config->pre_max;
  native.post_max = config->post_max;
  native.pre_avg = config->pre_avg;
  native.post_avg = config->post_avg;
  native.delta = config->delta;
  native.wait = config->wait;
  native.backtrack = config->backtrack != 0;
  native.backtrack_range = config->backtrack_range;
  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    return copy_vector(sonare::detect_onsets(audio, native), out_times, out_count);
  });
}

// Full quick analysis. Although SonareAnalysisResult is a flat C struct, it
// still exposes meter/beat data, so this intentionally pays the full quick
// analysis cost. Use sonare_detect_bpm/key/beats for cheaper single-purpose
// queries.

SonareError sonare_analyze(const float* samples, size_t length, int sample_rate,
                           SonareAnalysisResult* out) {
  SONARE_C_API_ENTRY;
  if (out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  // Zero the whole struct up front so a rejected input (e.g. validate_audio_params
  // failure inside run_offline) never leaves an inconsistent (null beat_times,
  // garbage beat_count) pair (matches sonare_analyze_melody).
  *out = {};

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    AnalysisResult result = quick::analyze(audio.data(), audio.size(), audio.sample_rate());

    return fill_analysis_result(result, out);
  });
}

// Full analysis serialized to a camelCase JSON object (chords, sections,
// timbre, dynamics, rhythm, melody, form) — the rich counterpart to
// sonare_analyze, which only fills the flat bpm/key/beats struct. The schema is
// the single source of truth in analysis_result_to_json and is mirrored by the
// WASM native object. *out_json is heap-allocated; free with sonare_free_string.
SonareMusicAnalyzeOptions sonare_music_analyze_options_default(void) {
  const MusicAnalyzerConfig config;
  return {config.n_fft,
          config.hop_length,
          config.bpm_min,
          config.bpm_max,
          config.start_bpm,
          config.use_triads_only ? 1 : 0,
          config.use_hpss ? 1 : 0,
          config.chroma_highpass_hz,
          config.use_bass_weighted ? 1 : 0,
          config.chroma_hop_multiplier,
          config.use_chord_hmm ? 1 : 0,
          config.use_chord_key_context ? 1 : 0,
          config.chord_hmm_beam_width,
          config.detect_chord_inversions ? 1 : 0};
}

SonareError sonare_analyze_json_ex(const float* samples, size_t length, int sample_rate,
                                   const SonareMusicAnalyzeOptions* options, char** out_json) {
  SONARE_C_API_ENTRY;
  // Field rules live in validate_config, which MusicAnalyzer's constructor
  // applies below; the exception maps back to SONARE_ERROR_INVALID_PARAMETER, so
  // this entry point only checks what the core cannot see (the out pointers).
  if (out_json == nullptr || options == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out_json = nullptr;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    MusicAnalyzerConfig config;
    config.n_fft = options->n_fft;
    config.hop_length = options->hop_length;
    config.bpm_min = options->bpm_min;
    config.bpm_max = options->bpm_max;
    config.start_bpm = options->start_bpm;
    config.use_triads_only = options->use_triads_only != 0;
    config.use_hpss = options->use_hpss != 0;
    config.chroma_highpass_hz = options->chroma_highpass_hz;
    config.use_bass_weighted = options->use_bass_weighted != 0;
    config.chroma_hop_multiplier = options->chroma_hop_multiplier;
    config.use_chord_hmm = options->use_chord_hmm != 0;
    config.use_chord_key_context = options->use_chord_key_context != 0;
    config.chord_hmm_beam_width = options->chord_hmm_beam_width;
    config.detect_chord_inversions = options->detect_chord_inversions != 0;
    MusicAnalyzer analyzer(audio, config);
    AnalysisResult result = analyzer.analyze();
    *out_json = copy_string(analysis_result_to_json(result));
    return SONARE_OK;
  });
}

SonareError sonare_analyze_json(const float* samples, size_t length, int sample_rate,
                                char** out_json) {
  SONARE_C_API_ENTRY;
  const SonareMusicAnalyzeOptions options = sonare_music_analyze_options_default();
  return sonare_analyze_json_ex(samples, length, sample_rate, &options, out_json);
}

// Same as sonare_analyze_json but reports per-stage progress through @p callback
// (progress in [0,1] plus a stage label). A null callback runs silently. The
// callback is invoked on the calling thread before the function returns.
SonareError sonare_analyze_json_with_progress(const float* samples, size_t length, int sample_rate,
                                              SonareAnalyzeProgressCallback callback,
                                              void* user_data, char** out_json) {
  return sonare_analyze_json_with_progress_ex(samples, length, sample_rate, callback, user_data,
                                              out_json, nullptr, nullptr);
}

SonareError sonare_analyze_json_with_progress_ex(
    const float* samples, size_t length, int sample_rate, SonareAnalyzeProgressCallback callback,
    void* user_data, char** out_json, SonareCancelCallback cancel_cb, void* cancel_user_data) {
  SONARE_C_API_ENTRY;
  if (out_json == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out_json = nullptr;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    MusicAnalyzer analyzer(audio);
    if (callback != nullptr) {
      analyzer.set_progress_callback([callback, user_data](float progress, const char* stage) {
        callback(progress, stage, user_data);
      });
    }
    if (cancel_cb != nullptr) {
      analyzer.set_cancel_callback(
          [cancel_cb, cancel_user_data]() { return cancel_cb(cancel_user_data) != 0; });
      auto result = analyzer.analyze_cancellable();
      if (!result) return SONARE_ERROR_CANCELLED;
      *out_json = copy_string(analysis_result_to_json(*result));
    } else {
      AnalysisResult result = analyzer.analyze();
      *out_json = copy_string(analysis_result_to_json(result));
    }
    return SONARE_OK;
  });
}
