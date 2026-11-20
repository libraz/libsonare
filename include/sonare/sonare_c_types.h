#pragma once

#include <stddef.h>
#include <stdint.h>

/// @brief ABI version of the general C function contracts and flat analysis /
///        feature POD structs declared in this header (SonareKey,
///        SonareAnalysisResult, the feature result structs,
///        SonareChordDetectionOptions, ...). Bump on ANY layout change
///        to one of those structs or a shipped general C function signature.
///        Mirrors the project / engine / voice / acoustic per-subsystem
///        versioning pattern. Exposed at runtime through
///        the aggregate sonare_abi_version() so a prebuilt binding can detect a
///        layout or function-contract mismatch before calling into the library.
#define SONARE_FEATURE_ABI_VERSION 8u

/// @brief ABI version of the realtime command-queue record layout, as returned
///        by sonare_engine_abi_version(). Not folded into SONARE_ABI_VERSION.
#define SONARE_ENGINE_ABI_VERSION 3u

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

/// @brief Caller-filled structs: every struct a caller fills that has a field with
///        a library default starts with @c uint32_t struct_size and has a
///        @c void sonare_<struct>_init(T*) that writes sizeof(T) and every
///        default (a NULL pointer is ignored). Start from it rather than zeroing:
///        an entry refuses a struct_size below sizeof(T) by name. A plain data
///        record whose every field is the caller's own value (an event, a point,
///        a segment) carries neither. Library-filled results carry no version.
///
/// @brief Optional floats: a float field or argument documented as optional is
///        "not given" when it holds NaN (NAN from <math.h>). Its _init function
///        seeds NaN, the library resolves the default, and every other value is
///        checked against the field's domain, so 0 or a negative is never read
///        as absence. An infinity is refused like any out-of-domain value.

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
#include "sonare_c_types_analysis.h"
#include "sonare_c_types_engine.h"
#include "sonare_c_types_enums.h"
#include "sonare_c_types_functions.h"

#ifdef __cplusplus
}
#endif
