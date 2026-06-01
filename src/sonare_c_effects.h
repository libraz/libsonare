#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Effects
// ============================================================================

SonareError sonare_hpss(const float* samples, size_t length, int sample_rate, int kernel_harmonic,
                        int kernel_percussive, SonareHpssResult* out);
SonareError sonare_harmonic(const float* samples, size_t length, int sample_rate, float** out,
                            size_t* out_length);
SonareError sonare_percussive(const float* samples, size_t length, int sample_rate, float** out,
                              size_t* out_length);
SonareError sonare_time_stretch(const float* samples, size_t length, int sample_rate, float rate,
                                float** out, size_t* out_length);
SonareError sonare_pitch_shift(const float* samples, size_t length, int sample_rate,
                               float semitones, float** out, size_t* out_length);
SonareError sonare_pitch_correct_to_midi(const float* samples, size_t length, int sample_rate,
                                         float current_midi, float target_midi, float** out,
                                         size_t* out_length);
SonareError sonare_note_stretch(const float* samples, size_t length, int sample_rate,
                                int onset_sample, int offset_sample, float stretch_ratio,
                                float** out, size_t* out_length);
SonareError sonare_voice_change(const float* samples, size_t length, int sample_rate,
                                float pitch_semitones, float formant_factor, float** out,
                                size_t* out_length);

/// @brief Flat POD mirror of @c editing::voice_changer::RealtimeVoiceChangerConfig
///        for C callers that want to avoid the JSON round-trip.
/// @details Field ordering follows the nested C++ struct (top-level →
///          retune → formant → eq → gate → compressor → deesser → reverb →
///          limiter). Values pass through @c normalize_realtime_voice_changer_config
///          before being applied, so out-of-range entries are clamped rather than
///          rejected (matching the JSON entry point).
typedef struct {
  float input_gain_db;
  float output_gain_db;
  float wet_mix;

  float retune_semitones;
  float retune_mix;
  int retune_grain_size;

  float formant_factor;
  float formant_amount;
  float formant_body;
  float formant_brightness;
  float formant_nasal;

  float eq_highpass_hz;
  float eq_body_db;
  float eq_presence_db;
  float eq_air_db;

  float gate_threshold_db;
  float gate_attack_ms;
  float gate_release_ms;
  float gate_range_db;

  float compressor_threshold_db;
  float compressor_ratio;
  float compressor_attack_ms;
  float compressor_release_ms;
  float compressor_makeup_gain_db;

  float deesser_frequency_hz;
  float deesser_threshold_db;
  float deesser_ratio;
  float deesser_range_db;

  float reverb_mix;
  float reverb_time_ms;
  float reverb_damping;
  int reverb_seed;

  float limiter_ceiling_db;
  float limiter_release_ms;
} SonareRealtimeVoiceChangerConfig;

// Verify the POD struct layout is stable. The ABI version constant
// SONARE_VOICE_CHANGER_ABI_VERSION below MUST be bumped whenever this size
// or any field offset changes. Bindings that rely on POD memcpy across the
// FFI boundary (Rust FFI, raw C ABI consumers) read this size at compile
// time and detect ABI drift before a single byte is exchanged.
//
// Layout: 32 float fields + 2 int fields, every member is 4 bytes and
// 4-byte aligned -> no struct padding on any target we ship. Exact equality
// (not >=) so silent padding insertion fails the check too.
#ifdef __cplusplus
static_assert(sizeof(SonareRealtimeVoiceChangerConfig) == 34u * sizeof(float),
              "SonareRealtimeVoiceChangerConfig unexpected size");
#endif

