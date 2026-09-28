#include <memory>
#include <vector>

#if defined(SONARE_WITH_PLAYBACK)
#include "playback/config.h"
#include "playback/hrtf_set.h"
#include "playback/loudness_meter.h"
#include "playback/renderer.h"
#endif
#include <sonare/sonare_c.h>

#include "sonare_c_internal.h"

using namespace sonare;
using namespace sonare_c_detail;

#if defined(SONARE_WITH_PLAYBACK)
struct SonareHrtfSet {
  std::unique_ptr<sonare::playback::HrtfSet> set;
};

struct SonarePlaybackRenderer {
  std::unique_ptr<sonare::playback::PlaybackRenderer> renderer;
};

struct SonarePlaybackLoudnessMeter {
  std::unique_ptr<sonare::playback::PlaybackLoudnessMeter> meter;
};
#endif

#ifndef __EMSCRIPTEN__
SonareError sonare_hrtf_set_create_default(SonareHrtfSet** out) {
  SONARE_C_API_ENTRY;
  if (out) *out = nullptr;
#if defined(SONARE_WITH_PLAYBACK)
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  auto handle = std::make_unique<SonareHrtfSet>();
  handle->set = std::make_unique<playback::HrtfSet>(playback::HrtfSet::builtin_default());
  *out = handle.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(out);
#endif
}
#endif

SonareError sonare_hrtf_set_create_from_memory(const uint8_t* data, size_t size,
                                               SonareHrtfSet** out) {
  SONARE_C_API_ENTRY;
  if (out) *out = nullptr;
#if defined(SONARE_WITH_PLAYBACK)
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  auto handle = std::make_unique<SonareHrtfSet>();
  handle->set = std::make_unique<playback::HrtfSet>(playback::HrtfSet::from_memory(data, size));
  *out = handle.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(data, size, out);
#endif
}

void sonare_hrtf_set_destroy(SonareHrtfSet* set) {
#if defined(SONARE_WITH_PLAYBACK)
  delete set;
#else
  (void)set;
#endif
}

SonareError sonare_playback_renderer_create_json(const char* config_json, const SonareHrtfSet* hrtf,
                                                 int sample_rate, int max_block_size,
                                                 SonarePlaybackRenderer** out) {
  SONARE_C_API_ENTRY;
  if (out) *out = nullptr;
#if defined(SONARE_WITH_PLAYBACK)
  if (!out || !config_json) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  const playback::RendererConfig config = playback::parse_renderer_config(config_json);
  const playback::HrtfSet* hrtf_ptr = hrtf ? hrtf->set.get() : nullptr;
  auto handle = std::make_unique<SonarePlaybackRenderer>();
  handle->renderer =
      std::make_unique<playback::PlaybackRenderer>(config, hrtf_ptr, sample_rate, max_block_size);
  *out = handle.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(config_json, hrtf, sample_rate, max_block_size, out);
#endif
}

void sonare_playback_renderer_destroy(SonarePlaybackRenderer* renderer) {
#if defined(SONARE_WITH_PLAYBACK)
  delete renderer;
#else
  (void)renderer;
#endif
}

SonareError sonare_playback_renderer_reset(SonarePlaybackRenderer* renderer) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PLAYBACK)
  if (!renderer) return SONARE_ERROR_INVALID_PARAMETER;
  renderer->renderer->reset();
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(renderer);
#endif
}

SonareError sonare_playback_renderer_set_config_json(SonarePlaybackRenderer* renderer,
                                                     const char* config_json) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PLAYBACK)
  if (!renderer || !config_json) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  const playback::RendererConfig config = playback::parse_renderer_config(config_json);
  renderer->renderer->set_config(config);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(renderer, config_json);
#endif
}

SonareError sonare_playback_renderer_config_json(const SonarePlaybackRenderer* renderer,
                                                 char** out_json) {
  SONARE_C_API_ENTRY;
  if (out_json) *out_json = nullptr;
#if defined(SONARE_WITH_PLAYBACK)
  if (!renderer || !out_json) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  *out_json = copy_string(playback::serialize_renderer_config(renderer->renderer->config()));
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(renderer, out_json);
#endif
}

SonareError sonare_playback_renderer_set_head_orientation(SonarePlaybackRenderer* renderer,
                                                          float yaw_deg, float pitch_deg,
                                                          float roll_deg) {
  // Realtime entry: never touches the thread-local error message.
  SONARE_C_RT_API_ENTRY;
#if defined(SONARE_WITH_PLAYBACK)
  if (!renderer) return SONARE_ERROR_INVALID_PARAMETER;
  renderer->renderer->set_head_orientation(yaw_deg, pitch_deg, roll_deg);
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(renderer, yaw_deg, pitch_deg, roll_deg);
#endif
}

SonareError sonare_playback_renderer_input_channel_count(const SonarePlaybackRenderer* renderer,
                                                         int* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = 0;
#if defined(SONARE_WITH_PLAYBACK)
  if (!renderer || !out) return SONARE_ERROR_INVALID_PARAMETER;
  *out = renderer->renderer->input_channel_count();
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(renderer, out);
#endif
}

SonareError sonare_playback_renderer_output_channel_count(const SonarePlaybackRenderer* renderer,
                                                          int* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = 0;
#if defined(SONARE_WITH_PLAYBACK)
  if (!renderer || !out) return SONARE_ERROR_INVALID_PARAMETER;
  *out = renderer->renderer->output_channel_count();
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(renderer, out);
#endif
}

