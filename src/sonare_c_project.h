#pragma once

/// @file sonare_c_project.h
/// @brief C ABI for the headless arrangement / DAW project.
///
/// This is the curated C surface over the arrangement control-plane subsystem
/// (Project / EditHistory / EditCompiler / project serializer / MIR bridges /
/// SMF). It is the KEYSTONE that the Python / Node / WASM / CLI bindings wrap;
/// it deliberately exposes a SMALL, flat-POD surface rather than every internal
/// function.
///
/// Threading / IO: every entry point here is CONTROL-THREAD-ONLY and performs
/// NO file or device I/O. Bytes (project JSON, SMF) are exchanged through
/// caller-owned buffers; the host reads / writes storage.
///
/// Memory ownership: opaque @ref SonareProject handles are created / destroyed
/// via @ref sonare_project_create / @ref sonare_project_destroy. Heap buffers
/// returned through out-pointers are freed with the existing
/// @ref sonare_free_string (for char*) and @ref sonare_free_floats (for float*).
///
/// Feature gating: the symbols are ALWAYS exported. When libsonare was built
/// without @c SONARE_WITH_ARRANGEMENT every function returns
/// @c SONARE_ERROR_NOT_SUPPORTED and @ref sonare_project_abi_version returns 0.
///
/// ABI stability: every flat POD struct below is guarded by a @c static_assert
/// on its exact size / member offsets (C++ only). Bump
/// @ref SONARE_PROJECT_ABI_VERSION on ANY layout change so POD-memcpy consumers
/// (Rust FFI, raw C) detect drift before exchanging a single byte. This version
/// is INDEPENDENT of @ref sonare_engine_abi_version (the RT command-queue ABI)
/// and of @c SONARE_PROJECT_SCHEMA_VERSION (the JSON schema version).

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_types.h"

/// @brief Compile-time mirror of the runtime project ABI version returned by
///        @ref sonare_project_abi_version. Bump on ANY flat POD layout change.
#define SONARE_PROJECT_ABI_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Opaque headless-project handle. Wraps an arrangement EditHistory
///        (owning the Project + MIDI content store) plus an audio content store.
typedef struct SonareProject SonareProject;

// ============================================================================
// Flat POD descriptors / results
// ============================================================================

/// Track kind ordinals; mirror sonare::arrangement::Track::Kind. Pinned by a
/// static_assert in the .cpp so reordering the C++ enum is caught.
typedef enum {
  SONARE_TRACK_AUDIO = 0,
  SONARE_TRACK_MIDI = 1,
  SONARE_TRACK_AUX = 2
} SonareProjectTrackKind;

/// @brief Description for @ref sonare_project_add_track. The track name is an
///        optional NUL-terminated C string (NULL = empty name).
typedef struct {
  int kind; /* SonareProjectTrackKind */
  const char* name;
} SonareProjectTrackDesc;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectTrackDesc, kind) == 0, "TrackDesc.kind offset");
static_assert(offsetof(SonareProjectTrackDesc, name) == sizeof(void*), "TrackDesc.name offset");
static_assert(sizeof(SonareProjectTrackDesc) == 2u * sizeof(void*),
              "SonareProjectTrackDesc layout drift");
#endif

/// @brief Description for @ref sonare_project_add_clip.
///
/// For an AUDIO clip, optionally supply decoded interleaved samples
/// (@p audio_interleaved / @p audio_frames / @p audio_channels /
/// @p audio_sample_rate). When @p audio_interleaved is non-NULL the call
/// registers a fresh audio source, populates the audio content store (so the
/// clip is renderable by @ref sonare_project_bounce), and binds the clip to it.
/// When @p audio_interleaved is NULL the clip is created against a metadata-only
/// audio source (URI from @p source_uri, which may be NULL/empty).
///
/// For a MIDI clip set @p is_midi to non-zero: a MIDI source is registered and
/// the clip is bound to it. MIDI events are supplied separately via
/// @ref sonare_project_set_midi_events.
///
/// All musical positions are PPQ (quarter notes). @p length_ppq must be > 0.
typedef struct {
  uint32_t track_id;
  int is_midi; /* 0 = audio clip, non-zero = MIDI clip */

  double start_ppq;
  double length_ppq;
  double source_offset_ppq;
  float gain;

  /* Audio source content (audio clips only; NULL => metadata-only source). */
  const float* audio_interleaved;
  int64_t audio_frames;
  int audio_channels;
  int audio_sample_rate;

  /* Optional host-local source reference for metadata-only audio sources. */
  const char* source_uri;
} SonareProjectClipDesc;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectClipDesc, track_id) == 0, "ClipDesc.track_id offset");
static_assert(offsetof(SonareProjectClipDesc, is_midi) == 4, "ClipDesc.is_midi offset");
static_assert(offsetof(SonareProjectClipDesc, start_ppq) == 8, "ClipDesc.start_ppq offset");
static_assert(offsetof(SonareProjectClipDesc, length_ppq) == 16, "ClipDesc.length_ppq offset");
static_assert(offsetof(SonareProjectClipDesc, source_offset_ppq) == 24,
              "ClipDesc.source_offset_ppq offset");