/// @section voice_changer_threading Thread safety
/// @details
/// - `sonare_realtime_voice_changer_process_mono`,
///   `sonare_realtime_voice_changer_process_interleaved`,
///   `sonare_realtime_voice_changer_process_planar_stereo` and
///   `sonare_realtime_voice_changer_latency_samples` are realtime-safe: they
///   neither allocate nor throw.
/// - `sonare_realtime_voice_changer_create*`,
///   `sonare_realtime_voice_changer_destroy`,
///   `sonare_realtime_voice_changer_set_config`,
///   `sonare_realtime_voice_changer_set_config_json` and the JSON validator
///   entry points allocate and parse — call them only from the configuration
///   thread.
/// - On a single handle, the realtime thread and the configuration thread may
///   run concurrently, but two threads MUST NOT call any `_process_*` function
///   on the same handle simultaneously.
/// - `sonare_realtime_voice_changer_set_config` and
///   `sonare_realtime_voice_changer_set_config_json` are themselves safe to
///   call concurrently with a `_process_*` function on the same handle: the
///   new configuration is published through a lock-free snapshot mechanism and
///   adopted by the realtime thread at the next block boundary. Two threads
///   MUST NOT call `set_config*` concurrently with each other on the same
///   handle (single-producer hand-off).

/// @brief Populates @p out with the canonical (normalized) defaults for the
///        named preset.
SonareError sonare_realtime_voice_changer_preset_config(SonareVoiceCharacterPreset preset,
                                                        SonareRealtimeVoiceChangerConfig* out);

/// @brief Same as @ref sonare_realtime_voice_changer_create_json but accepts
///        a flat POD config. Pass NULL to start from the neutral-monitor preset.
SonareError sonare_realtime_voice_changer_create(const SonareRealtimeVoiceChangerConfig* config,
                                                 int sample_rate, int max_block_size,
                                                 int num_channels,
                                                 SonareRealtimeVoiceChanger** out);

/// @brief Realtime-safe configuration update from a POD config.
SonareError sonare_realtime_voice_changer_set_config(
    SonareRealtimeVoiceChanger* handle, const SonareRealtimeVoiceChangerConfig* config);

/// @brief Reads the handle's current (normalized) configuration into @p out.
SonareError sonare_realtime_voice_changer_get_config(const SonareRealtimeVoiceChanger* handle,
                                                     SonareRealtimeVoiceChangerConfig* out);

/// @brief Creates a streaming realtime voice changer handle from a preset id or
///        a full chain config JSON document.
/// @details @p preset_or_config_json may be one of the preset ids returned by
///          @ref sonare_realtime_voice_changer_preset_names, or a JSON object
///          following the realtime-voice-changer-preset schema. The created
///          handle pre-allocates all internal buffers (including a planar
///          deinterleave scratch) so every subsequent process call is
///          realtime-safe.
/// @note Release the handle with @ref sonare_realtime_voice_changer_destroy.
SonareError sonare_realtime_voice_changer_create_json(const char* preset_or_config_json,
                                                      int sample_rate, int max_block_size,
                                                      int num_channels,
                                                      SonareRealtimeVoiceChanger** out);
/// @brief Releases a handle created by @ref sonare_realtime_voice_changer_create_json.
void sonare_realtime_voice_changer_destroy(SonareRealtimeVoiceChanger* handle);
/// @brief Resets streaming state (envelopes, reverb tails, smoothed gains).
SonareError sonare_realtime_voice_changer_reset(SonareRealtimeVoiceChanger* handle);
/// @brief Realtime-safe configuration update; accepts a preset id or a JSON config.
SonareError sonare_realtime_voice_changer_set_config_json(SonareRealtimeVoiceChanger* handle,
                                                          const char* preset_or_config_json);
/// @brief Processes a single mono block. @p num_samples must be <= max_block_size.
SonareError sonare_realtime_voice_changer_process_mono(SonareRealtimeVoiceChanger* handle,
                                                       const float* input, float* output,
                                                       size_t num_samples);
/// @brief Processes an interleaved block. Uses a pre-allocated planar scratch
///        buffer; never allocates at runtime.
SonareError sonare_realtime_voice_changer_process_interleaved(SonareRealtimeVoiceChanger* handle,
                                                              const float* input, float* output,
                                                              size_t num_frames, int num_channels);
