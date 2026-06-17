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
///        @ref sonare_project_abi_version.
///
/// This is the PUBLISHED project ABI contract. Bump it once per RELEASE that
/// changes the flat POD layout a distributed binary exposes — NOT on every
/// in-development edit. The project ABI has not shipped in any release yet
/// (absent from v1.2.3), so it stays at 1 while the surface is still being
/// built out; additive, unreleased changes do not require a bump.
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

/// Project clip-overlap policy ordinals.
typedef enum {
  SONARE_PROJECT_OVERLAP_DISALLOW = 0,
  SONARE_PROJECT_OVERLAP_ALLOW = 1
} SonareProjectOverlapPolicy;

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
  float gain; /* linear playback gain; must be finite and >= 0. 0 = silent clip
                 (passed through verbatim — no coercion to unity). */

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

/// @brief Description for @ref sonare_project_add_loop_recording_takes.
///
/// The input is a captured interleaved audio buffer. The project tempo map is
/// used to convert @p loop_length_ppq at @p start_ppq into an audio-frame loop
/// span; each loop span becomes a separate audio source/take, and one clip of
/// length @p loop_length_ppq is added to @p track_id. The newest take is made
/// active. A final partial loop is kept as the last take.
typedef struct {
  uint32_t track_id;
  uint32_t reserved;
  double start_ppq;
  double loop_length_ppq;
  const float* audio_interleaved;
  int64_t audio_frames;
  int audio_channels;
  int audio_sample_rate;
} SonareProjectLoopRecordingDesc;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectLoopRecordingDesc, track_id) == 0,
              "LoopRecordingDesc.track_id offset");
static_assert(offsetof(SonareProjectLoopRecordingDesc, reserved) == sizeof(uint32_t),
              "LoopRecordingDesc.reserved offset");
static_assert(offsetof(SonareProjectLoopRecordingDesc, start_ppq) == sizeof(double),
              "LoopRecordingDesc.start_ppq offset");
static_assert(offsetof(SonareProjectLoopRecordingDesc, loop_length_ppq) == 2u * sizeof(double),
              "LoopRecordingDesc.loop_length_ppq offset");
static_assert(offsetof(SonareProjectLoopRecordingDesc, audio_interleaved) == 3u * sizeof(double),
              "LoopRecordingDesc.audio_interleaved offset");
static_assert(offsetof(SonareProjectLoopRecordingDesc, audio_frames) ==
                  ((3u * sizeof(double) + sizeof(void*) + 7u) & ~static_cast<size_t>(7u)),
              "LoopRecordingDesc.audio_frames offset");
static_assert(sizeof(SonareProjectLoopRecordingDesc) == 6u * sizeof(double),
              "SonareProjectLoopRecordingDesc layout drift");
#endif

/// @brief One warp-map anchor for @ref sonare_project_set_warp_map.
typedef struct {
  double warp_sample;
  double source_sample;
} SonareProjectWarpAnchor;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectWarpAnchor, warp_sample) == 0, "WarpAnchor.warp_sample offset");
static_assert(offsetof(SonareProjectWarpAnchor, source_sample) == sizeof(double),
              "WarpAnchor.source_sample offset");
static_assert(sizeof(SonareProjectWarpAnchor) == 2u * sizeof(double),
              "SonareProjectWarpAnchor layout drift");
#endif

typedef enum {
  SONARE_PROJECT_WARP_MODE_OFF = 0,
  SONARE_PROJECT_WARP_MODE_REPITCH = 1,
  SONARE_PROJECT_WARP_MODE_TEMPO_SYNC = 2,
} SonareProjectWarpMode;

/// @brief First-class project warp-map descriptor.
typedef struct {
  uint32_t id;
  const char* name;
  const SonareProjectWarpAnchor* anchors;
  size_t anchor_count;
} SonareProjectWarpMapDesc;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectWarpMapDesc, id) == 0, "WarpMapDesc.id offset");
static_assert(offsetof(SonareProjectWarpMapDesc, name) == sizeof(void*), "WarpMapDesc.name offset");
static_assert(offsetof(SonareProjectWarpMapDesc, anchors) == 2u * sizeof(void*),
              "WarpMapDesc.anchors offset");
static_assert(offsetof(SonareProjectWarpMapDesc, anchor_count) == 3u * sizeof(void*),
              "WarpMapDesc.anchor_count offset");
static_assert(sizeof(SonareProjectWarpMapDesc) == 3u * sizeof(void*) + sizeof(size_t),
              "SonareProjectWarpMapDesc layout drift");
#endif

/// @brief Project tempo segment descriptor for @ref sonare_project_set_tempo_segments.
///
/// @p start_sample is an output/derived timeline value kept in this POD for ABI
/// compatibility with compile/import surfaces. The setter ignores it and
/// derives sample positions from @p start_ppq / @p bpm during tempo-map
/// normalization.
typedef struct {
  double start_ppq;
  double bpm;
  double start_sample;
  double end_bpm; /* 0 = constant tempo */
} SonareProjectTempoSegment;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectTempoSegment, start_ppq) == 0, "TempoSegment.start_ppq offset");
static_assert(offsetof(SonareProjectTempoSegment, bpm) == 8, "TempoSegment.bpm offset");
static_assert(offsetof(SonareProjectTempoSegment, start_sample) == 16,
              "TempoSegment.start_sample offset");
static_assert(offsetof(SonareProjectTempoSegment, end_bpm) == 24, "TempoSegment.end_bpm offset");
static_assert(sizeof(SonareProjectTempoSegment) == 4u * sizeof(double),
              "SonareProjectTempoSegment layout drift");
#endif

/// @brief Project time-signature segment descriptor.
typedef struct {
  double start_ppq;
  int numerator;
  int denominator;
} SonareProjectTimeSignatureSegment;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectTimeSignatureSegment, start_ppq) == 0,
              "TimeSignatureSegment.start_ppq offset");
static_assert(offsetof(SonareProjectTimeSignatureSegment, numerator) == sizeof(double),
              "TimeSignatureSegment.numerator offset");
static_assert(offsetof(SonareProjectTimeSignatureSegment, denominator) ==
                  sizeof(double) + sizeof(int),
              "TimeSignatureSegment.denominator offset");
static_assert(sizeof(SonareProjectTimeSignatureSegment) == 2u * sizeof(double),
              "SonareProjectTimeSignatureSegment layout drift");
#endif

/// @brief One compile diagnostic surfaced by @ref sonare_project_compile.
///        Mirrors sonare::arrangement::Diagnostic (code / severity / target_id).
///        Human-readable messages are stored in the same order in
///        SonareProjectCompileResult::messages, one line per diagnostic, with
///        embedded CR/LF normalized to spaces to preserve this frozen POD layout
///        across the C ABI boundary.
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
///        string, same free function), with line @c i corresponding to
///        @c diagnostics[i]. @p has_timeline is non-zero when compilation
///        produced a renderable timeline (no error diagnostics).
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

/// @brief Heap-owned AssistSidecar snapshot returned by
///        @ref sonare_project_get_assist_sidecar.
///
/// @p module_id is a heap C string (free via
/// @ref sonare_project_free_assist_sidecar). @p payload is a heap byte array
/// of @p payload_len bytes (also freed by the same helper). The core treats the
/// payload as opaque module-owned bytes.
typedef struct {
  char* module_id;
  uint32_t schema_version;
  uint32_t target_track_id;
  double region_start_ppq;
  double region_end_ppq;
  uint8_t* payload;
  size_t payload_len;
} SonareProjectAssistSidecar;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectAssistSidecar, module_id) == 0,
              "AssistSidecar.module_id offset");
