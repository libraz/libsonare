/// @file bowed_string_force_window_test.cpp
/// @brief The bow-force window: where multiple slip begins as a function of bow
///        position, swept over the friction law's two branches. Cases carry
///        [bowed][window] and are invoked as an AND, because [window] alone
///        reaches other tests.

#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "midi/bowed_string_probe.h"
#include "midi/synth/bowed_string_voice.h"
#include "midi/synth/gm_fallback_map.h"
