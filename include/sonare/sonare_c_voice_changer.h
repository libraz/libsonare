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
///   neither allocate nor throw. They also do not access or clear the
///   thread-local `sonare_last_error_message()` / `sonare_last_warning_message()`
///   channels, because first-touch TLS setup is not realtime-safe: they report
///   a rejected argument through their return code only, and leave whatever a
///   previous call on the same thread recorded untouched. Validate arguments on
///   the configuration thread before entering the render callback.
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

/// @brief Flat POD mirror of @c editing::voice_changer::RealtimeVoiceChangerConfig
///        for C callers that want to avoid the JSON round-trip.
/// @details Field ordering follows the nested C++ struct (top-level →
///          retune → formant → eq → gate → compressor → deesser → reverb →
///          limiter → voice engine → pitch correction → harmony → vocoder →
///          character → reverb variation → midi). Enum fields hold the ordinals of the
///          matching Sonare* enums; an unknown ordinal is refused. A non-finite or out-of-range
///          entry is refused with
///          @c SONARE_ERROR_INVALID_PARAMETER (the message names the field, its range and the
///          value) and the handle keeps its previous configuration, as the JSON entry points do.
typedef struct {
  /// @brief Layout version; must be 2. Earlier layouts are refused.
  int32_t struct_version;

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
  /// @brief A @ref SonareFormantMode. Relative (0, the default) leaves @ref formant_factor to the
  ///        colour stage after the retune; absolute (1) warps the formants to
  ///        @ref formant_factor times the input's ahead of the retune, adds one analysis frame of
  ///        latency and ignores @ref formant_amount. Fixed when the handle is created: a
  ///        set_config carrying a different mode is SONARE_ERROR_INVALID_PARAMETER. Absolute
  ///        mode needs formant_factor / 2^(retune_semitones / 12) within [0.55, 1.65]; the call
  ///        is refused otherwise, with the reachable formant-factor range in
  ///        sonare_last_error_message().
  int32_t formant_mode;

  float eq_highpass_hz;
  float eq_body_db;
  float eq_presence_db;
  float eq_air_db;

  /* Gate, compressor and limiter attack/release times are one time constant:
     about 63% of the way at the set time. */
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

  /// @brief A @ref SonarePitchEngine. Psola (the default) tracks f0; grain is the f0-less SOLA
  ///        retune and refuses pitch correction, harmony and the vocoder. Fixed at creation.
  int32_t voice_engine_engine;
  /// @brief Lowest tracked f0 in Hz, [50, 200]; sets the psola latency. Fixed at creation.
  float voice_engine_f0_min_hz;

  /// @brief A @ref SonarePitchCorrectionMode.
  int32_t pitch_correction_mode;
  float pitch_correction_amount;
  float pitch_correction_speed_ms;
  /// @brief Scale root, 0 = C .. 11 = B; validated but unused for a chromatic scale.
  int32_t pitch_correction_key_root;
  /// @brief A @ref SonareScaleKind.
  int32_t pitch_correction_scale;
  int32_t pitch_correction_robot_octave;

  /// @brief Harmony voices beside the lead, [0, 3].
  int32_t harmony_voices;
  /// @brief Scale degrees of each harmony voice from the lead (0 = unison, +2 = a third up).
  int32_t harmony_steps[3];
  float harmony_level;
  float harmony_lead_level;
  float harmony_detune_cents;
  float harmony_delay_ms;

  /// @brief Non-zero enables the channel vocoder.
  int32_t vocoder_enabled;
  /// @brief Band count, [8, 32]. Fixed at creation.
  int32_t vocoder_bands;
  float vocoder_attack_ms;
  float vocoder_release_ms;
  float vocoder_sibilance;
  /// @brief A @ref SonareVocoderCarrierSource. External reads the carrier passed to the
  ///        *_with_carrier process entries.
  int32_t vocoder_carrier_source;
  /// @brief A @ref SonareVocoderCarrierWave.
  int32_t vocoder_carrier_wave;
  float vocoder_ensemble_detune_cents;
  float vocoder_level;

  /// @brief A @ref SonareCharacterType.
  int32_t character_type;
  float character_amount;
  float character_tone;

  /// @brief A @ref SonareReverbVariation; every variation shares @ref reverb_mix.
  int32_t reverb_variation_variation;
  float reverb_variation_delay_ms;
  float reverb_variation_feedback;
  float reverb_variation_tempo_bpm;
  /// @brief A @ref SonareNoteDivision for the tempo delay.
  int32_t reverb_variation_division;

  /// @brief A @ref SonareMidiRelease: what the voices do once every held note is released.
  int32_t midi_release;
} SonareRealtimeVoiceChangerConfig;

