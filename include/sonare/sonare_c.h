#pragma once

/// @file sonare_c.h
/// @brief C API for libsonare.
/// @details Provides a C-compatible interface for use from C code and WASM.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "sonare_c_acoustic.h"
#include "sonare_c_effects.h"
#include "sonare_c_features.h"
#include "sonare_c_mastering.h"
#include "sonare_c_metering.h"
#include "sonare_c_mixing.h"
#include "sonare_c_project.h"
#include "sonare_c_streaming.h"
#include "sonare_c_types.h"

// ============================================================================
// ABI version
// ============================================================================

/// @brief Compile-time aggregate of every per-subsystem ABI version. Packs the
///        subsystem macros into one 32-bit value (see sonare_abi_version()).
///        Byte layout (LSB first):
///          bits  0..7  : SONARE_FEATURE_ABI_VERSION
///          bits  8..15 : SONARE_PROJECT_ABI_VERSION
///          bits 16..23 : SONARE_VOICE_CHANGER_ABI_VERSION
///          bits 24..31 : SONARE_ACOUSTIC_ABI_VERSION
#define SONARE_ABI_VERSION                                          \
  (((uint32_t)(SONARE_FEATURE_ABI_VERSION) & 0xFFu) |               \
   (((uint32_t)(SONARE_PROJECT_ABI_VERSION) & 0xFFu) << 8) |        \
   (((uint32_t)(SONARE_VOICE_CHANGER_ABI_VERSION) & 0xFFu) << 16) | \
   (((uint32_t)(SONARE_ACOUSTIC_ABI_VERSION) & 0xFFu) << 24))

/// @brief Returns the aggregate C-ABI version of the loaded libsonare.
/// @details A prebuilt language binding compares this against its compile-time
///   @ref SONARE_ABI_VERSION to detect a struct-layout / contract mismatch before
///   exchanging any POD across the boundary. The value packs the per-subsystem
///   versions (feature / project / voice-changer / acoustic) as documented on
///   @ref SONARE_ABI_VERSION. The RT command-queue ABI keeps its own dedicated
///   accessor (@ref sonare_engine_abi_version) and is NOT folded in here.
/// @return The packed aggregate ABI version; always equals @ref SONARE_ABI_VERSION
///   at the time this translation unit was compiled.
uint32_t sonare_abi_version(void);

// ============================================================================
// Editing - Scale quantizer (12-TET pitch target picker)
// ============================================================================

/// @brief Quantize a MIDI note number to the nearest pitch class enabled by
///        @p mode_mask, anchored to @p reference_midi.
/// @param root Root pitch class in [0, 11] (0 = C, ... 11 = B).
/// @param mode_mask 12-bit mask. Bit i (LSB = 0) is the i-th pitch class
///                  relative to @p root. E.g. 0b101010110101 = natural major.
/// @param reference_midi MIDI number used as the chromatic origin (e.g. 69 for
///                  A4 = 440 Hz). Pass 0.0f for the library default.
/// @param midi Input MIDI value (may be fractional).
/// @param out_quantized_midi Receives the quantized MIDI value.
SonareError sonare_scale_quantize_midi(int root, uint16_t mode_mask, float reference_midi,
                                       float midi, float* out_quantized_midi);

/// @brief Returns the quantization correction in semitones (quantized - input).
SonareError sonare_scale_correction_semitones(int root, uint16_t mode_mask, float reference_midi,
                                              float midi, float* out_semitones);

/// @brief Returns 1 if @p pitch_class is enabled by @p mode_mask (relative to
///        @p root), 0 otherwise. Returns @c SONARE_ERROR_INVALID_PARAMETER for
///        out-of-range @p pitch_class.
SonareError sonare_scale_pitch_class_enabled(int root, uint16_t mode_mask, int pitch_class,
                                             int* out_enabled);

// ============================================================================
// Core - Resample
// ============================================================================

SonareError sonare_resample(const float* samples, size_t length, int src_sr, int target_sr,
                            float** out, size_t* out_length);

// ============================================================================
// Memory management for result types
// ============================================================================

void sonare_free_stft_result(SonareStftResult* result);
void sonare_free_mel_result(SonareMelResult* result);
void sonare_free_mfcc_result(SonareMfccResult* result);
void sonare_free_chroma_result(SonareChromaResult* result);
void sonare_free_pitch_result(SonarePitchResult* result);
void sonare_free_hpss_result(SonareHpssResult* result);

#ifdef __cplusplus
}
#endif
