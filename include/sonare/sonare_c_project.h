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

// This header is the single public entry point for the headless arrangement /
// DAW project C ABI; every consumer includes <sonare/sonare_c_project.h>. Its
// body is split into domain sub-headers below, included in dependency order
// (shared descriptors + lifecycle first, then the function surfaces). Each
// sub-header is standalone-safe (its own #pragma once / includes / extern "C").
#include "sonare_c_project_core.h"
#include "sonare_c_project_instruments.h"
#include "sonare_c_project_edit.h"
#include "sonare_c_project_midi.h"
#include "sonare_c_project_annotate.h"
