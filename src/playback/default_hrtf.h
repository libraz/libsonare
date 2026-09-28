#pragma once

/// @file default_hrtf.h
/// @brief The embedded default HRTF set (SHRF v1 bytes of
///        `src/playback/default.shrf`). The definition is generated into the build
///        tree by `cmake/EmbedBinary.cmake` for native builds only; WASM
///        packages the file as an asset instead.

#include <cstddef>

namespace sonare::playback {

#ifndef __EMSCRIPTEN__
extern const unsigned char kDefaultHrtf[];
extern const std::size_t kDefaultHrtfSize;
#endif

}  // namespace sonare::playback