// Verify the POD struct layout is stable. The ABI version constant
// SONARE_VOICE_CHANGER_ABI_VERSION below MUST be bumped whenever this size
// or any field offset changes. Bindings that rely on POD memcpy across the
// FFI boundary (Rust FFI, raw C ABI consumers) read this size at compile
// time and detect ABI drift before a single byte is exchanged.
//
// Layout: 69 four-byte scalar fields and one 3-element int32 array, every
// member 4-byte aligned -> no struct padding on any target we ship. Exact
// equality (not >=) so silent padding insertion fails the check too.
#ifdef __cplusplus
static_assert(sizeof(SonareRealtimeVoiceChangerConfig) == 72u * sizeof(float),
              "SonareRealtimeVoiceChangerConfig unexpected size");
#endif

/// @brief Compile-time mirror of the runtime ABI version returned by
///        @ref sonare_voice_changer_abi_version. Bindings can `static_assert` /
///        `assertEqual` the runtime value against this at attach time.
#define SONARE_VOICE_CHANGER_ABI_VERSION 4u

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
///        a flat POD config. Pass NULL to start from the neutral-monitor preset. A config
///        refused by validation, a @p sample_rate outside the supported range or a
///        @p max_block_size outside [1, 1048576] is SONARE_ERROR_INVALID_PARAMETER.
/// @note Release @p out with @ref sonare_realtime_voice_changer_destroy; it is a handle, not a
///       sonare_free_* buffer.
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
///          A document whose content is refused (an unknown preset id, a malformed or
///          out-of-range field, a combination the chain cannot run) is
///          SONARE_ERROR_INVALID_FORMAT; a @p sample_rate outside the supported range, a
///          @p max_block_size outside [1, 1048576] or a @p num_channels other than 1 or 2 is
///          SONARE_ERROR_INVALID_PARAMETER.
/// @note Release @p out with @ref sonare_realtime_voice_changer_destroy; it is a handle, not a
///       sonare_free_* buffer.
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
/// @details A document whose content is refused is SONARE_ERROR_INVALID_FORMAT and the previous
///          configuration stays in effect.
SonareError sonare_realtime_voice_changer_set_config_json(SonareRealtimeVoiceChanger* handle,
                                                          const char* preset_or_config_json);
/// @brief Processes a single mono block. @p num_samples must be <= max_block_size.
SonareError sonare_realtime_voice_changer_process_mono(SonareRealtimeVoiceChanger* handle,
                                                       const float* input, float* output,
                                                       size_t num_samples);
/// @brief Processes an interleaved block. Uses a pre-allocated planar scratch
///        buffer; never allocates at runtime.
/// @param output Caller-owned block written only on success and left untouched on failure, so a
///        rejected call leaves whatever the host had there rather than punching a gap in it.
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
/// @brief Same as @ref sonare_realtime_voice_changer_process_mono, with an external vocoder
///        carrier of @p num_samples samples.
/// @details The carrier is read only while the vocoder is enabled with
///          @c SONARE_VOCODER_CARRIER_EXTERNAL; otherwise it is ignored. A NULL @p carrier is
///          SONARE_ERROR_INVALID_PARAMETER. The carrier-less process entries return
///          SONARE_ERROR_INVALID_STATE, writing no output, while an external carrier is configured.
SonareError sonare_realtime_voice_changer_process_mono_with_carrier(
    SonareRealtimeVoiceChanger* handle, const float* input, const float* carrier, float* output,
    size_t num_samples);
/// @brief Interleaved variant of @ref sonare_realtime_voice_changer_process_mono_with_carrier;
///        @p carrier is mono, @p num_frames samples, shared by every channel.
SonareError sonare_realtime_voice_changer_process_interleaved_with_carrier(
    SonareRealtimeVoiceChanger* handle, const float* input, const float* carrier, float* output,
    size_t num_frames, int num_channels);
/// @brief Planar-stereo variant of @ref sonare_realtime_voice_changer_process_mono_with_carrier;
///        @p carrier is mono, @p num_frames samples, shared by both channels.
SonareError sonare_realtime_voice_changer_process_planar_stereo_with_carrier(
    SonareRealtimeVoiceChanger* handle, float* left, float* right, const float* carrier,
    size_t num_frames);
