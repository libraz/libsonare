#pragma once

/// @file sonare_c_playback.h
/// @brief C ABI for the playback renderer: decoded movie audio (mono, stereo,
///        5.1 or 7.1 PCM) rendered to headphones (binaural, head tracking,
///        room model) or to stereo / 5.1 / 7.1 speakers (upmix, loudness
///        alignment, night-mode DRC, dialogue level, speaker calibration,
///        bass management). The symbols are always exported by the shared C
///        ABI; feature-off builds return SONARE_ERROR_NOT_SUPPORTED.
///
/// Configuration is a JSON document following
/// `schemas/playback-renderer-config.schema.json`. Every key is either a
/// realtime key (adopted at the next block boundary) or a prepare key (fixed at
/// creation; changing one needs a new renderer).
///
/// @section playback_threading Thread safety
/// - `sonare_playback_renderer_process_planar`,
///   `sonare_playback_renderer_process_interleaved` and
///   `sonare_playback_renderer_set_head_orientation` are realtime-safe: they
///   neither allocate nor throw, and they do not touch the thread-local
///   `sonare_last_error_message()` channel. They report a rejected argument
///   through the return code only.
/// - `sonare_playback_renderer_set_config_json` parses and allocates on the
///   calling thread and may run concurrently with a `_process_*` call on the
///   same handle; the new realtime values are adopted at the next block
///   boundary. Two threads MUST NOT call it concurrently with each other.
/// - `sonare_playback_renderer_set_head_orientation` may be called from any
///   single thread concurrently with `_process_*`.
/// - `_process_*`, `sonare_playback_renderer_reset` and
///   `sonare_playback_renderer_destroy` MUST NOT run concurrently with each
///   other on one handle. To reset during playback (a seek), call reset from
///   the thread that calls `_process_*`, or stop the stream first.
///
/// @section playback_latency Latency
/// `sonare_playback_renderer_latency_samples` depends only on the output
/// target, the sample rate and the speaker distance compensation. It does not
/// change with realtime keys or with the input layout.

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Opaque playback renderer handle.
typedef struct SonarePlaybackRenderer SonarePlaybackRenderer;
/// @brief Opaque HRTF set handle (SHRF v1 data).
typedef struct SonareHrtfSet SonareHrtfSet;
/// @brief Opaque integrated-loudness meter for multichannel program material.
typedef struct SonarePlaybackLoudnessMeter SonarePlaybackLoudnessMeter;

#ifndef __EMSCRIPTEN__
/// @brief Creates the built-in HRTF set.
/// @note Release @p out with @ref sonare_hrtf_set_destroy; it is a handle, not
///       a sonare_free_* buffer.
SonareError sonare_hrtf_set_create_default(SonareHrtfSet** out);
#endif

/// @brief Creates an HRTF set from SHRF v1 bytes.
/// @details The bytes are copied; @p data may be released after the call.
///          Malformed data returns SONARE_ERROR_INVALID_PARAMETER.
/// @note Release @p out with @ref sonare_hrtf_set_destroy; it is a handle, not
///       a sonare_free_* buffer.
SonareError sonare_hrtf_set_create_from_memory(const uint8_t* data, size_t size,
                                               SonareHrtfSet** out);
/// @brief Releases an HRTF set. A renderer created from it keeps its own copy.
void sonare_hrtf_set_destroy(SonareHrtfSet* set);

/// @brief Creates a playback renderer.
/// @details @p config_json may be complete or partial; omitted keys take the
///          schema defaults and unknown keys return
///          SONARE_ERROR_INVALID_PARAMETER. For a headphones target @p hrtf may
///          be NULL on native builds (the built-in set is used); WASM builds
///          require it. For a speakers target @p hrtf is ignored.
///          @p max_block_size bounds the frames of every `_process_*` call.
/// @note Release @p out with @ref sonare_playback_renderer_destroy; it is a
///       handle, not a sonare_free_* buffer.
SonareError sonare_playback_renderer_create_json(const char* config_json, const SonareHrtfSet* hrtf,
                                                 int sample_rate, int max_block_size,
                                                 SonarePlaybackRenderer** out);
/// @brief Releases a renderer.
void sonare_playback_renderer_destroy(SonarePlaybackRenderer* renderer);
/// @brief Clears all DSP state (filters, FIFOs, dynamics, convolution history,
///        pending input-layout drains). Configuration and head pose are kept.
SonareError sonare_playback_renderer_reset(SonarePlaybackRenderer* renderer);
/// @brief Applies a complete configuration document.
/// @details A prepare key that differs from the current value returns
///          SONARE_ERROR_INVALID_PARAMETER ("requires a new renderer: <key>").
SonareError sonare_playback_renderer_set_config_json(SonarePlaybackRenderer* renderer,
                                                     const char* config_json);
