#pragma once

/// @file sonare.h
/// @brief Main header for libsonare - Audio analysis library.
/// @details Include this file to access all libsonare functionality.

// Version information
#define SONARE_VERSION_MAJOR 1
#define SONARE_VERSION_MINOR 0
#define SONARE_VERSION_PATCH 0
#define SONARE_VERSION_STRING "1.0.0"

// Utility
#include "util/exception.h"
#include "util/math_utils.h"
#include "util/types.h"

// Core
#include "core/audio.h"
#include "core/audio_io.h"
#include "core/convert.h"
#include "core/fft.h"
#include "core/resample.h"
#include "core/spectrum.h"
#include "core/window.h"

// Filters
#include "filters/chroma.h"
#include "filters/dct.h"
#include "filters/iir.h"
#include "filters/mel.h"

// Features
#include "feature/chroma.h"
#include "feature/mel_spectrogram.h"
#include "feature/onset.h"
#include "feature/spectral.h"

// Effects
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/phase_vocoder.h"
#include "effects/pitch_shift.h"
#include "effects/time_stretch.h"

// Analysis
#include "analysis/beat_analyzer.h"
#include "analysis/boundary_detector.h"
#include "analysis/bpm_analyzer.h"
#include "analysis/chord_analyzer.h"
#include "analysis/chord_templates.h"
#include "analysis/dynamics_analyzer.h"
#include "analysis/key_analyzer.h"
#include "analysis/key_profiles.h"
#include "analysis/melody_analyzer.h"
#include "analysis/music_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "analysis/rhythm_analyzer.h"
#include "analysis/section_analyzer.h"
#include "analysis/timbre_analyzer.h"

// Quick API
#include "quick.h"

namespace sonare {

/// @brief Returns the library version string.
/// @return Version string (e.g., "1.0.0")
inline const char* version() { return SONARE_VERSION_STRING; }

/// @brief Returns the major version number.
inline int version_major() { return SONARE_VERSION_MAJOR; }

/// @brief Returns the minor version number.
inline int version_minor() { return SONARE_VERSION_MINOR; }

/// @brief Returns the patch version number.
inline int version_patch() { return SONARE_VERSION_PATCH; }

}  // namespace sonare