static_assert(offsetof(SonareProjectAssistSidecar, schema_version) == sizeof(void*),
              "AssistSidecar.schema_version offset");
static_assert(offsetof(SonareProjectAssistSidecar, target_track_id) ==
                  sizeof(void*) + sizeof(uint32_t),
              "AssistSidecar.target_track_id offset");
static_assert(offsetof(SonareProjectAssistSidecar, region_start_ppq) ==
                  ((sizeof(void*) + 2u * sizeof(uint32_t) + 7u) & ~static_cast<size_t>(7u)),
              "AssistSidecar.region_start_ppq offset");
static_assert(offsetof(SonareProjectAssistSidecar, region_end_ppq) ==
                  offsetof(SonareProjectAssistSidecar, region_start_ppq) + sizeof(double),
              "AssistSidecar.region_end_ppq offset");
static_assert(offsetof(SonareProjectAssistSidecar, payload) ==
                  offsetof(SonareProjectAssistSidecar, region_end_ppq) + sizeof(double),
              "AssistSidecar.payload offset");
static_assert(offsetof(SonareProjectAssistSidecar, payload_len) ==
                  offsetof(SonareProjectAssistSidecar, payload) + sizeof(void*),
              "AssistSidecar.payload_len offset");
static_assert(sizeof(SonareProjectAssistSidecar) ==
                  ((offsetof(SonareProjectAssistSidecar, payload_len) + sizeof(size_t) + 7u) &
                   ~static_cast<size_t>(7u)),
              "SonareProjectAssistSidecar layout drift");
#endif

/// @brief Key segment descriptor for @ref sonare_project_annotate_keys.
///
/// @p tonic_pc is 0..11 (C=0) or 255 for unknown. @p mode uses the
/// arrangement KeyMode ordinal (0 unknown, 1 major, 2 minor, 3 dorian,
/// 4 phrygian, 5 lydian, 6 mixolydian, 7 locrian).
typedef struct {
  double start_ppq;
  double end_ppq;
  uint32_t tonic_pc;
  uint32_t mode;
} SonareProjectKeySegment;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectKeySegment, start_ppq) == 0, "KeySegment.start_ppq offset");
static_assert(offsetof(SonareProjectKeySegment, end_ppq) == sizeof(double),
              "KeySegment.end_ppq offset");
static_assert(offsetof(SonareProjectKeySegment, tonic_pc) == 2u * sizeof(double),
              "KeySegment.tonic_pc offset");
static_assert(offsetof(SonareProjectKeySegment, mode) == 2u * sizeof(double) + sizeof(uint32_t),
              "KeySegment.mode offset");
static_assert(sizeof(SonareProjectKeySegment) == 2u * sizeof(double) + 2u * sizeof(uint32_t),
              "SonareProjectKeySegment layout drift");
#endif

/// @brief Chord-symbol descriptor for @ref sonare_project_annotate_chords.
///
/// @p root_pc and @p slash_bass_pc are 0..11 (C=0) or 255 for unknown / none.
/// @p quality uses the arrangement ChordQuality ordinal (0 unknown, 1 major,
/// 2 minor, 3 diminished, 4 augmented, 5 dominant, 6 half-diminished,
/// 7 suspended). @p extensions is copied and may be NULL only when
/// @p extension_count is 0. @p roman_numeral is optional and copied.
typedef struct {
  double start_ppq;
  double end_ppq;
  uint32_t root_pc;
  uint32_t quality;
  const uint8_t* extensions;
  size_t extension_count;
  uint32_t slash_bass_pc;
  const char* roman_numeral;
  int modulation_boundary;
} SonareProjectChordSymbol;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectChordSymbol, start_ppq) == 0, "ChordSymbol.start_ppq offset");
static_assert(offsetof(SonareProjectChordSymbol, end_ppq) == sizeof(double),
              "ChordSymbol.end_ppq offset");
static_assert(offsetof(SonareProjectChordSymbol, root_pc) == 2u * sizeof(double),
              "ChordSymbol.root_pc offset");
static_assert(offsetof(SonareProjectChordSymbol, quality) == 2u * sizeof(double) + sizeof(uint32_t),
              "ChordSymbol.quality offset");
static_assert(offsetof(SonareProjectChordSymbol, extensions) ==
                  ((2u * sizeof(double) + 2u * sizeof(uint32_t) + sizeof(void*) - 1u) &
                   ~(sizeof(void*) - 1u)),
              "ChordSymbol.extensions offset");
static_assert(offsetof(SonareProjectChordSymbol, extension_count) ==
                  offsetof(SonareProjectChordSymbol, extensions) + sizeof(void*),
              "ChordSymbol.extension_count offset");
static_assert(offsetof(SonareProjectChordSymbol, slash_bass_pc) ==
                  offsetof(SonareProjectChordSymbol, extension_count) + sizeof(size_t),
              "ChordSymbol.slash_bass_pc offset");
static_assert(offsetof(SonareProjectChordSymbol, roman_numeral) ==
                  ((offsetof(SonareProjectChordSymbol, slash_bass_pc) + sizeof(uint32_t) +
                    sizeof(void*) - 1u) &
                   ~(sizeof(void*) - 1u)),
              "ChordSymbol.roman_numeral offset");
static_assert(offsetof(SonareProjectChordSymbol, modulation_boundary) ==
                  offsetof(SonareProjectChordSymbol, roman_numeral) + sizeof(void*),
              "ChordSymbol.modulation_boundary offset");
static_assert(sizeof(SonareProjectChordSymbol) ==
                  ((offsetof(SonareProjectChordSymbol, modulation_boundary) + sizeof(int) + 7u) &
                   ~static_cast<size_t>(7u)),
              "SonareProjectChordSymbol layout drift");
#endif

