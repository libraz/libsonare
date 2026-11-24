#include <array>

#include "c_api/core_internal.h"
#include "c_api/feature_input_domains.h"
#include "core/audio_io.h"
#include "mixing/downmix.h"
#include "util/parameter_checks.h"

SonareError sonare_audio_from_buffer(const float* data, size_t length, int sample_rate,
                                     SonareAudio** out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SonareError err = validate_audio_params("data", data, length, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out);

  SONARE_C_TRY
  *out = new SonareAudio{Audio::from_buffer(data, length, sample_rate)};
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_from_memory(const uint8_t* data, size_t length, SonareAudio** out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(data, length);
  if (length == 0) {
    return run_validation([] { util::check::refuse("data", util::refusal::kNonEmptyPredicate); });
  }
  SONARE_C_REQUIRE_NONNULL(out);

  SONARE_C_TRY
  *out = new SonareAudio{Audio::from_memory(data, length)};
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_decode_channels(const uint8_t* data, size_t length, float** out_samples,
                                   size_t* out_frames, int* out_channels, int* out_sample_rate) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_samples, out_frames, out_channels, out_sample_rate);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(data, length);
  if (length == 0) {
    return run_validation([] { util::check::refuse("data", util::refusal::kNonEmptyPredicate); });
  }
  SONARE_C_REQUIRE_NONNULL(out_samples);
  SONARE_C_REQUIRE_NONNULL(out_frames);
  SONARE_C_REQUIRE_NONNULL(out_channels);
  SONARE_C_REQUIRE_NONNULL(out_sample_rate);

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

namespace {

SonareError downmix_planar(const float* channels, size_t frames, size_t num_channels,
                           int target_layout, const mixing::DownmixOptions& options,
                           float** out_samples) {
  SONARE_C_ZERO_OUTS(out_samples);
  SONARE_C_REQUIRE_NONNULL(channels);
  ChannelLayout to{};
  if (const SonareError err = run_validation([&] {
        util::check::integer_within(frames, sonare_c_detail::kDownmixFramesDomain, "frames");
        util::check::integer_within(num_channels, sonare_c_detail::kDownmixChannelCountDomain,
                                    "num_channels");
        util::check::integer_in_range(target_layout, 0, kMaxChannelLayoutValue, "target_layout");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_samples);
  to = static_cast<ChannelLayout>(target_layout);
  const int channel_count = static_cast<int>(num_channels);
  const size_t in_channels = num_channels;
  const size_t out_channels = static_cast<size_t>(sonare::channel_count(to));
  // The same rule the decoders apply: counts outside the speaker model fold to
  // mono as an unweighted mean and have no other target.
  const bool modelled =
      channel_count == sonare::channel_count(layout_from_channel_count(channel_count));
  if (!modelled && to != ChannelLayout::Mono) {
    return run_validation([&] {
      util::check::refuse_relation({"target_layout", "num_channels"},
                                   "{0} must be mono when {1} has no speaker layout");
    });
  }
  if (frames > std::numeric_limits<size_t>::max() / std::max(in_channels, out_channels)) {
    return run_validation([&] {
      util::check::refuse_relation({"frames", "num_channels", "target_layout"},
                                   "{0} * max({1}, the channel count of {2}) must fit in size_t");
    });
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
                    out_planes.data(), frames, options);
  }
  *out_samples = out.release();
  return SONARE_OK;
  SONARE_C_CATCH
}

}  // namespace

void sonare_downmix_options_init(SonareDownmixOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
}

SonareError sonare_downmix(const float* channels, size_t frames, size_t num_channels,
                           int target_layout, const SonareDownmixOptions* options,
                           float** out_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_samples);
  mixing::DownmixOptions fold;
  if (options != nullptr) {
    if (const SonareError err = sonare_c_detail::run_validation([&] {
          sonare_c_detail::require_struct_size(options->struct_size, sizeof(SonareDownmixOptions),
                                               "options", "sonare_downmix_options_init");
        });
        err != SONARE_OK) {
      return err;
    }
    fold.include_lfe = options->include_lfe != 0;
    fold.normalize = options->normalize != 0;
  }
  return downmix_planar(channels, frames, num_channels, target_layout, fold, out_samples);
}

#ifndef __EMSCRIPTEN__
void sonare_audio_file_options_init(SonareAudioFileOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
  options->channel_index = SONARE_AUDIO_CHANNEL_DOWNMIX;
}

namespace {
const std::array<CName, 1> kAudioFileOptionNames{{{"channel_index", "options.channel_index"}}};
}  // namespace

SonareError sonare_audio_from_file(const char* path, const SonareAudioFileOptions* options,
                                   SonareAudio** out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(path);
  SonareAudioFileOptions resolved;
  if (const SonareError err = sonare_c_detail::resolve_options(
          options, sonare_audio_file_options_init, "sonare_audio_file_options_init", &resolved);
      err != SONARE_OK) {
    return err;
  }
  if (resolved.channel_index != SONARE_AUDIO_CHANNEL_DOWNMIX) {
    if (const SonareError err = run_validation([&] {
          util::check::integer_within(resolved.channel_index,
                                      sonare_c_detail::kAudioChannelIndexDomain,
                                      "options.channel_index");
        });
        err != SONARE_OK) {
      return err;
    }
  }
  SONARE_C_REQUIRE_NONNULL(out);

  SONARE_C_TRY
  if (resolved.channel_index == SONARE_AUDIO_CHANNEL_DOWNMIX) {
    *out = new SonareAudio{Audio::from_file(path)};
  } else {
    *out = new SonareAudio{with_c_names(kAudioFileOptionNames, [&] {
      return Audio::from_file_channel(path, resolved.channel_index);
    })};
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_file_num_channels(const char* path, int* out_channels) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_channels);
  SONARE_C_REQUIRE_NONNULL(path);
  SONARE_C_REQUIRE_NONNULL(out_channels);

  SONARE_C_TRY
  const int channels = sonare::audio_channel_count(path);
  if (channels <= 0) {
    set_last_error_static(SONARE_ERROR_INVALID_FORMAT,
                          "audio file channel count could not be determined");
    return SONARE_ERROR_INVALID_FORMAT;
  }
  *out_channels = channels;
  return SONARE_OK;
  SONARE_C_CATCH
}
#endif

void sonare_audio_destroy(SonareAudio* audio) { delete audio; }

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

float sonare_audio_duration_sec(const SonareAudio* audio) {
  if (audio == nullptr) {
    return 0.0f;
  }
  return audio->audio.duration();
}

SonareError sonare_audio_detect_bpm(const SonareAudio* audio, float bpm_min, float bpm_max,
                                    float* out_bpm) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_bpm);
  SONARE_C_REQUIRE_NONNULL(audio);
  BpmConfig config;
  config.bpm_min = bpm_min;
  config.bpm_max = bpm_max;
  if (const SonareError err = run_validation([&] { validate_bpm_config(config); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_bpm);

  SONARE_C_TRY
  *out_bpm = quick::detect_bpm(audio->audio.data(), audio->audio.size(), audio->audio.sample_rate(),
                               config);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_detect_key(const SonareAudio* audio, SonareKey* out_key) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_key);
  SONARE_C_REQUIRE_NONNULL(audio);
  SONARE_C_REQUIRE_NONNULL(out_key);

  SONARE_C_TRY
  Key key = quick::detect_key(audio->audio.data(), audio->audio.size(), audio->audio.sample_rate());
  out_key->root = static_cast<SonarePitchClass>(key.root);
  out_key->mode = static_cast<SonareMode>(key.mode);
  out_key->confidence = key.confidence;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_detect_beats(const SonareAudio* audio, float bpm_min, float bpm_max,
                                      float** out_times, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_times, out_count);
  SONARE_C_REQUIRE_NONNULL(audio);
  BpmConfig tempo;
  tempo.bpm_min = bpm_min;
  tempo.bpm_max = bpm_max;
  if (const SonareError err = run_validation([&] { validate_bpm_config(tempo); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_times);
  SONARE_C_REQUIRE_NONNULL(out_count);
  BeatConfig config;
  config.bpm_min = bpm_min;
  config.bpm_max = bpm_max;

  SONARE_C_TRY
  std::vector<float> beats = quick::detect_beats(audio->audio.data(), audio->audio.size(),
                                                 audio->audio.sample_rate(), config);
  return copy_vector(beats, out_times, out_count);
  SONARE_C_CATCH
}

SonareError sonare_audio_detect_downbeats(const SonareAudio* audio, float** out_times,
                                          size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_times, out_count);
  SONARE_C_REQUIRE_NONNULL(audio);
  SONARE_C_REQUIRE_NONNULL(out_times);
  SONARE_C_REQUIRE_NONNULL(out_count);

  SONARE_C_TRY
  std::vector<float> downbeats =
      quick::detect_downbeats(audio->audio.data(), audio->audio.size(), audio->audio.sample_rate());
  return copy_vector(downbeats, out_times, out_count);
  SONARE_C_CATCH
}

SonareError sonare_audio_detect_onsets(const SonareAudio* audio, float** out_times,
                                       size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_times, out_count);
  SONARE_C_REQUIRE_NONNULL(audio);
  SONARE_C_REQUIRE_NONNULL(out_times);
  SONARE_C_REQUIRE_NONNULL(out_count);

  SONARE_C_TRY
  std::vector<float> onsets =
      quick::detect_onsets(audio->audio.data(), audio->audio.size(), audio->audio.sample_rate());
  return copy_vector(onsets, out_times, out_count);
  SONARE_C_CATCH
}