static_assert(offsetof(SonareProjectClipDesc, gain) == 32, "ClipDesc.gain offset");
// The tail (pointer + the int64/int run + trailing pointer) is pointer-size
// dependent, so express offsets relative to sizeof(void*) instead of hardcoding
// 64-bit values. This holds on both LP64/LLP64 native (8-byte pointers ->
// 40/48/56/60/64, size 72) and wasm32 (4-byte pointers -> 36/40/48/52/56,
// size 64). Native flat-POD memcpy FFI consumers are 64-bit; wasm marshals via
// embind, but the layout stays self-consistent on every target.
static_assert(offsetof(SonareProjectClipDesc, audio_interleaved) == 32u + sizeof(void*),
              "ClipDesc.audio_interleaved offset");
static_assert(offsetof(SonareProjectClipDesc, audio_frames) == 32u + 2u * sizeof(void*),
              "ClipDesc.audio_frames offset");
static_assert(offsetof(SonareProjectClipDesc, audio_channels) == 40u + 2u * sizeof(void*),
              "ClipDesc.audio_channels offset");
static_assert(offsetof(SonareProjectClipDesc, audio_sample_rate) == 44u + 2u * sizeof(void*),
              "ClipDesc.audio_sample_rate offset");
static_assert(offsetof(SonareProjectClipDesc, source_uri) == 48u + 2u * sizeof(void*),
              "ClipDesc.source_uri offset");
static_assert(sizeof(SonareProjectClipDesc) ==
                  ((48u + 3u * sizeof(void*) + 7u) & ~static_cast<size_t>(7u)),
              "SonareProjectClipDesc layout drift");
#endif

/// @brief One compile diagnostic surfaced by @ref sonare_project_compile.
///        Mirrors sonare::arrangement::Diagnostic (code / severity / target_id).
typedef struct {
  uint32_t code;      /* sonare::arrangement::Diagnostic::Code ordinal */
  uint32_t severity;  /* 0 = error, 1 = warning */
  uint32_t target_id; /* affected clip / track / source id (0 = n/a) */
} SonareProjectDiagnostic;

#ifdef __cplusplus
static_assert(sizeof(SonareProjectDiagnostic) == 3u * sizeof(uint32_t),
              "SonareProjectDiagnostic layout drift");
static_assert(offsetof(SonareProjectDiagnostic, code) == 0, "Diagnostic.code offset");
static_assert(offsetof(SonareProjectDiagnostic, severity) == 4, "Diagnostic.severity offset");
static_assert(offsetof(SonareProjectDiagnostic, target_id) == 8, "Diagnostic.target_id offset");
#endif

/// @brief Result of @ref sonare_project_compile. @p diagnostics is a
///        heap-allocated array of @p diagnostic_count entries (free with
///        @ref sonare_project_free_compile_result). @p messages is the
///        newline-joined human-readable detail of every diagnostic (heap C
///        string, same free function). @p has_timeline is non-zero when
///        compilation produced a renderable timeline (no error diagnostics).
typedef struct {
  SonareProjectDiagnostic* diagnostics;
  size_t diagnostic_count;
  char* messages;
  int has_timeline;
} SonareProjectCompileResult;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectCompileResult, diagnostics) == 0,
              "CompileResult.diagnostics offset");
static_assert(offsetof(SonareProjectCompileResult, diagnostic_count) == sizeof(void*),
              "CompileResult.diagnostic_count offset");
static_assert(offsetof(SonareProjectCompileResult, messages) == sizeof(void*) + sizeof(size_t),
              "CompileResult.messages offset");
