#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
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

/// @brief Populates @p out with the default configuration used by
///        @ref sonare_realtime_voice_changer_create when its config is NULL.
/// @details This is the normalized neutral-monitor preset. Do not use a
///          zero-initialized POD as a default: in particular, zero disables
///          the ISP limiter whereas the library default enables it.
SonareError sonare_realtime_voice_changer_config_default(SonareRealtimeVoiceChangerConfig* out);

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
///          @ref sonare_realtime_voice_changer_preset_names, a JSON object
///          following the realtime-voice-changer-preset schema, or the flat
///          @ref SonareRealtimeVoiceChangerConfig field names used by bindings.
///          JSON schema documents and flat configs must be complete; partial
///          documents are rejected rather than silently filled with defaults.
///          The created
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
/// @warning Control-thread operation: do not call concurrently with
///          sonare_realtime_voice_changer_process. For a live configuration
///          change, use sonare_realtime_voice_changer_set_config_json instead.
SonareError sonare_realtime_voice_changer_reset(SonareRealtimeVoiceChanger* handle);
/// @brief Realtime-safe configuration update; accepts a preset id or a complete JSON config.
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
/// @brief Reports the prepared chain's processing latency in samples. The dry
///        and wet paths are aligned to the retune OLA's fixed three-hop delay,
///        so the value is independent of `wet_mix` and `retune.mix`. When the
///        ISP limiter is enabled, it runs after that aligned mix and adds its
///        signal-path latency: a 6-sample FIR group delay plus
///        `ceil(5 * 0.1 ms * sample_rate)` attack-settle samples (31 samples at
///        48 kHz). Other stages add <= 8
///        samples combined and are intentionally omitted. The value is 0
///        before the handle has been prepared.
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

#ifdef __cplusplus
}
#endif
