#include <array>

#include "c_api/core_internal.h"
#include "core/audio_io.h"
#include "mixing/downmix.h"

SonareError sonare_audio_from_buffer(const float* data, size_t length, int sample_rate,
                                     SonareAudio** out) {
  SONARE_C_API_ENTRY;
  if (out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  SonareError err = validate_audio_params(data, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  *out = new SonareAudio{Audio::from_buffer(data, length, sample_rate)};
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_from_memory(const uint8_t* data, size_t length, SonareAudio** out) {
  SONARE_C_API_ENTRY;
  if (out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  if (data == nullptr || length == 0) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  *out = new SonareAudio{Audio::from_memory(data, length)};
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_decode_channels(const uint8_t* data, size_t length, float** out_samples,
                                   size_t* out_frames, int* out_channels, int* out_sample_rate) {
  SONARE_C_API_ENTRY;
  if (out_samples != nullptr) *out_samples = nullptr;
  if (out_frames != nullptr) *out_frames = 0;
  if (out_channels != nullptr) *out_channels = 0;
  if (out_sample_rate != nullptr) *out_sample_rate = 0;
  if (data == nullptr || length == 0 || out_samples == nullptr || out_frames == nullptr ||
      out_channels == nullptr || out_sample_rate == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  SONARE_C_TRY
  const std::vector<Audio> planes = Audio::from_memory_channels(data, length);
  const size_t frames = planes.front().size();
  std::unique_ptr<float[]> packed(new float[frames * planes.size()]);
  for (size_t channel = 0; channel < planes.size(); ++channel) {
    std::memcpy(packed.get() + channel * frames, planes[channel].data(), frames * sizeof(float));
  }
  *out_samples = packed.release();
  *out_frames = frames;
  *out_channels = static_cast<int>(planes.size());
  *out_sample_rate = planes.front().sample_rate();
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_downmix(const float* channels, size_t frames, int channel_count,
                           int target_layout, float** out_samples) {
  SONARE_C_API_ENTRY;
  if (out_samples == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out_samples = nullptr;
  if (channels == nullptr || frames == 0 || channel_count <= 0 || target_layout < 0 ||
      !is_valid_channel_layout(static_cast<uint8_t>(target_layout))) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const auto to = static_cast<ChannelLayout>(target_layout);
  const size_t in_channels = static_cast<size_t>(channel_count);
  const size_t out_channels = static_cast<size_t>(sonare::channel_count(to));
  // The same rule the decoders apply: counts outside the speaker model fold to
  // mono as an unweighted mean and have no other target.
  const bool modelled =
      channel_count == sonare::channel_count(layout_from_channel_count(channel_count));
  if (!modelled && to != ChannelLayout::Mono) return SONARE_ERROR_INVALID_PARAMETER;
  if (frames > std::numeric_limits<size_t>::max() / std::max(in_channels, out_channels)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  SONARE_C_TRY
  std::unique_ptr<float[]> out(new float[frames * out_channels]);
  if (!modelled) {
    for (size_t i = 0; i < frames; ++i) {
      double sum = 0.0;
      for (size_t channel = 0; channel < in_channels; ++channel) {
        sum += channels[channel * frames + i];
      }
      out[i] = static_cast<float>(sum / static_cast<double>(in_channels));
    }
  } else {
    std::array<const float*, 8> in_planes{};
    std::array<float*, 8> out_planes{};
    for (size_t channel = 0; channel < in_channels; ++channel) {
      in_planes[channel] = channels + channel * frames;
    }
    for (size_t channel = 0; channel < out_channels; ++channel) {
      out_planes[channel] = out.get() + channel * frames;
    }
    mixing::downmix(layout_from_channel_count(channel_count), to, in_planes.data(),
                    out_planes.data(), frames);
  }
  *out_samples = out.release();
  return SONARE_OK;
  SONARE_C_CATCH
}

#ifndef __EMSCRIPTEN__
SonareError sonare_audio_from_file(const char* path, SonareAudio** out) {
  SONARE_C_API_ENTRY;
  if (out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  if (path == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  *out = new SonareAudio{Audio::from_file(path)};
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_from_file_channel(const char* path, int channel_index, SonareAudio** out) {
  SONARE_C_API_ENTRY;
  if (out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  if (path == nullptr || channel_index < 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  SONARE_C_TRY
  *out = new SonareAudio{Audio::from_file_channel(path, channel_index)};
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_file_channel_count(const char* path, int* out_channels) {
  SONARE_C_API_ENTRY;
  if (out_channels != nullptr) *out_channels = 0;
  if (path == nullptr || out_channels == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  SONARE_C_TRY
  const int channels = sonare::audio_channel_count(path);
  if (channels <= 0) return SONARE_ERROR_INVALID_FORMAT;
  *out_channels = channels;
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
  SONARE_C_API_ENTRY;
  if (audio == nullptr || out_bpm == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  *out_bpm =
      quick::detect_bpm(audio->audio.data(), audio->audio.size(), audio->audio.sample_rate());
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_detect_key(const SonareAudio* audio, SonareKey* out_key) {
  SONARE_C_API_ENTRY;
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
  SONARE_C_API_ENTRY;
  if (out_times == nullptr || out_count == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out_times = nullptr;
  *out_count = 0;
  if (audio == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  std::vector<float> beats =
      quick::detect_beats(audio->audio.data(), audio->audio.size(), audio->audio.sample_rate());
  return copy_vector(beats, out_times, out_count);
  SONARE_C_CATCH
}

SonareError sonare_audio_detect_downbeats(const SonareAudio* audio, float** out_times,
                                          size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (out_times == nullptr || out_count == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out_times = nullptr;
  *out_count = 0;
  if (audio == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  std::vector<float> downbeats =
      quick::detect_downbeats(audio->audio.data(), audio->audio.size(), audio->audio.sample_rate());
  return copy_vector(downbeats, out_times, out_count);
  SONARE_C_CATCH
}

SonareError sonare_audio_detect_onsets(const SonareAudio* audio, float** out_times,
                                       size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (out_times == nullptr || out_count == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out_times = nullptr;
  *out_count = 0;
  if (audio == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  std::vector<float> onsets =
      quick::detect_onsets(audio->audio.data(), audio->audio.size(), audio->audio.sample_rate());
  return copy_vector(onsets, out_times, out_count);
  SONARE_C_CATCH
}

SonareError sonare_audio_analyze(const SonareAudio* audio, SonareAnalysisResult* out) {
  SONARE_C_API_ENTRY;
  if (out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  // Zero the whole struct up front so a rejected input never leaves an
  // inconsistent (null beat_times, garbage beat_count) pair (matches
  // sonare_analyze_melody).
  *out = {};
  if (audio == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  AnalysisResult result =
      quick::analyze(audio->audio.data(), audio->audio.size(), audio->audio.sample_rate());

  return fill_analysis_result(result, out);
  SONARE_C_CATCH
}