/// @brief Options for @ref sonare_project_bounce. Zero-initialize then override.
typedef struct {
  int64_t total_frames;           /* render length in frames at @p sample_rate */
  int block_size;                 /* render block size; <= 0 => 128 */
  int num_channels;               /* output channel count; <= 0 => 2 */
  int sample_rate;                /* must match the project sample rate; <= 0 => the project's */
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

/// @brief MIDI route config for @ref sonare_midi_route_events.
/// @details `-1` means any group/channel for filters and no remap for
///          `remap_channel`. Otherwise values must be in [0, 15].
typedef struct {
  int filter_group;
  int filter_channel;
  int remap_channel;
  int thru;
} SonareMidiRouteConfig;

#ifdef __cplusplus
static_assert(offsetof(SonareMidiRouteConfig, filter_group) == 0,
              "MidiRouteConfig.filter_group offset");
static_assert(offsetof(SonareMidiRouteConfig, filter_channel) == sizeof(int),
              "MidiRouteConfig.filter_channel offset");
static_assert(offsetof(SonareMidiRouteConfig, remap_channel) == 2u * sizeof(int),
              "MidiRouteConfig.remap_channel offset");
static_assert(offsetof(SonareMidiRouteConfig, thru) == 3u * sizeof(int),
              "MidiRouteConfig.thru offset");
static_assert(sizeof(SonareMidiRouteConfig) == 4u * sizeof(int),
              "SonareMidiRouteConfig layout drift");
#endif

/// @brief MIDI CC binding kind ordinals. Mirrors midi::CcBindingKind.
typedef enum {
  SONARE_MIDI_CC_CONTROL_CHANGE_7 = 0,
  SONARE_MIDI_CC_CONTROL_CHANGE_14 = 1,
  SONARE_MIDI_CC_RPN = 2,
  SONARE_MIDI_CC_NRPN = 3
} SonareMidiCcBindingKind;

/// @brief MIDI CC <-> automation binding descriptor for pure conversion helpers.
typedef struct {
  uint8_t cc_number;
  uint8_t channel;
  uint8_t kind;
  uint8_t cc_lsb_number;
  uint8_t selector_msb;
  uint8_t selector_lsb;
  uint16_t reserved;
  uint32_t param_id;
  float min_value;
  float max_value;
} SonareMidiCcBinding;

#ifdef __cplusplus
static_assert(offsetof(SonareMidiCcBinding, cc_number) == 0, "MidiCcBinding.cc_number offset");
static_assert(offsetof(SonareMidiCcBinding, channel) == 1, "MidiCcBinding.channel offset");
static_assert(offsetof(SonareMidiCcBinding, kind) == 2, "MidiCcBinding.kind offset");
static_assert(offsetof(SonareMidiCcBinding, cc_lsb_number) == 3,
              "MidiCcBinding.cc_lsb_number offset");
static_assert(offsetof(SonareMidiCcBinding, selector_msb) == 4,
              "MidiCcBinding.selector_msb offset");
static_assert(offsetof(SonareMidiCcBinding, selector_lsb) == 5,
              "MidiCcBinding.selector_lsb offset");
static_assert(offsetof(SonareMidiCcBinding, param_id) == 8, "MidiCcBinding.param_id offset");
static_assert(offsetof(SonareMidiCcBinding, min_value) == 12, "MidiCcBinding.min_value offset");
static_assert(offsetof(SonareMidiCcBinding, max_value) == 16, "MidiCcBinding.max_value offset");
static_assert(sizeof(SonareMidiCcBinding) == 20, "SonareMidiCcBinding layout drift");
#endif

/// @brief Result of @ref sonare_project_validate_midi_notes. @p ok is 1 when
///        every note-on in the clip has a matching note-off and vice versa, else
///        0; the unmatched counts are diagnostic.
typedef struct {
  int ok;
  uint32_t unmatched_note_ons;
  uint32_t unmatched_note_offs;
} SonareNotePairValidation;

/// @brief Clip fade-curve ordinals; mirror sonare::arrangement::FadeCurve.
///        Pinned by a static_assert in the .cpp so reordering is caught.
typedef enum {
  SONARE_FADE_CURVE_LINEAR = 0,
  SONARE_FADE_CURVE_EQUAL_POWER = 1,
  SONARE_FADE_CURVE_EXPONENTIAL = 2,
  SONARE_FADE_CURVE_LOGARITHMIC = 3,
} SonareProjectFadeCurve;

/// @brief Clip loop-mode ordinals; mirror sonare::arrangement::LoopMode.
///        Pinned by a static_assert in the .cpp.
typedef enum {
  SONARE_LOOP_MODE_OFF = 0,
  SONARE_LOOP_MODE_LOOP = 1,
} SonareProjectLoopMode;

/// @brief Automation breakpoint interpolation ordinals; mirror
///        sonare::AutomationCurve. Pinned by a static_assert in the .cpp.
typedef enum {
  SONARE_CURVE_LINEAR = 0,
  SONARE_CURVE_EXPONENTIAL = 1,
  SONARE_CURVE_HOLD = 2,
  SONARE_CURVE_SCURVE = 3,
} SonareProjectAutomationCurve;

/// @brief One clip fade region for @ref sonare_project_set_clip_fade.
///        @p length_ppq must be finite and >= 0 (0 = no fade). @p curve is a
///        @ref SonareProjectFadeCurve ordinal.
typedef struct {
  double length_ppq;
  uint32_t curve; /* SonareProjectFadeCurve */
} SonareProjectClipFade;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectClipFade, length_ppq) == 0, "ClipFade.length_ppq offset");
static_assert(offsetof(SonareProjectClipFade, curve) == sizeof(double), "ClipFade.curve offset");
static_assert(sizeof(SonareProjectClipFade) == sizeof(double) + 2u * sizeof(uint32_t),
              "SonareProjectClipFade layout drift");
#endif

/// @brief One recorded take/generation attached to a clip.
///
/// @p id is clip-local and must be non-zero. @p source_id may be 0 to use the
/// clip's current source, or an existing source id compatible with the clip's
/// track kind. @p source_offset_ppq must be finite and >= 0. @p name is optional.
typedef struct {
  uint32_t id;
  uint32_t source_id;
  double source_offset_ppq;
  const char* name;
} SonareProjectClipTake;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectClipTake, id) == 0, "ClipTake.id offset");
static_assert(offsetof(SonareProjectClipTake, source_id) == sizeof(uint32_t),
              "ClipTake.source_id offset");
static_assert(offsetof(SonareProjectClipTake, source_offset_ppq) == sizeof(double),
              "ClipTake.source_offset_ppq offset");
static_assert(offsetof(SonareProjectClipTake, name) == 2u * sizeof(double), "ClipTake.name offset");
static_assert(sizeof(SonareProjectClipTake) ==
                  ((2u * sizeof(double) + sizeof(void*) + 7u) & ~static_cast<size_t>(7u)),
              "SonareProjectClipTake layout drift");
#endif

/// @brief One clip-local comp segment selecting a take over [start_ppq,end_ppq).
typedef struct {
  double start_ppq;
  double end_ppq;
  uint32_t take_id;
} SonareProjectClipCompSegment;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectClipCompSegment, start_ppq) == 0,
              "ClipCompSegment.start_ppq offset");
static_assert(offsetof(SonareProjectClipCompSegment, end_ppq) == sizeof(double),
              "ClipCompSegment.end_ppq offset");
static_assert(offsetof(SonareProjectClipCompSegment, take_id) == 2u * sizeof(double),
              "ClipCompSegment.take_id offset");
static_assert(sizeof(SonareProjectClipCompSegment) == 3u * sizeof(double),
              "SonareProjectClipCompSegment layout drift");
#endif

/// @brief Description for @ref sonare_project_add_automation_lane and
///        @ref sonare_project_edit_automation_lane.
///
/// @p target_param_id identifies the parameter the lane drives (host-defined,
/// e.g. a mixer-strip volume parameter id). @p points is an array of
/// @p point_count breakpoints (the shared @ref SonareAutomationPoint POD whose
/// @c curve_to_next uses the @ref SonareProjectAutomationCurve ordinals). It may
/// be NULL only when @p point_count is 0. The breakpoints are copied; the core
/// does not require them pre-sorted but stores them verbatim.
typedef struct {
  uint32_t target_param_id;
  const SonareAutomationPoint* points;
  size_t point_count;
} SonareAutomationLaneDesc;

#ifdef __cplusplus
static_assert(offsetof(SonareAutomationLaneDesc, target_param_id) == 0,
              "AutomationLaneDesc.target_param_id offset");
static_assert(offsetof(SonareAutomationLaneDesc, points) == sizeof(void*),
              "AutomationLaneDesc.points offset");
