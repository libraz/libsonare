#include "c_api/core_internal.h"

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
  return copy_vector(beats, out_times, out_count);
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
  return copy_vector(downbeats, out_times, out_count);
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
  return copy_vector(onsets, out_times, out_count);
  SONARE_C_CATCH
}

SonareError sonare_audio_analyze(const SonareAudio* audio, SonareAnalysisResult* out) {
  if (audio == nullptr || out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  // Zero the whole struct up front so a rejected input never leaves an
  // inconsistent (null beat_times, garbage beat_count) pair (matches
  // sonare_analyze_melody).
  *out = {};

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