/// @brief Process a block of planar (non-interleaved) stereo audio in place.
/// @details @p left and @p right point to separate float buffers of length
///          @p num_frames. The handle must have been prepared with at least
///          2 channels. Like the interleaved variant, this is realtime-safe.
SonareError sonare_realtime_voice_changer_process_planar_stereo(SonareRealtimeVoiceChanger* handle,
                                                                float* left, float* right,
                                                                size_t num_frames);
/// @brief Reports the chain's processing latency in samples (= retune grain).
SonareError sonare_realtime_voice_changer_latency_samples(const SonareRealtimeVoiceChanger* handle,
                                                          int* out_latency_samples);
/// @brief Returns the live (normalized) configuration of the handle as a JSON
///        document. Useful for UI sync and for round-tripping the post-
///        normalize state across language boundaries.
/// @note The returned string is heap-allocated and MUST be released with
///       @ref sonare_free_string.
SonareError sonare_realtime_voice_changer_config_json(const SonareRealtimeVoiceChanger* handle,
                                                      char** out_json);
/// @brief Returns a newline-separated, NUL-terminated list of preset ids.
///        The pointer points to static storage and must NOT be freed.
/// @details Newline (`\n`) is used for the separator to match the convention of
///          every other `*_names` API in this header. Bindings should split
///          the returned string on `\n` and drop empty entries. The set of
///          preset identifiers is also available at compile time via
///          @ref SONARE_REALTIME_VOICE_CHANGER_PRESET_IDS.
const char* sonare_realtime_voice_changer_preset_names(void);
/// @brief Canonical newline-separated list of voice-changer preset identifiers.
/// @details Bindings (TS unions, Python enums, etc.) should reference this
///          macro instead of hand-copying the literal strings. The separator
///          mirrors @ref sonare_realtime_voice_changer_preset_names.
#define SONARE_REALTIME_VOICE_CHANGER_PRESET_IDS \
  "neutral-monitor\nbright-idol\nsoft-whisper\ndeep-narrator\nrobot-mascot\ndark-villain"
/// @brief Returns the canonical id string for a @ref SonareVoiceCharacterPreset enum.
/// @details Returns NULL for unknown enum values. The pointer is static storage
///          and must NOT be freed.
const char* sonare_voice_character_preset_id(SonareVoiceCharacterPreset preset);
/// @brief Returns the canonical JSON document for the named preset.
/// @note The returned string is heap-allocated and MUST be released with
///       @ref sonare_free_string.
SonareError sonare_realtime_voice_changer_preset_json(const char* name, char** out_json);
/// @brief Validates a preset JSON document.
/// @details On success (return SONARE_OK), @p out_normalized_json receives a
///          canonicalized JSON copy and @p out_error stays NULL. On failure
///          (return SONARE_ERROR_INVALID_PARAMETER), @p out_error receives a
///          human-readable message and @p out_normalized_json stays NULL.
/// @note In every case, any non-NULL pointer returned through @p out_normalized_json
///       or @p out_error MUST be released with @ref sonare_free_string by the
///       caller.
SonareError sonare_realtime_voice_changer_validate_preset_json(const char* json,
                                                               char** out_normalized_json,
                                                               char** out_error);

/// @brief Compile-time mirror of the runtime ABI version returned by
///        @ref sonare_voice_changer_abi_version. Bindings can `static_assert` /
///        `assertEqual` the runtime value against this at attach time.
#define SONARE_VOICE_CHANGER_ABI_VERSION 1u

/// @brief Returns the runtime ABI version of the
///        @ref SonareRealtimeVoiceChangerConfig POD layout.
/// @details Bindings that pass the POD struct across the C ABI (Rust, raw C
///          consumers) call this at attach time and compare against their
///          compile-time expectation; a mismatch means the host libsonare was
///          built against a different struct layout and the POD path would
///          corrupt memory. JSON-based bindings (Node/Python via
///          @ref sonare_realtime_voice_changer_create_json) are tolerant of
///          layout drift and do not need to gate on this.
///
///          Distinct from @ref sonare_engine_abi_version (which tracks the
///          realtime command queue layout) so that voice-changer-only
///          consumers can pin a narrower compatibility envelope.
///
///          Always equals @ref SONARE_VOICE_CHANGER_ABI_VERSION at the time
///          libsonare was built.
uint32_t sonare_voice_changer_abi_version(void);