static_assert(offsetof(SonareAutomationLaneDesc, point_count) == 2u * sizeof(void*),
              "AutomationLaneDesc.point_count offset");
static_assert(sizeof(SonareAutomationLaneDesc) == 2u * sizeof(void*) + sizeof(size_t),
              "SonareAutomationLaneDesc layout drift");
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

/// @brief Sets the project's clip-overlap policy.
SonareError sonare_project_set_overlap_policy(SonareProject* project, uint32_t overlap_policy);

/// @brief Replaces the project tempo segment list.
SonareError sonare_project_set_tempo_segments(SonareProject* project,
                                              const SonareProjectTempoSegment* segments,
                                              size_t segment_count);

/// @brief Replaces the project time-signature segment list.
SonareError sonare_project_set_time_signatures(SonareProject* project,
                                               const SonareProjectTimeSignatureSegment* segments,
                                               size_t segment_count);

/// @brief Project timeline marker with its kind and (for key signatures) the
///        structured key. @c kind is a SonareMarkerKind ordinal; the key fields
///        apply only to the key-signature kind. The layout mirrors
///        SonareEngineMarker so one binding shape serves both.
typedef struct {
  uint32_t id;
  uint8_t kind;      /* SonareMarkerKind */
  int8_t key_fifths; /* key signature only: -7..7 (sharps positive) */
  uint8_t key_minor; /* key signature only: 0 major / 1 minor */
  double ppq;
  char name[64];
} SonareProjectMarker;

#ifdef __cplusplus
static_assert(sizeof(SonareProjectMarker) == 80u, "SonareProjectMarker layout drift");
static_assert(offsetof(SonareProjectMarker, ppq) == 8u, "SonareProjectMarker ppq offset");
static_assert(offsetof(SonareProjectMarker, name) == 16u, "SonareProjectMarker name offset");
#endif

/// @brief Adds or replaces a marker. @p marker_id 0 allocates a new id. The
///        marker is created with the default (Marker) kind; use
///        sonare_project_set_marker_ex to set a text / lyric / cue / key
///        signature kind.
SonareError sonare_project_set_marker(SonareProject* project, uint32_t marker_id, double ppq,
                                      const char* name, uint32_t* out_marker_id);

/// @brief Adds or replaces a marker from a full SonareProjectMarker, including
///        its kind and key signature. @p marker->id 0 allocates a new id; the
///        allocated / affected id is returned via @p out_marker_id. For the
///        key-signature kind @p marker->key_fifths must be in -7..7 (the SMF
///        `sf` range); an out-of-range value yields SONARE_ERROR_INVALID_PARAMETER.
SonareError sonare_project_set_marker_ex(SonareProject* project, const SonareProjectMarker* marker,
                                         uint32_t* out_marker_id);

/// @brief Reads a project marker by index (0-based, in stored order). An index
///        out of range yields SONARE_ERROR_INVALID_PARAMETER.
SonareError sonare_project_marker_by_index(const SonareProject* project, size_t index,
                                           SonareProjectMarker* out);

/// @brief Replaces the project's mixer scene from scene JSON.
SonareError sonare_project_set_mixer_scene_json(SonareProject* project, const char* scene_json);

/// @brief Reads the project sample rate (Hz).
SonareError sonare_project_get_sample_rate(const SonareProject* project, double* out_sample_rate);

/// @brief Reads the project overlap policy (SonareProjectOverlapPolicy ordinal).
SonareError sonare_project_get_overlap_policy(const SonareProject* project,
                                              uint32_t* out_overlap_policy);

/// @brief Counts value-model entities without requiring JSON serialization.
SonareError sonare_project_track_count(const SonareProject* project, size_t* out_count);
SonareError sonare_project_clip_count(const SonareProject* project, size_t* out_count);
SonareError sonare_project_source_count(const SonareProject* project, size_t* out_count);
SonareError sonare_project_marker_count(const SonareProject* project, size_t* out_count);
SonareError sonare_project_tempo_segment_count(const SonareProject* project, size_t* out_count);
SonareError sonare_project_time_signature_count(const SonareProject* project, size_t* out_count);

/// @brief Compiles the project into an RT-readable timeline, surfacing
///        diagnostics. Never throws; bad input yields error diagnostics.
/// @param out Zero-initialized result; free with
///        @ref sonare_project_free_compile_result.
SonareError sonare_project_compile(SonareProject* project, SonareProjectCompileResult* out);

/// @brief Returns compile diagnostics produced by the most recent
///        @ref sonare_project_bounce / instrument bounce on this project.
/// @details This surfaces non-fatal warnings from the bounce's internal compile
///          step, such as MIDI clips rendering silently without a bound
///          instrument. If no bounce has run, the result is empty with
///          @p has_timeline set non-zero. Free with
///          @ref sonare_project_free_compile_result.
SonareError sonare_project_last_bounce_compile_result(const SonareProject* project,
                                                      SonareProjectCompileResult* out);

/// @brief Frees the heap buffers held by a compile result and zeroes it.
void sonare_project_free_compile_result(SonareProjectCompileResult* result);

/// @brief Compiles + renders the project offline to interleaved float audio.
/// @param options Render options (zero-init then override). May be NULL for
///        defaults (project sample rate, 2 channels, block 128).
/// @param out_interleaved Receives a heap float array (free with
///        @ref sonare_free_floats) of length @p out_len = total_frames * channels.
/// @param out_len Receives the interleaved sample count.
/// @details Deterministic: the same project + options yields bit-identical
///          output within one build. When options->total_frames is positive,
///          that explicit length is used as-is; mixer FX / instrument tails are
///          auto-extended only when total_frames <= 0.
SonareError sonare_project_bounce(SonareProject* project, const SonareProjectBounceOptions* options,
                                  float** out_interleaved, size_t* out_len);

// ============================================================================
// External-instrument host shim (callback-driven)
// ============================================================================

/// @brief Callback table a host supplies to drive an EXTERNAL instrument as the
///        engine's MIDI-driven instrument during a bounce. The bounce engine
///        treats it as a sonare::midi::MidiInstrument: @p prepare runs once on
///        the control thread, then per render block the engine delivers each
///        dispatched UMP event to @p on_event at its sample-accurate render
///        frame and calls @p render to sum the instrument's audio. Invariant 6:
///        only opaque words / buffers cross this seam — NO plugin-SDK type.
///
/// All callbacks receive @p user_data verbatim. @p render is REQUIRED; the
/// others may be NULL. The table (and any state @p user_data points to) must
/// outlive the bounce call. The callbacks run on the bounce's render thread and
/// must not free the project.
typedef struct {
  void* user_data;
  /// CONTROL thread (once, before rendering): negotiated sample rate + max block.
  void (*prepare)(void* user_data, double sample_rate, int max_block_size);
  /// RENDER thread: one dispatched channel-voice UMP event for @p destination_id
  /// at @p render_frame. @p ump_words points to @p word_count (1 or 2) 32-bit
  /// UMP words; the pointer is valid only for the duration of the call.
  void (*on_event)(void* user_data, uint32_t destination_id, const uint32_t* ump_words,
                   int word_count, int64_t render_frame);
  /// RENDER thread: ADD @p num_frames of the instrument's audio into the planar
  /// @p channels (channels[ch][i]); the engine zero-fills the scratch first.
  void (*render)(void* user_data, float* const* channels, int num_channels, int num_frames);
  /// Reported instrument latency in samples (PDC). 0 = no latency.
  int latency_samples;
  /// Reported release / effect tail in samples for auto-length bounces. 0 = no tail.
  int tail_samples;
} SonareInstrumentCallbacks;

