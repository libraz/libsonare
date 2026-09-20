/// @file bowed_string_onset_test.cpp
/// @brief Bowed-string note onset: how quickly Helmholtz motion establishes and
///        what the seeded initial condition does to the amplitude envelope.
///        Cases carry [bowed][onset] and are invoked as an AND, because
///        [onset] alone reaches other engines' tests.

#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "midi/bowed_string_probe.h"
#include "midi/synth/bowed_string_voice.h"
#include "midi/synth/gm_fallback_map.h"
#include "support/golden_hash.h"
