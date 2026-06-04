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
/// Applies a single CONSTANT transposition: the whole buffer is treated as one
/// note at @p current_midi and shifted by (target_midi - current_midi). This is
/// not pitch tracking — it does not follow a time-varying melody. For
/// contour-following correction use @ref sonare_pitch_correct_to_midi_timevarying
/// with a caller-supplied per-frame F0 track.
SonareError sonare_pitch_correct_to_midi(const float* samples, size_t length, int sample_rate,
                                         float current_midi, float target_midi, float** out,
                                         size_t* out_length);
/// @brief Per-frame ("time-varying") correction toward a fixed MIDI target.
/// @details Unlike @ref sonare_pitch_correct_to_midi (one constant transpose),
///          this follows a caller-supplied F0 contour: each of the @p n_frames
///          frames carries an @p f0_hz value (the measured pitch at that frame)
///          and the corrector retunes every voiced frame toward @p target_midi,
///          so vibrato/drift in the source is tracked rather than flattened.
/// @param f0_hz       Per-frame measured F0 in Hz (@p n_frames entries, required).
/// @param voiced_prob Per-frame voicing probability [0,1] (@p n_frames entries),
///                    or NULL to derive it from @p voiced (1.0 / 0.0).
/// @param voiced      Per-frame voiced flags (non-zero = voiced; @p n_frames
///                    entries), or NULL to treat every frame as voiced.
/// @param hop_length  F0 hop in samples (> 0; frame i covers sample i*hop_length).
/// @note The returned array is heap-allocated and MUST be released with
///       @ref sonare_free_floats.
SonareError sonare_pitch_correct_to_midi_timevarying(const float* samples, size_t length,
                                                     int sample_rate, const float* f0_hz,
                                                     const float* voiced_prob,
                                                     const int32_t* voiced, size_t n_frames,
                                                     int hop_length, float target_midi, float** out,
                                                     size_t* out_length);
SonareError sonare_note_stretch(const float* samples, size_t length, int sample_rate,
                                int onset_sample, int offset_sample, float stretch_ratio,
                                float** out, size_t* out_length);
SonareError sonare_voice_change(const float* samples, size_t length, int sample_rate,
                                float pitch_semitones, float formant_factor, float** out,
                                size_t* out_length);
/// @brief Convenience offline wrapper around the realtime voice changer chain.
/// @details Creates a temporary @ref SonareRealtimeVoiceChanger from @p preset,
///          processes the whole mono or interleaved stereo buffer in realtime-
///          sized blocks, and destroys the handle before returning. @p preset
///          may be NULL/empty (neutral-monitor), a preset id, or a full JSON
///          config document accepted by @ref sonare_realtime_voice_changer_create_json.
///          @p channels must be 1 (mono) or 2 (interleaved stereo).
/// @details The chain's processing latency (retune grain + ISP-limiter lookahead)
///          is compensated: an equal-length silent tail is flushed and the
///          leading @c latency_samples of pre-roll are dropped, so the returned
///          buffer is the same length as @p samples AND time-aligned with the
///          input (no head silence / tail truncation).
/// @note The returned array is heap-allocated and MUST be released with
///       @ref sonare_free_floats.
SonareError sonare_voice_change_realtime(const float* samples, size_t length, int sample_rate,
                                         const char* preset, int channels, float** out,
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

  /// @brief Enables the optional 4x-oversampled inter-sample peak (true-peak)
  ///        limiter as the final output stage (non-zero = enabled). Mirrors
  ///        editing::voice_changer::LimiterConfig::enable_isp_limiter. Defaults
  ///        to 1 (enabled).
  int limiter_enable_isp_limiter;
  /// @brief True-peak ceiling in dBTP applied by the ISP limiter when
  ///        @ref limiter_enable_isp_limiter is non-zero. Mirrors
  ///        editing::voice_changer::LimiterConfig::isp_ceiling_dbtp. Defaults to
  ///        -1.0 dBTP.
  float limiter_isp_ceiling_dbtp;
} SonareRealtimeVoiceChangerConfig;