/// @brief Holds MIDI note @p note from the start of the next processed block.
/// @details Held notes become the voices' pitch targets whatever the pitch correction mode, the
///          newest leading, up to 1 + harmony_voices of them. Realtime-safe and lock-free; one
///          producer thread for the note entries. @p note outside [0, 127] or @p velocity outside
///          (0, 1] is SONARE_ERROR_INVALID_PARAMETER; an accepted @p velocity is reserved and
///          does not change the rendered output. A grain-engine handle, or a full event queue
///          (the event is dropped), is SONARE_ERROR_INVALID_STATE. Like the process entries it
///          reports through its return code only.
SonareError sonare_realtime_voice_changer_note_on(SonareRealtimeVoiceChanger* handle, int note,
                                                  float velocity);
/// @brief Releases MIDI note @p note from the start of the next processed block; releasing a note
///        that is not held succeeds and does nothing. Errors as
///        @ref sonare_realtime_voice_changer_note_on.
SonareError sonare_realtime_voice_changer_note_off(SonareRealtimeVoiceChanger* handle, int note);
/// @brief Releases every held note from the start of the next processed block, bypassing the
///        event queue, so it succeeds even when the queue is full. Realtime-safe.
SonareError sonare_realtime_voice_changer_all_notes_off(SonareRealtimeVoiceChanger* handle);
/// @brief Reports the prepared chain's processing latency in samples. With the
///        psola engine the dry path, the voice stage and the vocoder are aligned to
///        max(ceil(sr / voice_engine_f0_min_hz), the vocoder's lowest-band group delay);
///        with the grain engine to the retune OLA's fixed one-grain delay. Either way
///        the value is independent of `wet_mix`, `retune.mix` and the vocoder switch. When the
///        ISP limiter is enabled, it runs after that aligned mix and adds its
///        signal-path latency: a 6-sample FIR group delay plus the samples a
///        five-time-constant attack settle needs, 31 in total at 48 kHz. Ask
///        for the number rather than deriving it -- the settle term rounds up
///        in single precision, so a rate whose product is already whole gains a
///        sample. Other stages add <= 8 samples combined and are intentionally
///        omitted. The value is 0 before the handle has been prepared.
SonareError sonare_realtime_voice_changer_latency_samples(const SonareRealtimeVoiceChanger* handle,
                                                          int* out_latency_samples);
/// @brief Number of blocks in which the chain discarded its own state because a
///        non-finite value had reached it.
/// @details Advisory telemetry, and the only thing that separates a degraded
///   stream from a clean one. Every stage of this chain leaves an in-domain
///   finite value where a non-finite one was -- the input scrub and the
///   inter-sample-peak limiter substitute silence, the sample-domain limiter
///   folds an infinity onto its ceiling -- so the output stays finite, in range
///   and free of any error while carrying samples unrelated to the input. A
///   non-zero count is what says the samples in between were not computed from
///   what was supplied.
///   Monotonic for the lifetime of the handle. The unit is one processed block,
///   never a channel, so a stereo block that discards on both channels adds one
///   and the number does not depend on a dimension the caller did not choose.
///   Realtime-safe.
SonareError sonare_realtime_voice_changer_non_finite_discard_count(
    const SonareRealtimeVoiceChanger* handle, uint32_t* out_count);
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
///          @ref SONARE_REALTIME_VOICE_CHANGER_PRESET_IDS. Never NULL: a build
///          without voice-changer support returns the empty string.
const char* sonare_realtime_voice_changer_preset_names(void);
/// @brief Canonical newline-separated list of voice-changer preset identifiers.
/// @details Bindings (TS unions, Python enums, etc.) should reference this
///          macro instead of hand-copying the literal strings. The separator
///          mirrors @ref sonare_realtime_voice_changer_preset_names.
#define SONARE_REALTIME_VOICE_CHANGER_PRESET_IDS \
  "neutral-monitor\n"                            \
  "bright-idol\n"                                \
  "soft-whisper\n"                               \
  "deep-narrator\n"                              \
  "robot-mascot\n"                               \
  "dark-villain\n"                               \
  "hard-tune\n"                                  \
  "robot-monotone\n"                             \
  "choir-harmony\n"                              \
  "vocoder-strings\n"                            \
  "megaphone\n"                                  \
  "radio-voice"
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
///          (return SONARE_ERROR_INVALID_FORMAT), @p out_error receives a
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