// Trailing `int has_timeline` + tail padding rounds the struct up to pointer
// alignment, so the int contributes exactly sizeof(void*) on both 64-bit
// (int 4 + pad 4 = 8) and wasm32 (int 4 = 4). Avoids a hardcoded 64-bit size.
static_assert(sizeof(SonareProjectCompileResult) == 3u * sizeof(void*) + sizeof(size_t),
              "SonareProjectCompileResult layout drift");
#endif

/// @brief Options for @ref sonare_project_bounce. Zero-initialize then override.
typedef struct {
  int64_t total_frames;           /* render length in frames at @p sample_rate */
  int block_size;                 /* render block size; <= 0 => 128 */
  int num_channels;               /* output channel count; <= 0 => 2 */
  int sample_rate;                /* output sample rate; <= 0 => the project's */
  int instrument_latency_samples; /* host-instrument PDC fed to the compiler */
} SonareProjectBounceOptions;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectBounceOptions, total_frames) == 0,
              "BounceOptions.total_frames offset");
static_assert(sizeof(SonareProjectBounceOptions) == sizeof(int64_t) + 4 * sizeof(int),
              "SonareProjectBounceOptions layout drift");
#endif

/// @brief A flat MIDI event POD bridging to sonare::arrangement::MidiClipEvent.
///
/// Layout rationale: this C POD is the stable public channel-voice event shape.
/// @p data0 and @p data1 are the first two UMP words of a channel-voice
/// message: callers building a note may pack
///   data0 = (0x2u << 28) | (status << 16) | (note << 8) | velocity, data1 = 0
/// (matching @ref make_midi1_note_on word[0]). The core stores these words in
/// the project and the compiler / SMF exporter interpret valid UMP channel-voice
/// packets as MIDI events. SysEx payloads imported from SMF are carried by an
/// internal side store and are preserved by project serialization / SMF export;
/// they are not constructible through this flat C event POD. @p ppq is the event
/// position within the clip source timeline (quarter notes). Prefer the
/// sonare_midi_* helpers below over hand-packing words.
typedef struct {
  double ppq;
  uint32_t data0;
  uint32_t data1;
} SonareMidiEventPod;

#ifdef __cplusplus
static_assert(sizeof(SonareMidiEventPod) == sizeof(double) + 2u * sizeof(uint32_t),
              "SonareMidiEventPod layout drift");
static_assert(offsetof(SonareMidiEventPod, ppq) == 0, "MidiEventPod.ppq offset");
static_assert(offsetof(SonareMidiEventPod, data0) == 8, "MidiEventPod.data0 offset");
static_assert(offsetof(SonareMidiEventPod, data1) == 12, "MidiEventPod.data1 offset");
#endif

// ============================================================================
// ABI version
// ============================================================================

/// @brief Runtime ABI version of the flat project POD layout. Equals
///        @ref SONARE_PROJECT_ABI_VERSION when the arrangement subsystem is
///        compiled in, 0 otherwise (so bindings can detect the absence).
uint32_t sonare_project_abi_version(void);

// ============================================================================
// Lifecycle / IO / render
// ============================================================================

/// @brief Creates an empty headless project handle.
SonareError sonare_project_create(SonareProject** out);
/// @brief Destroys a project handle. NULL is a safe no-op.
void sonare_project_destroy(SonareProject* project);

/// @brief Serializes the project (+ MIDI content) to deterministic JSON.
/// @param out_json Receives a heap C string (free with @ref sonare_free_string).
/// @param out_len  Receives the JSON byte length (excluding the NUL), or NULL.
SonareError sonare_project_serialize(const SonareProject* project, char** out_json,
                                     size_t* out_len);

/// @brief Deserializes project JSON into a NEW project handle.
/// @param json Project JSON (need not be NUL-terminated; @p len bytes are read).
/// @param len  Byte length of @p json.
/// @param out  Receives the new handle on success.
/// @param out_diag Optional; on failure receives a heap C string with the joined
///        diagnostic messages (free with @ref sonare_free_string). On success it
///        is set to NULL. Pass NULL to ignore. Malformed input returns an error
///        WITHOUT crashing.
SonareError sonare_project_deserialize(const char* json, size_t len, SonareProject** out,
                                       char** out_diag);

/// @brief Sets the project sample rate (Hz). Must be > 0.
SonareError sonare_project_set_sample_rate(SonareProject* project, double sample_rate);

/// @brief Compiles the project into an RT-readable timeline, surfacing
///        diagnostics. Never throws; bad input yields error diagnostics.
/// @param out Zero-initialized result; free with
///        @ref sonare_project_free_compile_result.
SonareError sonare_project_compile(SonareProject* project, SonareProjectCompileResult* out);