// Verify the POD struct layout is stable. The ABI version constant
// SONARE_VOICE_CHANGER_ABI_VERSION below MUST be bumped whenever this size
// or any field offset changes. Bindings that rely on POD memcpy across the
// FFI boundary (Rust FFI, raw C ABI consumers) read this size at compile
// time and detect ABI drift before a single byte is exchanged.
//
// Layout: 33 float fields + 3 int fields, every member is 4 bytes and
// 4-byte aligned -> no struct padding on any target we ship. Exact equality
// (not >=) so silent padding insertion fails the check too.
#ifdef __cplusplus
static_assert(sizeof(SonareRealtimeVoiceChangerConfig) == 36u * sizeof(float),
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
#define SONARE_VOICE_CHANGER_ABI_VERSION 2u

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

/// @section engine_threading Thread safety (RealtimeEngine)
/// @details The engine surface (`sonare_engine_*`) is built around two roles:
/// the AUDIO thread (the render callback) and one CONTROL thread (the host / UI
/// thread that drives the transport and edits state). Unless noted otherwise a
/// given engine handle has exactly one of each; do not call from more than one
/// control thread without external serialization.
///
/// AUDIO-thread functions (realtime-safe: no allocation, no lock, no throw —
/// call only from the render callback):
/// - `sonare_engine_process`, `sonare_engine_process_with_monitor`.
///
/// CONTROL-thread, realtime-safe hand-off (lock-free, non-allocating; safe to
/// call concurrently with `sonare_engine_process` on the same handle, adopted
/// at the next block boundary). These either enqueue a command on the engine's
/// lock-free command queue or publish through a lock-free snapshot:
/// - Transport / tempo / loop: `sonare_engine_play`, `sonare_engine_stop`,
///   `sonare_engine_seek_sample`, `sonare_engine_seek_ppq`,
///   `sonare_engine_set_tempo`, `sonare_engine_set_time_signature`,
///   `sonare_engine_set_loop`, `sonare_engine_seek_marker`,
///   `sonare_engine_set_loop_from_markers`.
/// - Live parameter / MIDI: `sonare_engine_set_parameter`,
///   `sonare_engine_set_parameter_smoothed`, `sonare_engine_push_midi_cc`,
///   `sonare_engine_push_midi_panic`.
/// - Capture control: `sonare_engine_set_capture_buffer`,
///   `sonare_engine_arm_capture`, `sonare_engine_set_capture_punch`,
///   `sonare_engine_reset_capture`. (These publish the capture state through a
///   lock-free snapshot adopted by the audio thread; the backing capture buffer
///   passed to `set_capture_buffer` must outlive capture and must not be freed
///   while the engine is armed.) Note `reset_capture` clears the captured-frame
///   counter and so should be issued while not actively capturing.
///
/// CONTROL-thread, NON realtime-safe (allocate / build internal structures —
/// call only from the control thread, and NOT concurrently with
/// `sonare_engine_process` unless your engine build documents otherwise; these
/// are intended to be issued between renders or while stopped):
/// - Lifecycle: `sonare_engine_create`, `sonare_engine_destroy`,
///   `sonare_engine_prepare`.
/// - Topology / registration: `sonare_engine_set_graph`,
///   `sonare_engine_set_clips`, `sonare_engine_add_parameter`,
///   `sonare_engine_set_automation_lane`, `sonare_engine_set_markers`,
///   `sonare_engine_set_metronome`.
/// - Offline render: `sonare_engine_render_offline`,
///   `sonare_engine_bounce_offline`, `sonare_engine_freeze_offline` (these own
///   the audio role internally; do not also call them from a render callback).
///
/// CONTROL-thread read-back (safe to call concurrently with the audio thread;
/// returns a consistent snapshot, may lag the audio thread by up to one block):
/// - `sonare_engine_get_transport_state`, `sonare_engine_capture_status`,
///   `sonare_engine_parameter_*`, `sonare_engine_*_count`,
///   `sonare_engine_marker*`, `sonare_engine_metronome`,
///   `sonare_engine_drain_telemetry`, `sonare_engine_drain_meter_telemetry`.
///   The drain functions are single-consumer: drive them from one thread only.
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
/// @brief Registers a parameter's metadata for automation UIs.
/// @details Control-thread only (allocates the name/unit string copies). Returns
///   @c SONARE_ERROR_INVALID_PARAMETER if @p info is NULL, the value range is
///   inverted, or a parameter with the same id is already registered (duplicate
///   ids are rejected, not replaced — clear and re-register to change metadata).
///   On rejection no backing strings are retained, so repeated re-registration
///   does not leak.
SonareError sonare_engine_add_parameter(SonareRealtimeEngine* engine,
                                        const SonareParameterInfo* info);
/// @brief Removes all registered parameters and releases their backing strings.
/// @details Control-thread only. Use before re-registering a parameter id to
///   change its metadata (add() rejects duplicate ids). Not realtime-safe; do
///   not call concurrently with @ref sonare_engine_process.
SonareError sonare_engine_clear_parameters(SonareRealtimeEngine* engine);
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
/// @brief Built-in realtime synth patch for @ref sonare_engine_set_builtin_instrument.
/// @details Same zero-init contract as project bounce built-in instruments:
///          non-positive fields use the default sine patch values.
typedef struct {
  int waveform;     /* 0=sine, 1=saw, 2=square, 3=triangle */
  float gain;       /* 0 => 0.2 */
  float attack_ms;  /* 0 => 5 */
  float decay_ms;   /* 0 => 60 */
  float sustain;    /* 0 => 0.7 */
  float release_ms; /* 0 => 120 */
  int polyphony;    /* 0 => 16, clamped to [1,64] */
} SonareEngineBuiltinSynthConfig;

/// @brief Binds/replaces a built-in synth on a realtime MIDI destination.
/// @details Control-thread API. The engine owns the synth instance. Live MIDI
///          note/CC commands and scheduled MIDI clips routed to @p destination_id
///          render through this instrument.
SonareError sonare_engine_set_builtin_instrument(SonareRealtimeEngine* engine,
                                                 uint32_t destination_id,
                                                 const SonareEngineBuiltinSynthConfig* config);
/// @brief Loads (parses) SF2 bytes into the realtime engine so SoundFont
///        instruments can be bound to destinations with
///        @ref sonare_engine_set_sf2_instrument. Control-thread API; replaces
///        any previously loaded SoundFont (already-bound SF2 instruments keep
///        the SoundFont they were created with). The bytes are copied/decoded,
///        so @p data may be freed after the call.
SonareError sonare_engine_load_soundfont(SonareRealtimeEngine* engine, const uint8_t* data,
                                         size_t size);

/// @brief Versioned SF2 player patch for @ref sonare_engine_set_sf2_instrument.
/// @details Same zero-init contract as the project-bounce SF2 instruments:
///          every field uses "0 => default" (struct_version 0 => version 1).
typedef struct {
  int struct_version; /* 0 or 1 => version 1 */
  float gain;         /* master output gain (linear); 0 => 0.5 */
  int polyphony;      /* max simultaneous voices; 0 => 48, clamped to [1, 64] */
} SonareEngineSf2InstrumentConfig;

/// @brief Binds/replaces a GS-compatible SoundFont player on a realtime MIDI
///        destination, fed by the engine's loaded SoundFont
///        (@ref sonare_engine_load_soundfont must succeed first; otherwise
///        SONARE_ERROR_INVALID_STATE). Control-thread API; the engine owns the
///        player. Live MIDI input (`sonare_engine_push_midi_input_*` /
///        `sonare_engine_push_midi_note_*`) and scheduled MIDI clips routed to
///        @p destination_id render through the SoundFont (16 MIDI channels,
///        channel 10 drums, GS NRPN part edits, GS/GM SysEx resets).
SonareError sonare_engine_set_sf2_instrument(SonareRealtimeEngine* engine, uint32_t destination_id,
                                             const SonareEngineSf2InstrumentConfig* config);

/// @brief Clears any realtime instrument bound to @p destination_id.
SonareError sonare_engine_clear_midi_instrument(SonareRealtimeEngine* engine,
                                                uint32_t destination_id);
SonareError sonare_engine_midi_instrument_count(SonareRealtimeEngine* engine, size_t* out_count);
/// @brief Binds a live MIDI CC to an engine automation parameter.
/// @details Control-thread API. After binding, @ref sonare_engine_push_midi_cc
///          still routes the MIDI event to the destination instrument, and also
///          maps the 7-bit CC value into [min_value, max_value] for @p param_id.
SonareError sonare_engine_bind_midi_cc(SonareRealtimeEngine* engine, uint8_t channel,
                                       uint8_t controller, uint32_t param_id, float min_value,
                                       float max_value);
/// @brief Clears all live MIDI CC to parameter bindings.
SonareError sonare_engine_clear_midi_cc_bindings(SonareRealtimeEngine* engine);
/// @brief Returns the number of live MIDI CC bindings.
SonareError sonare_engine_midi_cc_binding_count(SonareRealtimeEngine* engine, size_t* out_count);
/// @brief Installs/replaces a live non-destructive MIDI-FX insert for one destination.
/// @details Control-thread API. The JSON accepts the same fields as
///          @ref sonare_project_bake_midi_fx, but scheduled/live MIDI events are
///          transformed at dispatch time and clip contents are not modified.
SonareError sonare_engine_set_midi_fx(SonareRealtimeEngine* engine, uint32_t destination_id,
                                      const char* config_json);
/// @brief Clears the live MIDI-FX insert on one destination.
SonareError sonare_engine_clear_midi_fx(SonareRealtimeEngine* engine, uint32_t destination_id);
/// @brief Enables the engine-owned live MIDI input source for a destination.
/// @details Hosts can push timestamped input events with
///          `sonare_engine_push_midi_input_*`; the engine drains them at block
///          start through the same `set_midi_input_source` path used by native
///          C++ hosts. `destination_id` selects the realtime MIDI destination.
SonareError sonare_engine_set_midi_input_source(SonareRealtimeEngine* engine,
                                                uint32_t destination_id);
/// @brief Clears the engine-owned live MIDI input source.
SonareError sonare_engine_clear_midi_input_source(SonareRealtimeEngine* engine);
/// @brief Number of queued events in the engine-owned live MIDI input source.
SonareError sonare_engine_midi_input_pending_count(SonareRealtimeEngine* engine, size_t* out_count);
SonareError sonare_engine_push_midi_input_note_on(SonareRealtimeEngine* engine, uint8_t group,
                                                  uint8_t channel, uint8_t note, uint8_t velocity,
                                                  int64_t port_time_samples);
SonareError sonare_engine_push_midi_input_note_off(SonareRealtimeEngine* engine, uint8_t group,
                                                   uint8_t channel, uint8_t note, uint8_t velocity,
                                                   int64_t port_time_samples);
SonareError sonare_engine_push_midi_input_cc(SonareRealtimeEngine* engine, uint8_t group,
                                             uint8_t channel, uint8_t controller, uint8_t value,
                                             int64_t port_time_samples);
/// @brief Queues an immediate live MIDI note-on to a MIDI destination.
SonareError sonare_engine_push_midi_note_on(SonareRealtimeEngine* engine, uint32_t destination_id,
                                            uint8_t group, uint8_t channel, uint8_t note,
                                            uint8_t velocity, int64_t render_frame);
/// @brief Queues an immediate live MIDI note-off to a MIDI destination.
SonareError sonare_engine_push_midi_note_off(SonareRealtimeEngine* engine, uint32_t destination_id,
                                             uint8_t group, uint8_t channel, uint8_t note,
                                             uint8_t velocity, int64_t render_frame);
/// @brief Queues an immediate (live) MIDI control change to a MIDI destination.
/// @details Routed through the engine's queueable scalar MIDI command path; the
///          synthesized MIDI 1.0 CC reaches the registered host instrument at
///          @p render_frame. Values are 7-bit; channel 0..15, group 0..15.
/// @param destination_id MIDI destination id (clip/instrument destination).
/// @param group UMP group (0..15).
/// @param channel MIDI channel (0..15).
/// @param controller Controller number (0..127).
/// @param value 7-bit controller value (0..127).
/// @param render_frame Render-frame time to apply, or -1 for immediate.
SonareError sonare_engine_push_midi_cc(SonareRealtimeEngine* engine, uint32_t destination_id,
                                       uint8_t group, uint8_t channel, uint8_t controller,
                                       uint8_t value, int64_t render_frame);
/// @brief Queues a MIDI panic (all-notes-off) releasing every sounding note.
/// @param render_frame Render-frame time to apply, or -1 for immediate.
SonareError sonare_engine_push_midi_panic(SonareRealtimeEngine* engine, int64_t render_frame);
/// @brief Reads the current engine transport state (playing/position/ppq/tempo).
SonareError sonare_engine_get_transport_state(SonareRealtimeEngine* engine,
                                              SonareTransportState* out);
SonareError sonare_normalize(const float* samples, size_t length, int sample_rate, float target_db,
                             float** out, size_t* out_length);
SonareError sonare_trim(const float* samples, size_t length, int sample_rate, float threshold_db,
                        float** out, size_t* out_length);

/// @brief Non-negative matrix factorisation of a non-negative spectrogram
///        (mirror of @c sonare::decompose / librosa.decompose.decompose).
/// @details Both output matrices are heap-allocated row-major and MUST be
///          released with @ref sonare_free_floats. @p out_w is the component
///          matrix [n_features x n_components] (length n_features*n_components)
///          and @p out_h is the activation matrix [n_components x n_frames]
///          (length n_components*n_frames). Multi-matrix shape: returned as two
///          flat buffers because sonare_c_types.h has no decompose result struct.
/// @param s Input spectrogram [n_features x n_frames] row-major (non-negative).
/// @param n_features Feature dimension (rows). Must be > 0.
/// @param n_frames Number of time frames. Must be > 0.
/// @param n_components Target number of components (k). Must be > 0.
/// @param n_iter Number of multiplicative-update iterations. Must be > 0
///        (n_iter==0 would return the raw init matrices and is rejected).
/// @param beta Beta divergence (2 = Frobenius, 1 = KL, 0 = Itakura-Saito).
/// @param out_w Receives the [n_features x n_components] component matrix.
/// @param out_w_length Receives n_features * n_components.
/// @param out_h Receives the [n_components x n_frames] activation matrix.
/// @param out_h_length Receives n_components * n_frames.
/// @note Uses deterministic random initialisation. For the SVD-based NNDSVD
///       warm-start (faster convergence), use @ref sonare_decompose_with_init.
SonareError sonare_decompose(const float* s, int n_features, int n_frames, int n_components,
                             int n_iter, float beta, float** out_w, size_t* out_w_length,
                             float** out_h, size_t* out_h_length);

/// @brief Non-negative matrix factorisation with a selectable initialiser
///        (mirror of @c sonare::decompose with the @c init argument).
/// @details Identical to @ref sonare_decompose but exposes the initialisation
///          strategy so callers can opt into the NNDSVD warm-start, which tends
///          to converge in fewer iterations. Both output matrices are
///          heap-allocated row-major and MUST be released with
///          @ref sonare_free_floats.
/// @param s Input spectrogram [n_features x n_frames] row-major (non-negative).
/// @param n_features Feature dimension (rows). Must be > 0.
/// @param n_frames Number of time frames. Must be > 0.
/// @param n_components Target number of components (k). Must be > 0.
/// @param n_iter Number of multiplicative-update iterations. Must be > 0.
/// @param beta Beta divergence (2 = Frobenius, 1 = KL, 0 = Itakura-Saito).
/// @param init Initialiser: "random" (default if NULL) or "nndsvd".
/// @param out_w Receives the [n_features x n_components] component matrix.
/// @param out_w_length Receives n_features * n_components.
/// @param out_h Receives the [n_components x n_frames] activation matrix.
/// @param out_h_length Receives n_components * n_frames.
SonareError sonare_decompose_with_init(const float* s, int n_features, int n_frames,
                                       int n_components, int n_iter, float beta, const char* init,
                                       float** out_w, size_t* out_w_length, float** out_h,
                                       size_t* out_h_length);

/// @brief Nearest-neighbour filter for spectrogram denoising
///        (mirror of @c sonare::nn_filter / librosa.decompose.nn_filter).
/// @details Output is the smoothed spectrogram [n_features x n_frames] row-major
///          (length n_features*n_frames); release with @ref sonare_free_floats.
/// @param s Input spectrogram [n_features x n_frames] row-major.
/// @param n_features Feature dimension (rows). Must be > 0.
/// @param n_frames Number of time frames. Must be > 0.
/// @param aggregate Aggregator: "mean", "median", "min" or "max". NULL = "mean".
/// @param k Number of nearest neighbours.
/// @param width Time exclusion half-width. Must be >= 0 (negative widths are
///        rejected, mirroring librosa, instead of silently disabling exclusion).
/// @param out Receives the smoothed spectrogram buffer.
/// @param out_length Receives n_features * n_frames.
SonareError sonare_nn_filter(const float* s, int n_features, int n_frames, const char* aggregate,
                             int k, int width, float** out, size_t* out_length);

/// @brief Reorders / concatenates a signal by interval slices
///        (mirror of @c sonare::remix / librosa.effects.remix).
/// @details Each pair (intervals[2*i], intervals[2*i+1]) selects samples
///          [start, end). The output is the concatenation of all slices.
///          Output is heap-allocated; release with @ref sonare_free_floats.
/// @param samples Input signal.
/// @param length Number of samples.
/// @param sample_rate Sample rate (validated, carried for API symmetry).
/// @param intervals Flat array of @p interval_count (start, end) pairs.
/// @param interval_count Number of (start, end) pairs.
/// @param align_zeros Snap boundaries to zero-crossings (non-zero = true).
/// @param out Receives the remixed signal buffer.
/// @param out_length Receives the remixed signal length.
SonareError sonare_remix(const float* samples, size_t length, int sample_rate, const int* intervals,
                         size_t interval_count, int align_zeros, float** out, size_t* out_length);

/// @brief HPSS with residual: separates audio into harmonic, percussive and
///        residual signals (mirror of @c sonare::hpss_with_residual).
/// @details All three outputs share the same @p out_length and @p out_sample_rate
///          (residual = original - harmonic - percussive). Each buffer is
///          heap-allocated and MUST be released with @ref sonare_free_floats.
///          Three-signal shape: emitted as three flat buffers because
///          sonare_c_types.h has no with-residual HPSS result struct.
/// @param samples Input audio.
/// @param length Number of samples.
/// @param sample_rate Sample rate.
/// @param kernel_harmonic Horizontal median filter size (odd, >= 3).
/// @param kernel_percussive Vertical median filter size (odd, >= 3).
/// @param out_harmonic Receives the harmonic signal.
/// @param out_percussive Receives the percussive signal.
/// @param out_residual Receives the residual signal.
/// @param out_length Receives the (shared) signal length.
/// @param out_sample_rate Receives the (shared) sample rate.
SonareError sonare_hpss_with_residual(const float* samples, size_t length, int sample_rate,
                                      int kernel_harmonic, int kernel_percussive,
                                      float** out_harmonic, float** out_percussive,
                                      float** out_residual, size_t* out_length,
                                      int* out_sample_rate);

/// @brief Phase-vocoder time-scale modification of audio
///        (STFT -> @c sonare::phase_vocoder -> iSTFT).
/// @details Faithful audio wrapper: computes the STFT, time-stretches the
///          spectrogram with phase coherence, and reconstructs audio. Output is
///          heap-allocated; release with @ref sonare_free_floats.
/// @param samples Input audio.
/// @param length Number of samples.
/// @param sample_rate Sample rate.
/// @param rate Time stretch rate (< 1.0 = slower, > 1.0 = faster). Must be > 0.
/// @param n_fft FFT size used for analysis/synthesis.
/// @param hop_length Hop length used for analysis/synthesis.
/// @param out Receives the time-stretched audio buffer.
/// @param out_length Receives the output length.
SonareError sonare_phase_vocoder(const float* samples, size_t length, int sample_rate, float rate,
                                 int n_fft, int hop_length, float** out, size_t* out_length);

#ifdef __cplusplus
}
#endif
