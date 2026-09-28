#pragma once

/// @file default_hrtf.h
/// @brief The embedded default HRTF set (SHRF v1 bytes of
///        `src/playback/default.shrf`): SADIE II subject D1 (KU100), reduced to
///        a 5-degree-azimuth by 15-degree-elevation grid, minimum phase, 128
///        taps, int16. The definition is generated into the build tree by
///        `cmake/EmbedBinary.cmake` for native builds only; WASM packages the
///        file as an asset instead. See NOTICE for attribution.

#include <cstddef>

namespace sonare::playback {

#ifndef __EMSCRIPTEN__
extern const unsigned char kDefaultHrtf[];
extern const std::size_t kDefaultHrtfSize;
#endif

}  // namespace sonare::playback