SonareError sonare_engine_create(SonareRealtimeEngine** out);
void sonare_engine_destroy(SonareRealtimeEngine* engine);
SonareError sonare_engine_prepare(SonareRealtimeEngine* engine, double sample_rate,
                                  int max_block_size, size_t command_capacity,
                                  size_t telemetry_capacity);
SonareError sonare_engine_play(SonareRealtimeEngine* engine, int64_t render_frame);
SonareError sonare_engine_stop(SonareRealtimeEngine* engine, int64_t render_frame);
SonareError sonare_engine_seek_sample(SonareRealtimeEngine* engine, int64_t timeline_sample,
                                      int64_t render_frame);
SonareError sonare_engine_seek_ppq(SonareRealtimeEngine* engine, double ppq, int64_t render_frame);
SonareError sonare_engine_set_tempo(SonareRealtimeEngine* engine, double bpm);
SonareError sonare_engine_set_time_signature(SonareRealtimeEngine* engine, int numerator,
                                             int denominator);
SonareError sonare_engine_set_loop(SonareRealtimeEngine* engine, double start_ppq, double end_ppq,
                                   int enabled);
SonareError sonare_engine_add_parameter(SonareRealtimeEngine* engine,
                                        const SonareParameterInfo* info);
SonareError sonare_engine_parameter_count(SonareRealtimeEngine* engine, size_t* out_count);
SonareError sonare_engine_parameter_info_by_index(SonareRealtimeEngine* engine, size_t index,
                                                  SonareParameterInfo* out);
SonareError sonare_engine_parameter_info(SonareRealtimeEngine* engine, uint32_t id,
                                         SonareParameterInfo* out);
SonareError sonare_engine_set_automation_lane(SonareRealtimeEngine* engine, uint32_t param_id,
                                              const SonareAutomationPoint* points,
                                              size_t point_count);
SonareError sonare_engine_automation_lane_count(SonareRealtimeEngine* engine, size_t* out_count);
SonareError sonare_engine_set_markers(SonareRealtimeEngine* engine,
                                      const SonareEngineMarker* markers, size_t marker_count);
SonareError sonare_engine_marker_count(SonareRealtimeEngine* engine, size_t* out_count);
SonareError sonare_engine_marker_by_index(SonareRealtimeEngine* engine, size_t index,
                                          SonareEngineMarker* out);
SonareError sonare_engine_marker(SonareRealtimeEngine* engine, uint32_t id,
                                 SonareEngineMarker* out);
SonareError sonare_engine_seek_marker(SonareRealtimeEngine* engine, uint32_t marker_id,
                                      int64_t render_frame);
SonareError sonare_engine_set_loop_from_markers(SonareRealtimeEngine* engine,
                                                uint32_t start_marker_id, uint32_t end_marker_id);
SonareError sonare_engine_set_metronome(SonareRealtimeEngine* engine,
                                        const SonareEngineMetronomeConfig* config);
SonareError sonare_engine_metronome(SonareRealtimeEngine* engine, SonareEngineMetronomeConfig* out);
SonareError sonare_engine_count_in_end_sample(SonareRealtimeEngine* engine, int64_t start_sample,
                                              int bars, int64_t* out_sample);
SonareError sonare_engine_set_clips(SonareRealtimeEngine* engine, const SonareEngineClip* clips,
                                    size_t clip_count);
SonareError sonare_engine_clip_count(SonareRealtimeEngine* engine, size_t* out_count);
SonareError sonare_engine_set_capture_buffer(SonareRealtimeEngine* engine,
                                             const SonareEngineCaptureBuffer* buffer);
SonareError sonare_engine_arm_capture(SonareRealtimeEngine* engine, int armed);
SonareError sonare_engine_set_capture_punch(SonareRealtimeEngine* engine, int64_t start_sample,
                                            int64_t end_sample, int enabled);