SonareError sonare_playback_renderer_process_planar(SonarePlaybackRenderer* renderer,
                                                    const float* const* in, int in_channels,
                                                    float* const* out, int out_channels,
                                                    int frames) {
  // Realtime entry: never touches the thread-local error message.
  SONARE_C_RT_API_ENTRY;
#if defined(SONARE_WITH_PLAYBACK)
  if (!renderer) return SONARE_ERROR_INVALID_PARAMETER;
  const bool ok = renderer->renderer->process_planar(in, in_channels, out, out_channels, frames);
  return ok ? SONARE_OK : SONARE_ERROR_INVALID_PARAMETER;
#else
  SONARE_C_STUB_NOT_SUPPORTED(renderer, in, in_channels, out, out_channels, frames);
#endif
}

SonareError sonare_playback_renderer_process_interleaved(SonarePlaybackRenderer* renderer,
                                                         const float* in, int in_channels,
                                                         float* out, int out_channels, int frames) {
  // Realtime entry: never touches the thread-local error message.
  SONARE_C_RT_API_ENTRY;
#if defined(SONARE_WITH_PLAYBACK)
  if (!renderer) return SONARE_ERROR_INVALID_PARAMETER;
  const bool ok =
      renderer->renderer->process_interleaved(in, in_channels, out, out_channels, frames);
  return ok ? SONARE_OK : SONARE_ERROR_INVALID_PARAMETER;
#else
  SONARE_C_STUB_NOT_SUPPORTED(renderer, in, in_channels, out, out_channels, frames);
#endif
}

SonareError sonare_playback_renderer_latency_samples(const SonarePlaybackRenderer* renderer,
                                                     int* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = 0;
#if defined(SONARE_WITH_PLAYBACK)
  if (!renderer || !out) return SONARE_ERROR_INVALID_PARAMETER;
  *out = renderer->renderer->latency_samples();
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(renderer, out);
#endif
}

SonareError sonare_playback_renderer_diagnostics_json(const SonarePlaybackRenderer* renderer,
                                                      char** out_json) {
  SONARE_C_API_ENTRY;
  if (out_json) *out_json = nullptr;
#if defined(SONARE_WITH_PLAYBACK)
  if (!renderer || !out_json) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  *out_json = copy_string(playback::diagnostics_to_json(renderer->renderer->diagnostics()));
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(renderer, out_json);
#endif
}

SonareError sonare_playback_renderer_non_finite_discard_count(
    const SonarePlaybackRenderer* renderer, uint32_t* out_count) {
  SONARE_C_API_ENTRY;
  if (out_count) *out_count = 0;
#if defined(SONARE_WITH_PLAYBACK)
  if (!renderer || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = renderer->renderer->non_finite_discard_count();
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(renderer, out_count);
#endif
}

SonareError sonare_playback_render_interleaved(const float* in, size_t frames, int in_channels,
                                               int sample_rate, const char* config_json,
                                               const SonareHrtfSet* hrtf, float** out,
                                               size_t* out_frames, int* out_channels) {
  SONARE_C_API_ENTRY;
  if (out) *out = nullptr;
  if (out_frames) *out_frames = 0;
  if (out_channels) *out_channels = 0;
#if defined(SONARE_WITH_PLAYBACK)
  if (!out || !out_frames || !out_channels || !config_json) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  const playback::RendererConfig config = playback::parse_renderer_config(config_json);
  const playback::HrtfSet* hrtf_ptr = hrtf ? hrtf->set.get() : nullptr;
  int channels = 0;
  const std::vector<float> result = playback::render_interleaved(
      in, frames, in_channels, sample_rate, config, hrtf_ptr, &channels);
  *out_channels = channels;
  *out_frames = frames;
  *out = copy_vector(result);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(in, frames, in_channels, sample_rate, config_json, hrtf, out,
                              out_frames, out_channels);
#endif
}

void sonare_free_playback_render(float* samples) { delete[] samples; }

SonareError sonare_playback_loudness_meter_create(int channels, int sample_rate,
                                                  SonarePlaybackLoudnessMeter** out) {
  SONARE_C_API_ENTRY;
  if (out) *out = nullptr;
#if defined(SONARE_WITH_PLAYBACK)
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  auto handle = std::make_unique<SonarePlaybackLoudnessMeter>();
  handle->meter = std::make_unique<playback::PlaybackLoudnessMeter>(channels, sample_rate);
  *out = handle.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(channels, sample_rate, out);
#endif
}

SonareError sonare_playback_loudness_meter_push_interleaved(SonarePlaybackLoudnessMeter* meter,
                                                            const float* in, size_t frames) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PLAYBACK)
  if (!meter || (!in && frames > 0)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  meter->meter->push_interleaved(in, frames);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(meter, in, frames);
#endif
}

SonareError sonare_playback_loudness_meter_integrated_lufs(const SonarePlaybackLoudnessMeter* meter,
                                                           float* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = 0.0f;
#if defined(SONARE_WITH_PLAYBACK)
  if (!meter || !out) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  *out = meter->meter->integrated_lufs();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(meter, out);
#endif
}

void sonare_playback_loudness_meter_destroy(SonarePlaybackLoudnessMeter* meter) {
#if defined(SONARE_WITH_PLAYBACK)
  delete meter;
#else
  (void)meter;
#endif
}