#ifdef __cplusplus
static_assert(offsetof(SonareInstrumentCallbacks, user_data) == 0,
              "InstrumentCallbacks.user_data offset");
static_assert(offsetof(SonareInstrumentCallbacks, prepare) == sizeof(void*),
              "InstrumentCallbacks.prepare offset");
static_assert(offsetof(SonareInstrumentCallbacks, latency_samples) == 4u * sizeof(void*),
              "InstrumentCallbacks.latency_samples offset");
static_assert(offsetof(SonareInstrumentCallbacks, tail_samples) == 4u * sizeof(void*) + sizeof(int),
              "InstrumentCallbacks.tail_samples offset");
static_assert(sizeof(SonareInstrumentCallbacks) == 4u * sizeof(void*) + 2u * sizeof(int),
              "SonareInstrumentCallbacks layout drift");
#endif

/// @brief Binds a callback instrument to a MIDI destination id (the value set by
///        @ref sonare_project_set_track_midi_destination and stamped onto the
///        track's compiled clips). The default destination is 0.
typedef struct {
  uint32_t destination_id;
  SonareInstrumentCallbacks callbacks;
} SonareInstrumentBinding;

#ifdef __cplusplus
static_assert(offsetof(SonareInstrumentBinding, destination_id) == 0,
              "InstrumentBinding.destination_id offset");
static_assert(offsetof(SonareInstrumentBinding, callbacks) ==
                  ((sizeof(uint32_t) + alignof(SonareInstrumentCallbacks) - 1u) &
                   ~(alignof(SonareInstrumentCallbacks) - 1u)),
              "InstrumentBinding.callbacks offset");
#endif

/// @brief Like @ref sonare_project_bounce, but registers each callback
///        instrument on the engine before rendering, so MIDI tracks routed to
///        those destinations render the host instrument's audio instead of
///        silence. @p instruments points to @p instrument_count bindings (may be
///        NULL / 0 for a silent MIDI bounce identical to sonare_project_bounce).
///        Deterministic for a fixed project + options + instrument behavior.
SonareError sonare_project_bounce_with_instruments(SonareProject* project,
                                                   const SonareProjectBounceOptions* options,
                                                   const SonareInstrumentBinding* instruments,
                                                   size_t instrument_count, float** out_interleaved,
                                                   size_t* out_len);

// ============================================================================
// Built-in instrument (minimal polyphonic oscillator synth)
// ============================================================================

/// @brief Oscillator waveform for the built-in synth (see
///        @ref SonareBuiltinSynthConfig). Out-of-range values fall back to sine.
typedef enum {
  SONARE_SYNTH_WAVEFORM_SINE = 0,
  SONARE_SYNTH_WAVEFORM_SAW = 1,
  SONARE_SYNTH_WAVEFORM_SQUARE = 2,
  SONARE_SYNTH_WAVEFORM_TRIANGLE = 3,
} SonareSynthWaveform;

/// @brief Patch for the built-in minimal synth. Zero-initialize then override:
///        a zero-init config is sanitized into a usable sine patch (every field
///        is clamped to an audible range), so callers may fill only what they
///        need. This is a deliberately plain electronic sound source so MIDI
///        arrangements bounce to audio instead of silence; a richer instrument
///        bank is planned separately.
/// Every numeric field uses "0 (or non-positive) => default", so a zero-init
/// config is the default sine patch and callers override only what they need.
typedef struct {
  int waveform;     /* SonareSynthWaveform; 0 = sine */
  float gain;       /* master output gain (linear); 0 => 0.2 */
  float attack_ms;  /* ADSR attack in ms; 0 => 5 */
  float decay_ms;   /* ADSR decay in ms; 0 => 60 */
  float sustain;    /* ADSR sustain level [0,1]; 0 => 0.7 */
  float release_ms; /* ADSR release in ms; 0 => 120 */
  int polyphony;    /* max simultaneous voices; 0 => 16, clamped to [1, 64] */
} SonareBuiltinSynthConfig;

/// @brief Binds a built-in synth patch to a MIDI destination id (the value set
///        by @ref sonare_project_set_track_midi_destination). The default
///        destination is 0.
typedef struct {
  uint32_t destination_id;
  SonareBuiltinSynthConfig config;
} SonareBuiltinInstrumentBinding;

/// @brief Like @ref sonare_project_bounce, but renders MIDI tracks routed to the
///        given destinations through the built-in oscillator synth, so a
///        MIDI-only arrangement bounces to audible output without the caller
///        supplying its own instrument callbacks. @p instruments points to
///        @p instrument_count bindings (may be NULL / 0 for a silent bounce).
///        When @p options->total_frames <= 0 the render length is auto-derived
///        from the arrangement (musical end + the synth's release tail).
///        A positive total_frames is used as-is and does not auto-extend for
///        mixer FX / instrument tails.
///        Deterministic for a fixed project + options + patch.
SonareError sonare_project_bounce_with_builtin_instruments(
    SonareProject* project, const SonareProjectBounceOptions* options,
    const SonareBuiltinInstrumentBinding* instruments, size_t instrument_count,
    float** out_interleaved, size_t* out_len);

// ============================================================================
// NativeSynth instrument (patch-driven synthesizer; see SonareSynthPatch)
// ============================================================================

/// @brief Returns the NativeSynth preset catalog names separated by '\n'
///        ("sine", "saw-lead", "e-piano", "drum-kit", ...). Use these to
///        discover valid @ref SonareSynthPatch preset names instead of
///        hardcoding magic strings.
/// @details Pointer is owned by libsonare and remains valid for the program
///          lifetime; the caller must NOT free it (mirrors
///          @ref sonare_mastering_insert_names).
const char* sonare_synth_preset_names(void);

typedef enum {
  SONARE_SYNTH_ENUM_ENGINE_MODE = 0,
  SONARE_SYNTH_ENUM_OSC_WAVEFORM = 1,
  SONARE_SYNTH_ENUM_FILTER_MODEL = 2,
  SONARE_SYNTH_ENUM_FILTER_OUTPUT = 3,
  SONARE_SYNTH_ENUM_BODY_TYPE = 4,
  SONARE_SYNTH_ENUM_MOD_SOURCE = 5,
  SONARE_SYNTH_ENUM_MOD_DESTINATION = 6
} SonareSynthEnumKind;

/// @brief Returns the canonical names for a @ref SonareSynthPatch enum
///        separated by '\n'. Unknown @p kind returns an empty string.
/// @details Pointer is owned by libsonare and remains valid for the program
///          lifetime; the caller must NOT free it.
const char* sonare_synth_enum_names(int kind);

/// @brief Fills @p out with the named catalog preset: the preset name plus the
///        wrapper-section values (oscillator / filter / envelopes / LFO /
///        realism / bus), so hosts can inspect a preset and tweak fields
///        before binding it. Passing the result back unchanged selects the
///        exact preset. Unknown names return SONARE_ERROR_INVALID_PARAMETER.
SonareError sonare_synth_preset_patch(const char* name, SonareSynthPatch* out);

/// @brief Binds a NativeSynth patch to a MIDI destination id (the value set by
///        @ref sonare_project_set_track_midi_destination; default 0).
typedef struct {
  uint32_t destination_id;
  SonareSynthPatch patch;
} SonareSynthInstrumentBinding;