/// @brief Frees the heap buffers held by a compile result and zeroes it.
void sonare_project_free_compile_result(SonareProjectCompileResult* result);

/// @brief Compiles + renders the project offline to interleaved float audio.
/// @param options Render options (zero-init then override). May be NULL for
///        defaults (project sample rate, 2 channels, block 128).
/// @param out_interleaved Receives a heap float array (free with
///        @ref sonare_free_floats) of length @p out_len = total_frames * channels.
/// @param out_len Receives the interleaved sample count.
/// @details Deterministic: the same project + options yields bit-identical
///          output within one build.
SonareError sonare_project_bounce(SonareProject* project, const SonareProjectBounceOptions* options,
                                  float** out_interleaved, size_t* out_len);

// ============================================================================
// Edit (all mutation routes through EditHistory commands)
// ============================================================================

/// @brief Adds a track; returns the allocated stable id via @p out_track_id.
SonareError sonare_project_add_track(SonareProject* project, const SonareProjectTrackDesc* desc,
                                     uint32_t* out_track_id);

/// @brief Adds a clip (audio or MIDI per @p desc); returns the allocated clip id.
SonareError sonare_project_add_clip(SonareProject* project, const SonareProjectClipDesc* desc,
                                    uint32_t* out_clip_id);

/// @brief Convenience wrapper that creates a MIDI track + a MIDI clip on it.
///        Returns the allocated track and clip ids. @p length_ppq must be > 0.
SonareError sonare_project_add_midi_clip(SonareProject* project, double start_ppq,
                                         double length_ppq, uint32_t* out_track_id,
                                         uint32_t* out_clip_id);

/// @brief Splits a clip at @p split_ppq (absolute PPQ). @p out_new_clip_id
///        receives the right-hand clip id (may be NULL).
SonareError sonare_project_split_clip(SonareProject* project, uint32_t clip_id, double split_ppq,
                                      uint32_t* out_new_clip_id);

/// @brief Trims a clip's start / length (PPQ). See arrangement::TrimClip.
SonareError sonare_project_trim_clip(SonareProject* project, uint32_t clip_id, double new_start_ppq,
                                     double new_length_ppq);

/// @brief Moves a clip to @p new_start_ppq, optionally to @p new_track_id
///        (0 = keep current track).
SonareError sonare_project_move_clip(SonareProject* project, uint32_t clip_id, double new_start_ppq,
                                     uint32_t new_track_id);

/// @brief Undoes the most recent edit. Returns SONARE_ERROR_INVALID_STATE when
///        the undo stack is empty.
SonareError sonare_project_undo(SonareProject* project);
/// @brief Redoes the most recently undone edit. Returns
///        SONARE_ERROR_INVALID_STATE when the redo stack is empty.
SonareError sonare_project_redo(SonareProject* project);

// ============================================================================
// MIDI
// ============================================================================

/// @brief Replaces a MIDI clip's entire event list in the content store. Pass
///        @p count == 0 to clear. Events are stored opaquely (see
///        @ref SonareMidiEventPod).
SonareError sonare_project_set_midi_events(SonareProject* project, uint32_t clip_id,
                                           const SonareMidiEventPod* events, size_t count);

/// @brief Packs a MIDI 1.0 note-on event POD at @p ppq.
SonareError sonare_midi_note_on(double ppq, uint8_t group, uint8_t channel, uint8_t note,
                                uint8_t velocity, SonareMidiEventPod* out);

/// @brief Packs a MIDI 1.0 note-off event POD at @p ppq.
SonareError sonare_midi_note_off(double ppq, uint8_t group, uint8_t channel, uint8_t note,
                                 uint8_t velocity, SonareMidiEventPod* out);

/// @brief Packs a MIDI 1.0 control-change event POD at @p ppq.
SonareError sonare_midi_cc(double ppq, uint8_t group, uint8_t channel, uint8_t controller,
                           uint8_t value, SonareMidiEventPod* out);

/// @brief Packs a MIDI 1.0 poly-pressure event POD at @p ppq.
SonareError sonare_midi_poly_pressure(double ppq, uint8_t group, uint8_t channel, uint8_t note,
                                      uint8_t pressure, SonareMidiEventPod* out);

/// @brief Packs a MIDI 1.0 program-change event POD at @p ppq.
SonareError sonare_midi_program(double ppq, uint8_t group, uint8_t channel, uint8_t program,
                                SonareMidiEventPod* out);

