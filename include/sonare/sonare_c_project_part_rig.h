#pragma once

/// @file sonare_c_project_part_rig.h
/// @brief Per-part rig selection for the headless arrangement C ABI. Included
///        via @ref sonare_c_project.h.

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_project_core.h"
#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Where a part's rig comes from.
typedef enum {
  /// The player's default rig for the part's program (pass-through when none).
  SONARE_PART_RIG_BANK = 0,
  /// No rig: the voice's direct output.
  SONARE_PART_RIG_NONE = 1,
  /// An explicit chain of inserts, replacing the default rig.
  SONARE_PART_RIG_CHAIN = 2
} SonarePartRigMode;

/// @brief Part number addressing the destination default instead of one part.
#define SONARE_PART_RIG_ALL_PARTS 0xFFu

/// @brief Sets (inserts or replaces) the rig of a part, or of the destination
///        default, for a MIDI destination. Undoable.
///
/// The destination need not be referenced by a track; the entry is kept and
/// serialized regardless. The project serializes at schema version 5 while at
/// least one entry exists.
///
/// @param destination_id MIDI destination id (see
///        @ref sonare_project_set_track_midi_destination).
/// @param part GS part slot 0-15, or @ref SONARE_PART_RIG_ALL_PARTS.
/// @param mode A @ref SonarePartRigMode value.
/// @param inserts_json Required for @c SONARE_PART_RIG_CHAIN, NULL otherwise: a
///        JSON array of 1-8 objects `{"processor": "<name>", "params": "<JSON
///        object as a string>"}` (@c params defaults to "{}"). Every processor
///        must be a known insert, accept its params, and report at most 256
///        samples of latency at 48 kHz.
/// @return SONARE_ERROR_INVALID_FORMAT when @p inserts_json does not parse or is
///         not an array of objects with string @c processor / @c params;
///         SONARE_ERROR_INVALID_PARAMETER for a part out of range, an unknown
///         mode, inserts that do not match the mode, or an invalid chain
///         (stage count, unknown processor, bad params, excess latency);
///         SONARE_ERROR_NOT_SUPPORTED for a chain in a build without mastering.
SonareError sonare_project_set_part_rig(SonareProject* project, uint32_t destination_id,
                                        uint8_t part, int mode, const char* inserts_json);

/// @brief Reads the rig entry of a part, or of the destination default.
///
/// @param out_mode Receives the entry's @ref SonarePartRigMode value, or
///        @c SONARE_PART_RIG_BANK when no entry exists.
/// @param out_inserts_json Optional. Receives a heap C string in the
///        @ref sonare_project_set_part_rig inserts format for a chain entry and
///        NULL otherwise; release with @ref sonare_free_string.
/// @param out_present Receives 1 when an entry exists and 0 when none does.
SonareError sonare_project_get_part_rig(const SonareProject* project, uint32_t destination_id,
                                        uint8_t part, int* out_mode, char** out_inserts_json,
                                        int* out_present);

/// @brief Removes the rig entry of a part, or of the destination default.
///        Removing an absent entry succeeds and records no undo step.
SonareError sonare_project_clear_part_rig(SonareProject* project, uint32_t destination_id,
                                          uint8_t part);

#ifdef __cplusplus
}
#endif