/// @brief Like @ref sonare_project_bounce, but renders MIDI tracks routed to
///        the given destinations through the patch-driven NativeSynth (the
///        full synthesizer: subtractive / FM / Karplus-Strong / modal /
///        additive / percussion / extended-waveguide piano engines plus the
///        realism layer). Each binding resolves its @ref SonareSynthPatch via
///        the preset catalog + field overrides; an invalid struct_version or
///        unknown preset name fails with SONARE_ERROR_INVALID_PARAMETER.
///        When @p options->total_frames <= 0 the render length is auto-derived
///        from the arrangement (musical end + the patch's release tail).
///        A positive total_frames is used as-is and does not auto-extend for
///        mixer FX / instrument tails.
///        Deterministic for a fixed project + options + patch.
SonareError sonare_project_bounce_with_synth_instruments(
    SonareProject* project, const SonareProjectBounceOptions* options,
    const SonareSynthInstrumentBinding* instruments, size_t instrument_count,
    float** out_interleaved, size_t* out_len);

// ============================================================================
// SoundFont (SF2) instrument: host-supplied sample data, GS-compatible player
// ============================================================================

/// @brief Loads (parses) SF2 bytes into the project: presets / instruments /
///        sample headers plus the sample PCM converted to a float pool.
///        Replaces any previously loaded SoundFont. CONTROL thread; the bytes
///        are copied/decoded, so @p data may be freed after the call. On a
///        malformed file the project's SoundFont is left unchanged and
///        SONARE_ERROR_INVALID_PARAMETER is returned (sonare_last_error_message
///        carries the parser detail).
SonareError sonare_project_load_soundfont(SonareProject* project, const uint8_t* data, size_t size);

/// @brief Releases the project's loaded SoundFont (no-op when none is loaded).
SonareError sonare_project_clear_soundfont(SonareProject* project);

/// @brief Number of presets in the project's loaded SoundFont (0 when none).
SonareError sonare_project_soundfont_preset_count(SonareProject* project, size_t* out_count);

/// @brief Source backend a resolved MIDI program renders through: the loaded
///        SoundFont (kSf2) or the built-in synthesizer fallback (kSynth).
typedef enum {
  SONARE_SOURCE_BACKEND_SYNTH = 0,
  SONARE_SOURCE_BACKEND_SF2 = 1,
} SonareSourceBackend;

/// @brief One bounce-manifest entry: a (channel, bank, program) combination the
///        arrangement actually plays, with the backend it resolves to.
///        `bank` is the effective SF2 bank (drum channels report 128, melodic
///        channels the CC0 variation bank). `preset_name` is the resolved SF2
///        preset name (GS fallback included), empty for SONARE_SOURCE_BACKEND_SYNTH.
typedef struct {
  uint8_t channel; /* MIDI channel 0-15 */
  uint8_t program; /* program number 0-127 */
  uint16_t bank;   /* effective SF2 bank (128 = drums) */
  int backend;     /* SonareSourceBackend */
  char preset_name[64];
} SonareSf2ProgramStatus;

/// @brief Enumerates every (channel, bank, program) combination the compiled
///        arrangement plays a note through, in first-use order, and reports
///        whether each resolves in the loaded SoundFont (GS variation/drum
///        fallbacks included) or would fall back to the built-in synth.
///        Bank select (CC0) and program-change events are tracked per
///        (destination, channel) in event order; channel 10 resolves drums via
///        bank 128. @p out may be NULL when @p max_entries is 0 to query the
///        count: @p out_count always receives the TOTAL entry count and at most
///        @p max_entries entries are written.
SonareError sonare_project_soundfont_manifest(SonareProject* project, SonareSf2ProgramStatus* out,
                                              size_t max_entries, size_t* out_count);

/// @brief Versioned SF2 player patch for @ref
///        sonare_project_bounce_with_sf2_instruments. Zero-initialize then
///        override: every field uses "0 => default" (struct_version 0 is
///        treated as the current version 1).
typedef struct {
  int struct_version; /* 0 or 1 => version 1 */
  float gain;         /* master output gain (linear); 0 => 0.5 */
  int polyphony;      /* max simultaneous voices; 0 => 48, clamped to [1, 64] */
} SonareSf2InstrumentConfig;

/// @brief Binds an SF2 player patch to a MIDI destination id (the value set by
///        @ref sonare_project_set_track_midi_destination; default 0).
typedef struct {
  uint32_t destination_id;
  SonareSf2InstrumentConfig config;
} SonareSf2InstrumentBinding;

/// @brief Like @ref sonare_project_bounce, but renders MIDI tracks routed to
///        the given destinations through a GS-compatible SoundFont player fed
///        by the project's loaded SoundFont (@ref
///        sonare_project_load_soundfont). Each bound player is multitimbral
///        (16 MIDI channels, channel 10 drums via bank 128, GS NRPN part
///        edits and GS/GM SysEx resets honored); programs the SoundFont does
///        not cover — including bouncing with no SoundFont loaded at all —
///        play through the built-in synthesizer GM fallback bank (the
///        data-free floor; see @ref sonare_project_soundfont_manifest for the
///        per-program backend). Deterministic for a fixed project + options +
///        SoundFont + patch.
SonareError sonare_project_bounce_with_sf2_instruments(
    SonareProject* project, const SonareProjectBounceOptions* options,
    const SonareSf2InstrumentBinding* instruments, size_t instrument_count, float** out_interleaved,
    size_t* out_len);

// ============================================================================
// Edit (all mutation routes through EditHistory commands)
// ============================================================================

/// @brief Adds a track; returns the allocated stable id via @p out_track_id.
SonareError sonare_project_add_track(SonareProject* project, const SonareProjectTrackDesc* desc,
                                     uint32_t* out_track_id);

/// @brief Adds a clip (audio or MIDI per @p desc); returns the allocated clip id.
SonareError sonare_project_add_clip(SonareProject* project, const SonareProjectClipDesc* desc,
                                    uint32_t* out_clip_id);

/// @brief Splits captured loop-recording audio into takes and adds one clip.
///        Returns the allocated clip id and optional take count.
SonareError sonare_project_add_loop_recording_takes(SonareProject* project,
                                                    const SonareProjectLoopRecordingDesc* desc,
                                                    uint32_t* out_clip_id, size_t* out_take_count);

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

/// @brief Changes a track kind via an undoable edit command.
SonareError sonare_project_set_track_kind(SonareProject* project, uint32_t track_id, uint32_t kind);

/// @brief Sets a clip's warp reference id via an undoable edit command.
///
/// Use @p warp_ref_id = 0 to clear the reference. Non-zero ids are intended to
/// reference a map registered with @ref sonare_project_set_warp_map.
SonareError sonare_project_set_clip_warp_ref(SonareProject* project, uint32_t clip_id,
                                             uint32_t warp_ref_id);

/// @brief Sets a clip's warp playback mode via an undoable edit command.
SonareError sonare_project_set_clip_warp_mode(SonareProject* project, uint32_t clip_id,
                                              SonareProjectWarpMode mode);