/// @brief Packs a MIDI 1.0 channel-pressure event POD at @p ppq.
SonareError sonare_midi_channel_pressure(double ppq, uint8_t group, uint8_t channel,
                                         uint8_t pressure, SonareMidiEventPod* out);

/// @brief Packs a MIDI 1.0 pitch-bend event POD at @p ppq. @p bend is 0..16383,
///        center = 8192.
SonareError sonare_midi_pitch_bend(double ppq, uint8_t group, uint8_t channel, uint16_t bend,
                                   SonareMidiEventPod* out);

/// @brief Imports an in-memory SMF byte buffer, adding one MIDI track + clip per
///        imported channel-voice track. @p out_first_clip_id (optional) receives
///        the id of the first added clip. Malformed input returns an error
///        without crashing.
SonareError sonare_project_import_smf(SonareProject* project, const uint8_t* bytes, size_t len,
                                      uint32_t* out_first_clip_id);

/// @brief Exports the project's tempo map + MIDI clips to an SMF byte buffer.
/// @param out_bytes Receives a heap byte array (free with @ref sonare_free_bytes).
/// @param out_len   Receives the byte length.
/// @note Valid MIDI 1.0/2.0 channel-voice UMP words stored via
///       @ref sonare_project_set_midi_events are emitted. MIDI 2.0-only events
///       that cannot be represented in SMF/MIDI 1.0 are dropped by the MIDI
///       exporter, matching @ref sonare::midi::export_smf.
SonareError sonare_project_export_smf(const SonareProject* project, uint8_t** out_bytes,
                                      size_t* out_len);

/// @brief Sets a MIDI clip's program / bank by inserting deterministic
///        MIDI-1.0 bank-select CC + program-change events at source PPQ 0.
///        Uses group 0 / channel 0. For explicit routing use
///        @ref sonare_project_set_program_on_channel.
///        @p program must be in [0,127]. @p bank may be -1 (no bank select) or
///        a 14-bit bank number [0,16383] encoded as CC0(MSB), CC32(LSB).
SonareError sonare_project_set_program(SonareProject* project, uint32_t clip_id, int program,
                                       int bank);

/// @brief Sets a MIDI clip's program / bank on a specific UMP group/channel.
///        Existing PPQ-0 bank-select/program events for that same group/channel
///        are replaced; other channels' program events are preserved.
SonareError sonare_project_set_program_on_channel(SonareProject* project, uint32_t clip_id,
                                                  uint8_t group, uint8_t channel, int program,
                                                  int bank);

/// @brief Applies a deterministic MIDI-FX transform to a clip's stored events.
///
/// Supported JSON object fields are optional:
///   - transpose_semitones: integer
///   - velocity_scale / velocity_offset / velocity_gamma: numbers
///   - quantize_ppq: positive PPQ grid, with quantize_strength in [0,1]
///   - chord_intervals: array of up to 8 integer semitone offsets
///   - humanize_ppq / humanize_velocity / seed: deterministic jitter controls
///
/// This is an offline/control-plane destructive transform over the clip's event
/// list, not a live RT insert chain. Malformed JSON returns
/// SONARE_ERROR_INVALID_FORMAT; invalid values return
/// SONARE_ERROR_INVALID_PARAMETER.
SonareError sonare_project_set_midi_fx(SonareProject* project, uint32_t clip_id,
                                       const char* config_json);

// ============================================================================
// MIR (offline analysis -> edit commands; deterministic)
// ============================================================================

/// @brief Detects tempo from a mono audio buffer and installs the primary
///        tempo-segment estimate via an edit command (undoable). Uses the
///        beat analysis -> tempo bridge. @p out_bpm (optional) receives the
///        primary BPM.
SonareError sonare_project_auto_tempo(SonareProject* project, const float* audio, size_t len,
                                      int sample_rate, float* out_bpm);

/// @brief Snaps a PPQ coordinate to the nearest beat of the project's grid at
///        that position. @p strength in [0,1] (0 = no snap, 1 = exact line).
SonareError sonare_project_snap_to_grid(const SonareProject* project, double ppq, double strength,
                                        double* out_ppq);

// ============================================================================
// Memory management (heap byte buffers)
// ============================================================================

/// @brief Frees a heap byte buffer returned by @ref sonare_project_export_smf.
void sonare_free_bytes(uint8_t* ptr);

#ifdef __cplusplus
}
#endif
