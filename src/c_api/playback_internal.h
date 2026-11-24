#pragma once

#include <memory>

#include "playback/renderer.h"

struct SonarePlaybackRenderer {
  std::unique_ptr<sonare::playback::PlaybackRenderer> renderer;
};