/// @brief Replaces a clip's take list and active take via an undoable edit.
///
/// @p takes may be NULL only when @p take_count is 0. @p active_take_id may be
/// 0 to use the clip's base source, otherwise it must reference one of @p takes.
/// Existing comp segments must still reference valid take ids after replacement.
SonareError sonare_project_set_clip_takes(SonareProject* project, uint32_t clip_id,
                                          const SonareProjectClipTake* takes, size_t take_count,
                                          uint32_t active_take_id);

/// @brief Replaces a clip's comp lane via an undoable edit.
///
/// Segments are clip-local PPQ ranges and must be finite, positive length,
/// sorted, non-overlapping, inside the clip, and reference existing take ids
/// (or 0 for the base/active fallback).
SonareError sonare_project_set_clip_comp_segments(SonareProject* project, uint32_t clip_id,
                                                  const SonareProjectClipCompSegment* segments,
                                                  size_t segment_count);

/// @brief Adds or replaces a first-class project warp map via an undoable edit.
///        @p desc->id must be non-zero and @p anchors must contain at least two
///        finite, strictly increasing anchor pairs.
SonareError sonare_project_set_warp_map(SonareProject* project,
                                        const SonareProjectWarpMapDesc* desc);

/// @brief Removes a first-class project warp map via an undoable edit command.
SonareError sonare_project_remove_warp_map(SonareProject* project, uint32_t warp_ref_id);

/// @brief Routes a track's MIDI to host-instrument destination @p destination_id
///        (0 = the default destination). The arrangement compiler stamps every
///        MIDI clip on the track with this id so the engine dispatches its events
///        to the instrument registered for that destination. Routes through an
///        undoable edit command. @p track_id must reference an existing track.
SonareError sonare_project_set_track_midi_destination(SonareProject* project, uint32_t track_id,
                                                      uint32_t destination_id);

/// @brief Removes a clip via an undoable edit command. @p clip_id must
///        reference an existing clip. Undo restores the clip (and its MIDI
///        content) at its original position.
SonareError sonare_project_remove_clip(SonareProject* project, uint32_t clip_id);

/// @brief Sets a clip's linear playback gain via an undoable edit command.
///        @p gain must be finite and >= 0 (0 = muted). Like the @p gain field of
///        @ref sonare_project_add_clip, the value is applied verbatim (no
///        coercion of 0 to unity).
SonareError sonare_project_set_clip_gain(SonareProject* project, uint32_t clip_id, float gain);

/// @brief Sets a clip's fade-in and fade-out regions via an undoable edit
///        command. Each fade length (PPQ) must be finite and >= 0 (0 = no fade);
///        each curve is a @ref SonareProjectFadeCurve ordinal.
SonareError sonare_project_set_clip_fade(SonareProject* project, uint32_t clip_id,
                                         const SonareProjectClipFade* fade_in,
                                         const SonareProjectClipFade* fade_out);

/// @brief Sets a clip's loop mode + loop length (PPQ) via an undoable edit
///        command. @p loop_mode is a @ref SonareProjectLoopMode ordinal. When
///        @p loop_mode is SONARE_LOOP_MODE_LOOP, @p loop_length_ppq must be
///        finite and > 0; otherwise it must be finite and >= 0.
SonareError sonare_project_set_clip_loop(SonareProject* project, uint32_t clip_id, int loop_mode,
                                         double loop_length_ppq);

/// @brief Rebinds a clip to a different (already-registered) source via an
///        undoable edit command. @p source_id must reference an existing source;
///        @p clip_id an existing clip.
SonareError sonare_project_set_clip_source(SonareProject* project, uint32_t clip_id,
                                           uint32_t source_id);

/// @brief Duplicates a clip at @p new_start_ppq (same track), allocating a fresh
///        id and copying any MIDI content, via an undoable edit command.
///        @p out_new_clip_id receives the new clip id (may be NULL).
SonareError sonare_project_duplicate_clip(SonareProject* project, uint32_t clip_id,
                                          double new_start_ppq, uint32_t* out_new_clip_id);

/// @brief Removes a track via an undoable edit command. @p track_id must
///        reference an existing track. (The command layer removes the track's
///        clips first; undo restores the track and its clips.)
SonareError sonare_project_remove_track(SonareProject* project, uint32_t track_id);

/// @brief Renames a track via an undoable edit command. @p name is an optional
///        NUL-terminated C string (NULL = empty name). @p track_id must
///        reference an existing track.
SonareError sonare_project_rename_track(SonareProject* project, uint32_t track_id,
                                        const char* name);

/// @brief Sets a track's mixer-strip binding + output target via an undoable
///        edit command. @p channel_strip_ref and @p output_target are
///        NUL-terminated C strings; pass NULL or "" to clear the respective
///        field. @p track_id must reference an existing track.
SonareError sonare_project_set_track_route(SonareProject* project, uint32_t track_id,
                                           const char* channel_strip_ref,
                                           const char* output_target);

/// @brief Appends an automation lane to a track via an undoable edit command.
///        @p track_id must reference an existing track; @p desc describes the
///        target parameter id and breakpoints (see @ref SonareAutomationLaneDesc).
///        @p out_lane_index receives the appended lane's index within the track
///        (may be NULL).
SonareError sonare_project_add_automation_lane(SonareProject* project, uint32_t track_id,
                                               const SonareAutomationLaneDesc* desc,
                                               size_t* out_lane_index);

/// @brief Replaces an existing automation lane in place via an undoable edit
///        command. @p track_id must reference an existing track and
///        @p lane_index an existing lane on it.
SonareError sonare_project_edit_automation_lane(SonareProject* project, uint32_t track_id,
                                                size_t lane_index,
                                                const SonareAutomationLaneDesc* desc);

/// @brief Removes an automation lane from a track via an undoable edit command.
///        @p track_id must reference an existing track and @p lane_index an
///        existing lane on it.
SonareError sonare_project_remove_automation_lane(SonareProject* project, uint32_t track_id,
                                                  size_t lane_index);

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

/// @brief Returns the General MIDI Level 1 instrument name for @p program.
/// @details Returns NULL when @p program is outside [0,127]. The returned
///          pointer is owned by libsonare and valid for the program lifetime.
const char* sonare_midi_gm_instrument_name(int program);
/// @brief Reverse GM instrument lookup. Returns -1 when @p name is NULL or unknown.
int sonare_midi_gm_program_for_name(const char* name);
/// @brief Returns the GM family name for @p family [0,15], or NULL.
const char* sonare_midi_gm_family_name(int family);
/// @brief Returns the first GM program in @p family [0,15], or -1.
int sonare_midi_gm_family_first_program(int family);
/// @brief Returns the GM2 melodic instrument name for bank LSB + program.
const char* sonare_midi_gm2_instrument_name(int bank_lsb, int program);
/// @brief Returns the GM drum name for note [35,81], or NULL.
const char* sonare_midi_gm_drum_name(int note);
/// @brief Reverse GM drum lookup. Returns -1 when @p name is NULL or unknown.
int sonare_midi_gm_drum_note_for_name(const char* name);
/// @brief Returns the GM2 drum set name for bank LSB, or NULL.
const char* sonare_midi_gm2_drum_set_name(int bank_lsb);
/// @brief Returns the GM2 drum name for bank LSB + note, or NULL.
const char* sonare_midi_gm2_drum_name(int bank_lsb, int note);
/// @brief Returns the standard MIDI CC name for controller [0,127], or NULL.
const char* sonare_midi_cc_name(int controller);
/// @brief Reverse standard MIDI CC lookup. Returns -1 when @p name is NULL or unknown.
int sonare_midi_cc_index_for_name(const char* name);
/// @brief Returns a MIDI 2.0 registered per-note controller name, or NULL.
const char* sonare_midi_per_note_controller_name(int index);
/// @brief Lowers a bank/program selection to MIDI 1.0 bank MSB, bank LSB,
///        program-change events at @p ppq.
SonareError sonare_midi_bank_program(double ppq, uint8_t group, uint8_t channel, int bank_msb,
                                     int bank_lsb, int program, SonareMidiEventPod* out_events,
                                     size_t out_capacity, size_t* out_count);