SonareError sonare_engine_reset_capture(SonareRealtimeEngine* engine);
SonareError sonare_engine_capture_status(SonareRealtimeEngine* engine,
                                         SonareEngineCaptureStatus* out);
SonareError sonare_engine_set_graph(SonareRealtimeEngine* engine,
                                    const SonareEngineGraphSpec* spec);
SonareError sonare_engine_graph_node_count(SonareRealtimeEngine* engine, size_t* out_count);
SonareError sonare_engine_graph_connection_count(SonareRealtimeEngine* engine, size_t* out_count);
SonareError sonare_engine_process(SonareRealtimeEngine* engine, float* const* channels,
                                  int num_channels, int num_frames);
SonareError sonare_engine_process_with_monitor(SonareRealtimeEngine* engine, float* const* channels,
                                               float* const* monitor_out, int num_channels,
                                               int num_frames);
SonareError sonare_engine_render_offline(SonareRealtimeEngine* engine, float* const* out,
                                         int num_channels, int64_t total_frames, int block_size);
SonareError sonare_engine_bounce_offline(SonareRealtimeEngine* engine,
                                         const SonareEngineBounceOptions* options,
                                         SonareEngineBounceResult* out);
/// @brief Fills @p options with documented defaults for sonare_engine_bounce_offline.
/// @details This is the canonical source of bounce-option defaults for all
///   language bindings. Callers should invoke this helper first and then
///   override only the fields they care about, which guarantees the same
///   normalization target (SONARE_DEFAULT_BOUNCE_TARGET_LUFS) across the C,
///   Node, Python and WASM facades.
/// @param options Output struct; must not be NULL.
/// @return @c SONARE_OK on success or @c SONARE_ERROR_INVALID_PARAMETER if
///         @p options is NULL.
SonareError sonare_engine_bounce_options_default(SonareEngineBounceOptions* options);
/// @brief Free the heap-allocated buffer held by a bounce result.
/// @param result Result whose @c interleaved buffer is deleted and nulled.
void sonare_free_bounce_result(SonareEngineBounceResult* result);
SonareError sonare_engine_freeze_offline(SonareRealtimeEngine* engine,
                                         const SonareEngineFreezeOptions* options,
                                         SonareEngineFreezeResult* out);
SonareError sonare_engine_drain_telemetry(SonareRealtimeEngine* engine, SonareEngineTelemetry* out,
                                          size_t max_records, size_t* written);
/// @brief Drains pending meter telemetry records published by the engine.
/// @param out Caller-owned array receiving up to @p max_records entries.
/// @param max_records Capacity of @p out. May be 0 to query without copying.
/// @param out_count Receives the number of records written.
SonareError sonare_engine_drain_meter_telemetry(SonareRealtimeEngine* engine,
                                                SonareMeterTelemetryRecord* out, size_t max_records,
                                                size_t* out_count);
/// @brief Pushes a live parameter value to the engine (immediate jump).
/// @param param_id Target parameter id.
/// @param value New value.
/// @param render_frame Render-frame time to apply, or -1 for immediate.
SonareError sonare_engine_set_parameter(SonareRealtimeEngine* engine, uint32_t param_id,
                                        float value, int64_t render_frame);
/// @brief Pushes a live parameter value to the engine using a smoothed ramp.
SonareError sonare_engine_set_parameter_smoothed(SonareRealtimeEngine* engine, uint32_t param_id,
                                                 float value, int64_t render_frame);
/// @brief Reads the current engine transport state (playing/position/ppq/tempo).
SonareError sonare_engine_get_transport_state(SonareRealtimeEngine* engine,
                                              SonareTransportState* out);
SonareError sonare_normalize(const float* samples, size_t length, int sample_rate, float target_db,
                             float** out, size_t* out_length);
SonareError sonare_trim(const float* samples, size_t length, int sample_rate, float threshold_db,
                        float** out, size_t* out_length);

#ifdef __cplusplus
}
#endif
