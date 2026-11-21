#pragma once

/// @file sonare_c_project_annotate.h
/// @brief Assist-sidecar PODs, MIR annotation surfaces (auto-tempo, key / chord),
///        and heap byte-buffer cleanup for the headless arrangement C ABI.
///        Included via @ref sonare_c_project.h.

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_project_core.h"
#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Heap-owned AssistSidecar snapshot returned by
///        @ref sonare_project_get_assist_sidecar.
///
/// @p module_id is a heap C string (free via
/// @ref sonare_free_project_assist_sidecar). @p payload is a heap byte array
/// of @p payload_len bytes (also freed by the same helper). The core treats the
/// payload as opaque module-owned bytes.
typedef struct {
  char* module_id;           ///< Heap NUL-terminated id of the module that owns the payload.
  uint32_t schema_version;   ///< Module-defined version of the payload format.
  uint32_t target_track_id;  ///< Track the sidecar applies to.
  double region_start_ppq;   ///< Region start in quarter notes.
  double region_end_ppq;     ///< Region end in quarter notes.
  uint8_t* payload;          ///< Heap payload bytes; opaque to the core.
  size_t payload_len;        ///< Size of payload in bytes.
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
  double start_ppq;   ///< Segment start in quarter notes; finite and non-negative.
  double end_ppq;     ///< Segment end in quarter notes; must be after start_ppq.
  uint32_t tonic_pc;  ///< Tonic pitch class 0..11 (C = 0), or 255 for unknown.
  uint32_t mode;      ///< Key mode ordinal; see the list above.
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
/// 7 suspended). @p extensions holds scale degrees (e.g. 7, 9, 13, or 2 / 4 for a
/// suspension), not semitone offsets; at most 32 are accepted. It is copied and
/// may be NULL only when @p extension_count is 0. @p roman_numeral is optional
/// and copied. @ref sonare_chord_symbol_from_analysis fills the root, quality,
/// extensions and slash bass from an analysis chord.
typedef struct {
  double start_ppq;           ///< Chord start in quarter notes; finite and non-negative.
  double end_ppq;             ///< Chord end in quarter notes; must be after start_ppq.
  uint32_t root_pc;           ///< Root pitch class 0..11 (C = 0), or 255 for unknown.
  uint32_t quality;           ///< Chord quality ordinal 0..7; see the list above.
  const uint8_t* extensions;  ///< Extension scale degrees; copied; NULL only when count is 0.
  size_t extension_count;     ///< Number of entries in extensions; at most 32.
  uint32_t slash_bass_pc;     ///< Slash-bass pitch class 0..11, or 255 for none.
  const char* roman_numeral;  ///< Optional NUL-terminated Roman-numeral label; copied; may be NULL.
  int modulation_boundary;    ///< Nonzero when the chord marks a key change.
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
/// @ref sonare_free_project_assist_sidecar. On failure @p out is zeroed.
SonareError sonare_project_get_assist_sidecar(const SonareProject* project, size_t index,
                                              SonareProjectAssistSidecar* out);

/// @brief Frees heap fields returned by @ref sonare_project_get_assist_sidecar
///        and zeros the struct. NULL is safe.
void sonare_free_project_assist_sidecar(SonareProjectAssistSidecar* sidecar);

// ============================================================================
// MIR (offline analysis -> edit commands; deterministic)
// ============================================================================

/// Labels for ranked tempo-octave hypotheses returned by
/// @ref sonare_project_analyze_tempo.
typedef enum SONARE_ENUM_BASE {
  SONARE_TEMPO_CANDIDATE_PRIMARY = 0,
  SONARE_TEMPO_CANDIDATE_HALF = 1,
  SONARE_TEMPO_CANDIDATE_DOUBLE = 2,
} SonareProjectTempoCandidateKind;

/// A ranked tempo hypothesis. The full detected time-signature sequence is
/// applied by @ref sonare_project_auto_tempo; this preview exposes its
/// first segment and total segment count for selection UI.
typedef struct {
  float bpm;         ///< Candidate tempo in beats per minute.
  float confidence;  ///< Relative confidence of the candidate.
  uint32_t kind;     ///< A @ref SonareProjectTempoCandidateKind labelling the octave hypothesis.
  size_t time_signature_count;  ///< Number of detected time-signature segments.
  SonareProjectTimeSignatureSegment
      first_time_signature;  ///< First detected time-signature segment.
} SonareProjectTempoCandidate;

/// Maximum number of ranked tempo candidates emitted by
/// @ref sonare_project_analyze_tempo (primary, half, and double tempo).
/// Callers can allocate this fixed-size array and complete analysis in one
/// call; it is not necessary to make a capacity-0 count query first.
#define SONARE_PROJECT_MAX_TEMPO_CANDIDATES 3u

/// Scoring options for the beat-analysis -> tempo bridge. Start from
/// @ref sonare_project_tempo_options_init; every field is then read literally and checked
/// against the range given beside it.
typedef struct {
  uint32_t struct_size;  ///< sizeof(SonareProjectTempoOptions); set by the init function.
  /// Whether beat tracking may follow a tempo that changes during the take.
  /// Default 0, which keeps the one-tempo-per-take behaviour,
  /// which is the right answer for material recorded to a click. Turn it on for a
  /// performance that accelerates, slows or breathes: with it off the tracker locks
  /// near the take's average and every tempo segment sits near that same average.
  int adaptive_tempo;
  /// Beats of context the local tempo estimate is read over. Used only when
  /// adaptive_tempo is 1. Must be positive. Default 8.
  int tempo_update_interval_beats;
  /// Relative tempo change (as a fraction) at which the bridge closes one tempo
  /// segment and opens the next. Smaller follows the performance more closely
  /// and emits more segments; larger merges more of it into constant stretches.
  /// Must be non-negative and finite. Default 0.02.
  float ramp_threshold;
  /// Whether to emit the half-tempo and double-tempo alternatives alongside the
  /// primary candidate. Default 1; 0 emits the primary only.
  int include_octave_candidates;
} SonareProjectTempoOptions;

/// @brief Writes the tempo-bridge defaults into @p options and sets its
///        @c struct_size; NULL is ignored.
void sonare_project_tempo_options_init(SonareProjectTempoOptions* options);

/// @brief Returns ranked primary/half/double tempo candidates without mutating
///        the project. At most @ref SONARE_PROJECT_MAX_TEMPO_CANDIDATES
///        candidates are emitted, so normal callers should pass an array of
///        that size and avoid a capacity-0 count query followed by a second,
///        identical analysis. A NULL @p candidates with @p capacity 0 remains
///        available when a caller only needs the count.
/// @details The bridge runs on a beat track fitted with one tempo for the whole
///          take unless @p options asks for something else. A NULL @p options
///          is the defaults from @ref sonare_project_tempo_options_init.
SonareError sonare_project_analyze_tempo(const SonareProject* project, const float* audio,
                                         size_t len, int sample_rate,
                                         const SonareProjectTempoOptions* options,
                                         SonareProjectTempoCandidate* candidates, size_t capacity,
                                         size_t* out_count);

/// @brief Detects tempo from a mono audio buffer and installs the requested
///        ranked candidate via undoable edits.
/// @details @p candidate_index is the index returned by
///          @ref sonare_project_analyze_tempo run on the same @p options, so a
///          caller pairs the two rather than mixing option sets. When
///          @p apply_time_signatures is nonzero, the candidate's complete
///          detected meter sequence is also installed. @p out_bpm (optional)
///          receives the selected BPM. A NULL @p options is the defaults. Read
///          the installed map back with @ref sonare_project_tempo_segment_count
///          and @ref sonare_project_tempo_segment_by_index.
SonareError sonare_project_auto_tempo(SonareProject* project, const float* audio, size_t len,
                                      int sample_rate, const SonareProjectTempoOptions* options,
                                      size_t candidate_index, uint8_t apply_time_signatures,
                                      float* out_bpm);

/// Options for @ref sonare_project_snap_to_grid. Start from
/// @ref sonare_project_snap_to_grid_options_init.
typedef struct {
  uint32_t struct_size;  ///< sizeof(SonareProjectSnapToGridOptions); set by the init function.
  /// Target grid: 0 snaps to bar lines, 1 to beat lines, and values >= 2 to
  /// that many equal subdivisions per beat (for example, 4 is a sixteenth-note
  /// grid in 4/4). Default 1.
  int division;
} SonareProjectSnapToGridOptions;

/// @brief Writes the snap defaults (beat grid) into @p options and sets its
///        @c struct_size; NULL is ignored.
void sonare_project_snap_to_grid_options_init(SonareProjectSnapToGridOptions* options);

/// @brief Snaps a PPQ coordinate to a bar, beat, or beat subdivision of the
///        project's grid at that position.
/// @details @p strength is in [0,1] (0 = no snap, 1 = exact line). A NULL
///          @p options snaps to the beat grid.
SonareError sonare_project_snap_to_grid(const SonareProject* project, double ppq, double strength,
                                        const SonareProjectSnapToGridOptions* options,
                                        double* out_ppq);

/// @brief Replaces the project's key annotation stream via an undoable command.
///
/// Existing chord / section / onset annotations are preserved. @p keys may be
/// NULL only when @p count is 0.
SonareError sonare_project_annotate_keys(SonareProject* project,
                                         const SonareProjectKeySegment* keys, size_t count);

/// @brief Replaces the project's chord-symbol annotation stream via an
///        undoable command. @p chords may be NULL only when @p count is 0.
/// @return ::SONARE_ERROR_INVALID_PARAMETER when a span does not start at a
///   finite, non-negative PPQ and end after it, a pitch class is neither 0..11
///   nor 255, a quality is not in 0..7, or a chord carries more than 32
///   extension scale degrees (or a NULL @c extensions with a non-zero count);
///   nothing is applied in that case.
SonareError sonare_project_annotate_chords(SonareProject* project,
                                           const SonareProjectChordSymbol* chords, size_t count);

/// @brief Converts an analysis chord into the fields of a
///        @ref SonareProjectChordSymbol.
/// @details The analysis vocabulary and the annotation vocabulary are numbered
///   differently and the annotation one is coarser: an analysis quality becomes
///   an annotation family plus extension scale degrees (Dominant9 -> dominant
///   with 7, 9), so an ordinal offset mislabels most of them. This is the one
///   published conversion, the same the library uses when it builds a harmonic
///   timeline from analysis. The root is kept as given, including for
///   ::SONARE_CHORD_UNKNOWN; a @p bass equal to @p root is no slash bass. Times
///   are not converted: the caller places the result on the timeline.
/// @param root Chord root, 0..11.
/// @param quality Analysis chord quality, below ::SONARE_CHORD_QUALITY_COUNT.
/// @param bass Bass pitch class, 0..11; pass @p root for a chord without one.
/// @param out_root_pc Receives the annotation root (0..11).
/// @param out_quality Receives the annotation quality ordinal (0..7).
/// @param out_extensions Caller-owned array of @p extension_capacity scale
///   degrees; may be NULL only when @p extension_capacity is 0. A capacity of 32,
///   the annotation's own cap, always suffices.
/// @param out_extension_count Receives the number of degrees the chord carries,
///   written on every return, including when @p extension_capacity is too small.
/// @param out_slash_bass_pc Receives the slash bass (0..11), or 255 for none.
/// @return ::SONARE_ERROR_INVALID_PARAMETER when a pitch class is not in 0..11,
///   @p quality is not a ::SonareChordQuality, a required out-parameter is NULL,
///   or @p extension_capacity is below the count, which is then already written.
///   Every input is checked before it is read as an enumerator.
SonareError sonare_chord_symbol_from_analysis(SonarePitchClass root, SonareChordQuality quality,
                                              SonarePitchClass bass, uint32_t* out_root_pc,
                                              uint32_t* out_quality, uint8_t* out_extensions,
                                              size_t extension_capacity,
                                              size_t* out_extension_count,
                                              uint32_t* out_slash_bass_pc);

/// @brief Converts an analysis key mode into the @c mode of a
///        @ref SonareProjectKeySegment.
/// @details The annotation numbering reserves 0 for an unknown mode, so every
///   analysis mode lands one ordinal higher; this is the conversion the library
///   uses itself.
/// @param mode Analysis mode, 0..6.
/// @param out_mode Receives the annotation KeyMode ordinal (1..7).
/// @return ::SONARE_ERROR_INVALID_PARAMETER when @p mode is not in 0..6 or
///   @p out_mode is NULL.
SonareError sonare_key_mode_from_analysis(SonareMode mode, uint32_t* out_mode);

// ============================================================================
// Memory management (heap byte buffers)
// ============================================================================

/// @brief Frees a heap byte buffer returned by @ref sonare_project_export_smf.
void sonare_free_bytes(uint8_t* ptr);

#ifdef __cplusplus
}
#endif
