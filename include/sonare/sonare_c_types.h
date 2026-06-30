#pragma once

#include <stddef.h>
#include <stdint.h>

/// @brief ABI version of the flat analysis / feature POD structs declared in
///        this header (SonareKey, SonareAnalysisResult, the feature result
///        structs, SonareChordDetectionOptions, ...). Bump on ANY layout change
///        to one of those structs. Mirrors the project / engine / voice /
///        acoustic per-subsystem versioning pattern. Exposed at runtime through
///        the aggregate sonare_abi_version() so a prebuilt binding can detect a
///        struct-layout mismatch before exchanging a single byte.
#define SONARE_FEATURE_ABI_VERSION 1u

/// @brief Single aggregate C-ABI version. Encodes the per-subsystem versions so a
///        prebuilt binding linked against a different libsonare can detect a
///        layout/contract mismatch with one comparison. The byte layout is:
///          bits  0..7  : SONARE_FEATURE_ABI_VERSION
///          bits  8..15 : SONARE_PROJECT_ABI_VERSION
///          bits 16..23 : SONARE_VOICE_CHANGER_ABI_VERSION
///          bits 24..31 : SONARE_ACOUSTIC_ABI_VERSION
///        The RT command-queue / engine ABI keeps its own dedicated accessor
///        (sonare_engine_abi_version) because it versions a SharedArrayBuffer
///        record layout independent of these PODs.
/// @note  Defined in the top-level sonare_c.h once every subsystem macro is in
///        scope; do not redefine it here.

#ifdef __cplusplus
extern "C" {
#endif

// Umbrella for the flat analysis / feature / engine C-ABI types. The actual
// declarations live in the sonare_c_types_*.h siblings; this file aggregates
// them in dependency order so every existing consumer keeps the same bare-name
// #include "sonare_c_types.h" and transitively gets the full surface.
//   - enums:     shared enums, opaque handles, key/time-signature/result PODs.
//   - engine:    realtime-engine telemetry / transport / bounce / freeze PODs.
//   - analysis:  feature/effect result PODs + the NativeSynth patch surface.
//   - functions: audio / detect / analyze prototypes + memory/error/version.
#include "sonare_c_types_enums.h"
#include "sonare_c_types_engine.h"
#include "sonare_c_types_analysis.h"
#include "sonare_c_types_functions.h"

#ifdef __cplusplus
}
#endif