/// @brief Returns the current complete configuration document.
/// @note Release @p out_json with @ref sonare_free_string.
SonareError sonare_playback_renderer_config_json(const SonarePlaybackRenderer* renderer,
                                                 char** out_json);
/// @brief Publishes the listener head orientation in degrees.
/// @details Right-handed: positive yaw turns the head right, positive pitch
///          looks up, positive roll lowers the right ear. Applied in
///          yaw, pitch, roll order. Ignored by speakers targets.
SonareError sonare_playback_renderer_set_head_orientation(SonarePlaybackRenderer* renderer,
                                                          float yaw_deg, float pitch_deg,
                                                          float roll_deg);
/// @brief Channel count of the active input layout.
/// @details With `input.layout = "auto"` this follows the channel count of the
///          most recent non-empty `_process_*` call (2 before the first call).
SonareError sonare_playback_renderer_input_channel_count(const SonarePlaybackRenderer* renderer,
                                                         int* out);
/// @brief Channel count of the output target.
SonareError sonare_playback_renderer_output_channel_count(const SonarePlaybackRenderer* renderer,
                                                          int* out);
/// @brief Renders one planar block.
/// @details @p frames must not exceed the max block size; 0 is a no-op that
///          never switches the input layout. With a fixed input layout
///          @p in_channels must equal the input channel count; with "auto" it
///          must be 1, 2, 6 or 8, and a change switches the input layout at the
///          start of this block without changing the latency. @p out_channels
///          must equal the output channel count. A rejected call does not
///          advance any state. Non-finite input samples are replaced with 0 and
///          counted.
SonareError sonare_playback_renderer_process_planar(SonarePlaybackRenderer* renderer,
                                                    const float* const* in, int in_channels,
                                                    float* const* out, int out_channels,
                                                    int frames);
/// @brief Interleaved variant of @ref sonare_playback_renderer_process_planar.
/// @details @p in and @p out must not alias.
SonareError sonare_playback_renderer_process_interleaved(SonarePlaybackRenderer* renderer,
                                                         const float* in, int in_channels,
                                                         float* out, int out_channels, int frames);
/// @brief Renderer latency in frames (headphones: near ear).
SonareError sonare_playback_renderer_latency_samples(const SonarePlaybackRenderer* renderer,
                                                     int* out);
/// @brief Diagnostics document: inactive stages, per-stage latency, clamps,
///        active input layout, layout switch and truncated drain counters.
/// @note Release @p out_json with @ref sonare_free_string.
SonareError sonare_playback_renderer_diagnostics_json(const SonarePlaybackRenderer* renderer,
                                                      char** out_json);
/// @brief Number of non-finite input samples replaced with 0 since creation.
SonareError sonare_playback_renderer_non_finite_discard_count(
    const SonarePlaybackRenderer* renderer, uint32_t* out_count);

/// @brief Renders a whole interleaved buffer offline.
/// @details The output is aligned with the input (the renderer latency is
///          removed) and has @p frames frames of @p out_channels channels.
/// @note Release @p out with @ref sonare_free_playback_render.
SonareError sonare_playback_render_interleaved(const float* in, size_t frames, int in_channels,
                                               int sample_rate, const char* config_json,
                                               const SonareHrtfSet* hrtf, float** out,
                                               size_t* out_frames, int* out_channels);
/// @brief Releases a buffer returned by @ref sonare_playback_render_interleaved.
void sonare_free_playback_render(float* samples);

/// @brief Creates an integrated-loudness meter (BS.1770 channel weights by
///        channel count: 1, 2, 6 or 8).
/// @note Release @p out with @ref sonare_playback_loudness_meter_destroy; it is
///       a handle, not a sonare_free_* buffer.
SonareError sonare_playback_loudness_meter_create(int channels, int sample_rate,
                                                  SonarePlaybackLoudnessMeter** out);
/// @brief Feeds interleaved frames of any length.
SonareError sonare_playback_loudness_meter_push_interleaved(SonarePlaybackLoudnessMeter* meter,
                                                            const float* in, size_t frames);
/// @brief Integrated loudness of everything pushed so far, in LUFS.
SonareError sonare_playback_loudness_meter_integrated_lufs(const SonarePlaybackLoudnessMeter* meter,
                                                           float* out);
/// @brief Releases a meter.
void sonare_playback_loudness_meter_destroy(SonarePlaybackLoudnessMeter* meter);

#ifdef __cplusplus
}
#endif