/// @brief Routes MIDI events through the RT MidiRouter filter/remap/thru logic.
/// @details `out_events` receives at most `out_capacity` events. Overflow due to
///          the router fixed capacity or caller capacity is reported through
///          `out_overflowed` / `out_overflow_count` when those pointers are not
///          NULL. `out_count` is always required.
SonareError sonare_midi_route_events(const SonareMidiEventPod* events, size_t count,
                                     const SonareMidiRouteConfig* config,
                                     SonareMidiEventPod* out_events, size_t out_capacity,
                                     size_t* out_count, int* out_overflowed,
                                     uint32_t* out_overflow_count);

/// @brief Runs MIDI learn over an event stream and returns the learned binding.
/// @details Observes events in order using the native CcMap learn logic,
///          including 14-bit CC pair and RPN/NRPN selector assembly. Returns
///          SONARE_ERROR_INVALID_STATE if no binding is learned.
SonareError sonare_midi_cc_learn(const SonareMidiEventPod* events, size_t count, uint32_t param_id,
                                 float min_value, float max_value, uint8_t min_movement,
                                 SonareMidiCcBinding* out_binding);

/// @brief Converts a CC event to one automation breakpoint using a binding table.
SonareError sonare_midi_cc_to_breakpoint(const SonareMidiCcBinding* bindings, size_t binding_count,
                                         const SonareMidiEventPod* event,
                                         SonareAutomationPoint* out_point);

/// @brief Converts an automation parameter value back to a CC UMP event.
SonareError sonare_midi_param_to_cc(const SonareMidiCcBinding* bindings, size_t binding_count,
                                    uint32_t param_id, float unit_value, uint8_t group, double ppq,
                                    SonareMidiEventPod* out_event);

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

/// @brief Imports an in-memory MIDI 2.0 Clip File ("SMF2CLIP", MIDI Association
///        MIDI Clip File). Unlike SMF, the UMP-based container carries MIDI 2.0
///        channel-voice messages (16-bit velocity, 32-bit CC, per-note /
///        registered controllers, bank-valid Program Change) without loss.
///        Adds one MIDI track + clip. @p out_first_clip_id (optional) receives
///        the added clip id. Malformed input returns an error without crashing.
SonareError sonare_project_import_clip_file(SonareProject* project, const uint8_t* bytes,
                                            size_t len, uint32_t* out_first_clip_id);

/// @brief Exports the project's tempo map + MIDI clips to a MIDI 2.0 Clip File
///        byte buffer (single-clip container). MIDI 2.0-only events are written
///        WITHOUT loss — the reason to prefer this over @ref
///        sonare_project_export_smf when MIDI 2.0 fidelity matters.
/// @param out_bytes Receives a heap byte array (free with @ref sonare_free_bytes).
/// @param out_len   Receives the byte length.
SonareError sonare_project_export_clip_file(const SonareProject* project, uint8_t** out_bytes,
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
///   - arpeggiator_intervals: array of up to 16 integer semitone offsets (one
///     per step), with arpeggiator_step_ppq (positive PPQ between steps) and an
///     optional arpeggiator_gate_ppq (positive gate length, capped to the step;
///     defaults to the full step)
///   - humanize_ppq / humanize_velocity / seed: deterministic jitter controls
///
/// This is an offline/control-plane destructive bake over the clip's event list,
/// not a live RT insert chain. Malformed JSON returns
/// SONARE_ERROR_INVALID_FORMAT; invalid values return
/// SONARE_ERROR_INVALID_PARAMETER.
SonareError sonare_project_bake_midi_fx(SonareProject* project, uint32_t clip_id,
                                        const char* config_json);

/// @brief Backward alias for @ref sonare_project_bake_midi_fx.
/// @deprecated Use @ref sonare_project_bake_midi_fx for destructive clip edits,
///             or RealtimeEngine MIDI-FX APIs for live non-destructive inserts.
SonareError sonare_project_set_midi_fx(SonareProject* project, uint32_t clip_id,
                                       const char* config_json);

/// @brief Pre-flight check for hanging / unmatched notes in a MIDI clip.
///
/// Builds a transient view of the clip's stored events and reports whether every
/// note-on is matched by a note-off (FIFO per channel+note). Useful before
/// bouncing to catch a stuck note that would otherwise sustain. Does not mutate
/// the project. Returns SONARE_ERROR_INVALID_PARAMETER if @p project / @p out is
/// null or @p clip_id is not a MIDI clip.
SonareError sonare_project_validate_midi_notes(const SonareProject* project, uint32_t clip_id,
                                               SonareNotePairValidation* out);

// ============================================================================
// Assist sidecars (opaque module state)
// ============================================================================

/// @brief Adds or updates an opaque assist sidecar by module id + target scope.
///
/// The payload is copied. Existing sidecars with the same module id,
/// target_track_id, region_start_ppq, and region_end_ppq are replaced via an
/// undoable command; otherwise a new sidecar is appended. @p module_id must be a
/// non-empty NUL-terminated string. @p payload may be NULL only when
/// @p payload_len is 0.
SonareError sonare_project_set_assist_sidecar(SonareProject* project, const char* module_id,
                                              uint32_t schema_version, uint32_t target_track_id,
                                              double region_start_ppq, double region_end_ppq,
                                              const uint8_t* payload, size_t payload_len);

/// @brief Number of assist sidecars currently stored on the project.
size_t sonare_project_assist_sidecar_count(const SonareProject* project);

/// @brief Reads one assist sidecar by stable project order.
///
/// On success @p out owns heap memory and must be released with
/// @ref sonare_project_free_assist_sidecar. On failure @p out is zeroed.
SonareError sonare_project_get_assist_sidecar(const SonareProject* project, size_t index,
                                              SonareProjectAssistSidecar* out);

/// @brief Frees heap fields returned by @ref sonare_project_get_assist_sidecar
///        and zeros the struct. NULL is safe.
void sonare_project_free_assist_sidecar(SonareProjectAssistSidecar* sidecar);

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

/// @brief Replaces the project's key annotation stream via an undoable command.
///
/// Existing chord / section / onset annotations are preserved. @p keys may be
/// NULL only when @p count is 0.
SonareError sonare_project_annotate_keys(SonareProject* project,
                                         const SonareProjectKeySegment* keys, size_t count);

/// @brief Replaces the project's chord-symbol annotation stream via an
///        undoable command. @p chords may be NULL only when @p count is 0.
SonareError sonare_project_annotate_chords(SonareProject* project,
                                           const SonareProjectChordSymbol* chords, size_t count);

// ============================================================================
// Memory management (heap byte buffers)
// ============================================================================

/// @brief Frees a heap byte buffer returned by @ref sonare_project_export_smf.
void sonare_free_bytes(uint8_t* ptr);

#ifdef __cplusplus
}
#endif
