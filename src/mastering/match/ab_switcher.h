#pragma once

/// @file ab_switcher.h
/// @brief A/B audition helper with equal-length crossfade.

#include "core/audio.h"

namespace sonare::mastering::match {

enum class ABSelection { A, B };

Audio ab_switch(const Audio& a, const Audio& b, ABSelection selection);
Audio ab_crossfade(const Audio& a, const Audio& b, float mix);

}  // namespace sonare::mastering::match
