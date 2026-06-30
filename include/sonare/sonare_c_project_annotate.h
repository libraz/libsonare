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
